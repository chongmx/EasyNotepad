#include "enp/model/BinaryInfo.h"

#include "BinaryInternal.h"

#include "llvm/Demangle/Demangle.h"

#include <cctype>
#include <unordered_map>

namespace enp::model::bin {

using namespace detail;

// ---- Reading -------------------------------------------------------------------------------------

namespace detail {

namespace {
constexpr size_t kBlock = 256 * 1024; // read-ahead for small reads
constexpr size_t kSmall = 64 * 1024;
} // namespace

bool Source::read(std::uint64_t offset, size_t size, std::string& out) const
{
    out.clear();
    if (offset >= size_)
        return false;
    size = size_t(std::min<std::uint64_t>(size, size_ - offset));
    const std::uint64_t at = base_ + offset;
    if (size > kSmall)
        return cache_->reader(at, size, out) && !out.empty();
    Cache& c = *cache_;
    if (c.at == kNone || at < c.at || at + size > c.at + c.block.size()) {
        c.at = at;
        if (!c.reader(at, size_t(std::min<std::uint64_t>(kBlock, std::max<std::uint64_t>(size, c.fileSize - std::min(at, c.fileSize)))), c.block)) {
            c.at = kNone;
            c.block.clear();
            return false;
        }
    }
    const size_t from = size_t(at - c.at);
    out.assign(c.block, from, std::min(size, c.block.size() - std::min(from, c.block.size())));
    return !out.empty();
}

Text Builder::add(std::string_view s)
{
    s = s.substr(0, kMaxName);
    if (image_.strings.size() + s.size() >= UINT32_MAX) {
        image_.truncated = true;
        return {};
    }
    const std::string& pool = image_.strings;
    if (!s.empty() && s.data() >= pool.data() && s.data() < pool.data() + pool.size()) // a piece of the pool itself
        return Text{std::uint32_t(s.data() - pool.data()), std::uint32_t(s.size())};
    Text t{std::uint32_t(image_.strings.size()), std::uint32_t(s.size())};
    image_.strings.append(s);
    return t;
}

Text Builder::intern(std::string_view s)
{
    auto it = interned_.find(std::string(s));
    if (it != interned_.end())
        return it->second;
    const Text t = add(s);
    interned_.emplace(std::string(s), t);
    return t;
}

void Builder::heading(std::string_view title)
{
    Property p;
    p.key = intern(title);
    p.heading = true;
    image_.overview.push_back(p);
}

void Builder::property(std::string_view key, std::string_view value)
{
    if (value.empty())
        return;
    const Text v = add(value); // first: interning the key may grow the pool under `value`
    image_.overview.push_back(Property{intern(key), v, false});
}

Symbol* Builder::symbol(std::string_view raw, Side side)
{
    if (image_.symbols.size() >= kMaxSymbols) {
        image_.truncated = true;
        return nullptr;
    }
    Symbol& s = image_.symbols.emplace_back();
    s.side = side;
    s.name = add(raw);
    s.shown = s.name;
    Mangling scheme = Mangling::None;
    std::string shown = demangle(raw, &scheme);
    s.mangling = scheme;
    if (scheme != Mangling::None)
        ++image_.mangled;
    if (!shown.empty()) {
        s.shown = add(shown);
        s.demangled = true;
        ++image_.demangledCount;
    }
    return &s;
}

std::uint32_t Builder::library(std::string_view name)
{
    for (size_t i = 0; i < image_.libraries.size(); ++i)
        if (image_.text(image_.libraries[i].name) == name)
            return std::uint32_t(i);
    Library l;
    l.name = intern(name);
    image_.libraries.push_back(l);
    return std::uint32_t(image_.libraries.size() - 1);
}

ListNode& Builder::list(std::string_view title, ListType type, int depth)
{
    ListNode& n = image_.lists.emplace_back();
    n.title = intern(title);
    n.type = type;
    n.depth = std::uint8_t(depth);
    return n;
}

void Builder::buildLists(bool linkedImage)
{
    Image& img = image_;
    bool locals = false, imports = false, unlinked = false;
    for (const Symbol& s : img.symbols) {
        locals = locals || s.side == Side::Local;
        imports = imports || s.side == Side::Import;
        unlinked = unlinked || (s.side == Side::Import && s.library == kNoIndex);
    }
    list("Overview", ListType::Overview, 0);
    if (!img.sections.empty())
        list("Sections", ListType::Sections, 0).count = std::uint32_t(img.sections.size());
    if (img.format == Format::Archive && !img.members.empty()) {
        list("Members", ListType::Members, 0).count = std::uint32_t(img.members.size());
        // Members can share a name (every entry of a Windows import library is named after its
        // DLL): those are told apart by their first symbol.
        std::vector<std::uint8_t> shared(img.members.size(), 0);
        {
            std::unordered_map<std::string_view, std::uint32_t> names; // views into the pool: gone before it grows below
            for (const Member& m : img.members)
                ++names[img.text(m.name)];
            for (size_t i = 0; i < img.members.size(); ++i)
                shared[i] = names[img.text(img.members[i].name)] > 1;
        }
        std::vector<std::uint32_t> first(img.members.size(), kNoIndex);
        for (std::uint32_t k = 0; k < img.symbols.size(); ++k)
            if (const std::uint32_t m = img.symbols[k].member; m < first.size() && first[m] == kNoIndex)
                first[m] = k;
        for (size_t i = 0; i < img.members.size(); ++i) {
            ListNode& n = list({}, ListType::Symbols, 1);
            n.title = img.members[i].name;
            if (shared[i] && first[i] != kNoIndex)
                n.title = add(std::string(img.text(img.members[i].name)) + ": " + std::string(img.text(img.symbols[first[i]].shown)));
            n.member = std::uint32_t(i);
            n.count = img.members[i].symbols;
        }
    }
    auto symbols = [&](const char* title, Side side, KindFilter kinds) -> ListNode& {
        ListNode& n = list(title, ListType::Symbols, 0);
        n.sideFilter = true;
        n.side = side;
        n.kinds = kinds;
        return n;
    };
    symbols(linkedImage ? "Exported functions" : "Defined functions", Side::Export, KindFilter::Functions);
    symbols(linkedImage ? "Exported data" : "Defined data", Side::Export, KindFilter::Data);
    if (imports) {
        symbols(linkedImage ? "Imports" : "Undefined symbols", Side::Import, KindFilter::Any);
        if (linkedImage) {
            for (size_t i = 0; i < img.libraries.size(); ++i) {
                if (img.libraries[i].symbols == 0)
                    continue;
                ListNode& n = list({}, ListType::Symbols, 1);
                n.title = img.libraries[i].name;
                if (img.libraries[i].delayed)
                    n.title = add(std::string(img.text(img.libraries[i].name)) + " (delay-loaded)");
                n.library = std::uint32_t(i);
                n.count = img.libraries[i].symbols;
            }
            if (unlinked && !img.libraries.empty()) {
                ListNode& n = list("Any library (not versioned)", ListType::Symbols, 1);
                n.sideFilter = true;
                n.side = Side::Import;
                n.noLibrary = true;
            }
        }
    }
    if (locals)
        symbols(img.format == Format::Pe ? "Symbol table" : "Local symbols", Side::Local, KindFilter::Any);
    if (!img.libraries.empty()) {
        ListNode& deps = list(img.format == Format::Archive ? "DLLs" : "Dependencies", ListType::Libraries, 0);
        deps.count = std::uint32_t(img.libraries.size());
        if (img.format == Format::Archive)
            for (size_t i = 0; i < img.libraries.size(); ++i) {
                ListNode& n = list({}, ListType::Symbols, 1);
                n.title = img.libraries[i].name;
                n.library = std::uint32_t(i);
                n.count = img.libraries[i].symbols;
            }
    }
    countLists();
}

void Builder::countLists()
{
    Image& img = image_;
    for (ListNode& n : img.lists) {
        if (n.type != ListType::Symbols || n.library != kNoIndex || n.member != kNoIndex)
            continue; // groups were counted as their symbols were added
        n.count = 0;
    }
    for (const Symbol& s : img.symbols)
        for (ListNode& n : img.lists)
            if (n.type == ListType::Symbols && n.library == kNoIndex && n.member == kNoIndex && Image::matches(n, s))
                ++n.count;
}

std::string hex(std::uint64_t v)
{
    char buf[24];
    std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string number(std::uint64_t v)
{
    std::string digits = std::to_string(v), out;
    for (size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0)
            out += ',';
        out += digits[i];
    }
    return out;
}

} // namespace detail

bool Image::matches(const ListNode& node, const Symbol& s)
{
    if (node.sideFilter && s.side != node.side)
        return false;
    if (node.library != kNoIndex && s.library != node.library)
        return false;
    if (node.member != kNoIndex && s.member != node.member)
        return false;
    if (node.noLibrary && s.library != kNoIndex)
        return false;
    switch (node.kinds) {
        case KindFilter::Functions: return s.kind == SymbolKind::Function;
        case KindFilter::Data: return s.kind != SymbolKind::Function;
        case KindFilter::Any: return true;
    }
    return true;
}

// ---- Demangling ----------------------------------------------------------------------------------

namespace {

// The part of a name that carries the mangling: without a "__imp_" prefix (Windows import
// pointers), a leading '.' (PowerPC function descriptors, clang's local names) and an ELF version
// ("@GLIBC_2.2.5", "@@VERS_1"; not for MSVC names, which use '@' themselves).
struct Parts {
    std::string_view prefix, core, suffix;
};

Parts split(std::string_view name)
{
    Parts p;
    size_t start = 0;
    if (name.rfind("__imp_", 0) == 0)
        start = 6;
    else if (!name.empty() && name[0] == '.')
        start = 1;
    p.prefix = name.substr(0, start);
    p.core = name.substr(start);
    if (!p.core.empty() && p.core[0] != '?')
        if (const size_t at = p.core.find('@'); at != std::string_view::npos && at > 0) {
            p.suffix = p.core.substr(at);
            p.core = p.core.substr(0, at);
        }
    return p;
}

bool isItanium(std::string_view core)
{
    // "_Z", or with the platform's extra underscore ("__Z": Mach-O, 32-bit Windows), or a block ("___Z").
    size_t underscores = 0;
    while (underscores < core.size() && core[underscores] == '_')
        ++underscores;
    return underscores >= 1 && underscores <= 4 && underscores + 1 < core.size() && core[underscores] == 'Z';
}

// Rust's legacy mangling is Itanium with a hash: "...17h0123456789abcdefE".
bool isLegacyRust(std::string_view core)
{
    if (core.size() < 22 || core.back() != 'E')
        return false;
    const std::string_view tail = core.substr(core.size() - 20, 19); // "17h" and 16 hex digits, before the E
    if (tail.substr(0, 3) != "17h")
        return false;
    for (char c : tail.substr(3))
        if (!std::isxdigit(static_cast<unsigned char>(c)))
            return false;
    return true;
}

} // namespace

Mangling manglingOf(std::string_view name)
{
    const Parts p = split(name);
    if (p.core.empty())
        return Mangling::None;
    if (p.core[0] == '?')
        return Mangling::Microsoft;
    if (isItanium(p.core))
        return isLegacyRust(p.core) ? Mangling::Rust : Mangling::Itanium;
    if (p.core.size() > 2 && p.core[0] == '_' && p.core[1] == 'R' && (std::isupper(static_cast<unsigned char>(p.core[2])) ||
                                                                       std::isdigit(static_cast<unsigned char>(p.core[2]))))
        return Mangling::Rust;
    if (p.core.size() > 2 && p.core[0] == '_' && p.core[1] == 'D' && std::isdigit(static_cast<unsigned char>(p.core[2])))
        return Mangling::D;
    return Mangling::None;
}

std::string demangle(std::string_view name, Mangling* scheme)
{
    Mangling m = manglingOf(name);
    if (scheme)
        *scheme = Mangling::None;
    if (m == Mangling::None || name.size() > kMaxName)
        return {};
    const Parts p = split(name);
    std::string out;
    bool ok = false;
    if (m == Mangling::Microsoft) {
        out = llvm::demangle(p.core);
        ok = out != p.core;
    } else {
        std::string_view core = p.core;
        if (m != Mangling::D && core.size() > 3 && core[0] == '_' && core[1] == '_' && core[2] == 'Z')
            core.remove_prefix(1); // "__Z" -> "_Z" (the platform's underscore); "___Z" is a block and stays
        ok = llvm::nonMicrosoftDemangle(core, out, false);
    }
    // "_R..." and "_D..." are also ordinary C names ("_RTC_CheckEsp", "_Dmain" is fine but "_D2..." is
    // rare): they count as mangled only when they demangle. "_Z" and "?" are always mangled.
    if (!ok) {
        // A name that only starts like Itanium ("_ZombifyActCtx@4": 32-bit Windows puts '_' before C
        // names) is not counted; "_Z" then an upper-case letter or a digit is mangled all the same.
        size_t z = 0;
        while (z < p.core.size() && p.core[z] == '_')
            ++z;
        const unsigned char after = z + 1 < p.core.size() ? static_cast<unsigned char>(p.core[z + 1]) : 0;
        const bool itaniumShape = std::isupper(after) || std::isdigit(after);
        if (scheme && (m == Mangling::Microsoft || ((m == Mangling::Itanium || (m == Mangling::Rust && isItanium(p.core))) && itaniumShape)))
            *scheme = m;
        return {};
    }
    if (scheme)
        *scheme = m;
    std::string shown;
    shown.reserve(p.prefix.size() + out.size() + p.suffix.size() + 1);
    shown.append(p.prefix);
    if (!p.prefix.empty() && p.prefix != ".")
        shown += ' ';
    shown += out;
    shown.append(p.suffix);
    return shown;
}

const char* manglingName(Mangling m)
{
    switch (m) {
        case Mangling::Itanium: return "C++ (Itanium)";
        case Mangling::Microsoft: return "C++ (MSVC)";
        case Mangling::Rust: return "Rust";
        case Mangling::D: return "D";
        case Mangling::None: break;
    }
    return "";
}

// ---- Archives ------------------------------------------------------------------------------------

namespace {

bool isElf(std::string_view head) { return head.size() >= 20 && head.substr(0, 4) == "\x7F" "ELF"; }
bool isArchive(std::string_view head) { return head.substr(0, 8) == "!<arch>\n" || head.substr(0, 8) == "!<thin>\n"; }
bool isPe(std::string_view head) { return head.size() >= 64 && head[0] == 'M' && head[1] == 'Z'; }
bool isShortImport(std::string_view head)
{
    const Bytes b{head};
    return head.size() >= 20 && b.u16(0) == 0 && b.u16(2) == 0xFFFF && b.u16(4) == 0;
}

std::string trimRight(std::string_view s)
{
    while (!s.empty() && (s.back() == ' ' || s.back() == '\0'))
        s.remove_suffix(1);
    return std::string(s);
}

std::uint64_t decimal(std::string_view s)
{
    std::uint64_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9')
            break;
        v = v * 10 + std::uint64_t(c - '0');
    }
    return v;
}

// A Windows import library's short entry: one symbol, the DLL it comes from, and how to bind it.
std::string readImportEntry(const Source& src, Builder& b, std::uint32_t member)
{
    std::string data;
    if (!src.read(0, size_t(std::min<std::uint64_t>(src.size(), 64 * 1024)), data))
        return "import entry";
    const Bytes h{data};
    const std::uint16_t machine = h.u16(6);
    const std::uint16_t ordinalOrHint = h.u16(16);
    const std::uint16_t info = h.u16(18);
    const int type = info & 3, nameType = (info >> 2) & 7;
    const std::string_view name = h.cstr(20);
    const std::string_view dll = h.cstr(20 + name.size() + 1);
    if (Symbol* s = b.symbol(name, Side::Export)) {
        s->kind = type == 0 ? SymbolKind::Function : SymbolKind::Data;
        s->member = member;
        s->binding = Binding::Global;
        s->library = b.library(dll);
        s->detail = b.intern(dll);
        ++b.image().libraries[s->library].symbols;
        if (nameType == 0)
            s->ordinal = ordinalOrHint;
        else
            s->hint = ordinalOrHint;
    }
    return std::string("import entry, ") + coffMachine(machine);
}

std::string readArchive(const Source& src, Builder& b)
{
    Image& img = b.image();
    std::string head;
    src.read(0, 8, head);
    const bool thin = head == "!<thin>\n";
    std::string longNames, header, data;
    std::vector<std::pair<std::uint64_t, std::uint32_t>> memberAt; // header offset -> member index
    std::vector<std::pair<std::uint64_t, std::string>> index;      // the symbol index: header offset, name
    bool linkerMember = false, windows = false;
    std::uint64_t indexEntries = 0;
    std::uint64_t pos = 8;
    while (pos + 60 <= src.size()) {
        if (!src.read(pos, 60, header) || header.size() < 60)
            break;
        if (header[58] != '`' || header[59] != '\n') {
            img.error = "the member header at offset " + number(pos) + " is damaged; the members before it are listed";
            break;
        }
        std::string name = trimRight(std::string_view(header).substr(0, 16));
        std::uint64_t size = decimal(std::string_view(header).substr(48, 10));
        std::uint64_t at = pos + 60;
        const std::uint64_t next = at + size + ((at + size) & 1);
        if (name == "/" || name == "/SYM64/") {
            // The symbol index (GNU, and the first linker member of a Windows .lib): big-endian
            // count, member offsets, then the names. A Windows .lib has a second one, little-endian.
            if (!linkerMember && src.sub(at, size).read(0, size_t(std::min<std::uint64_t>(size, kMaxTable)), data)) {
                const bool wide = name == "/SYM64/";
                const Bytes t{data, true};
                const size_t w = wide ? 8 : 4;
                const std::uint64_t count = std::min<std::uint64_t>(t.word(0, wide), (data.size() - std::min(data.size(), w)) / w);
                size_t str = w + size_t(count) * w;
                for (std::uint64_t i = 0; i < count && str < data.size() && index.size() < kMaxSymbols; ++i) {
                    const std::string_view sym = t.cstr(str);
                    index.emplace_back(t.word(w + size_t(i) * w, wide), std::string(sym));
                    str += sym.size() + 1;
                }
                indexEntries = index.size();
                linkerMember = true;
            } else if (linkerMember) {
                windows = true;
            }
        } else if (name == "//") {
            src.sub(at, size).read(0, size_t(std::min<std::uint64_t>(size, kMaxTable)), longNames);
        } else if (name.rfind("/<", 0) == 0 || name.rfind("__.SYMDEF", 0) == 0) {
            // other indexes (/<ECSYMBOLS>/, /<XFGHASHMAP>/; the BSD __.SYMDEF): not needed
        } else {
            if (name.rfind("#1/", 0) == 0) { // BSD: the name follows the header
                const std::uint64_t length = std::min<std::uint64_t>(decimal(name.substr(3)), size);
                src.sub(at, length).read(0, size_t(length), data);
                name = trimRight(data);
                at += length;
                size -= length;
            } else if (name.size() > 1 && name[0] == '/' && std::isdigit(static_cast<unsigned char>(name[1]))) {
                const size_t from = size_t(decimal(name.substr(1)));
                if (from < longNames.size()) {
                    size_t end = longNames.find_first_of(std::string_view("\n\0", 2), from);
                    if (end == std::string::npos)
                        end = longNames.size();
                    name = longNames.substr(from, end - from);
                }
                if (!name.empty() && name.back() == '/')
                    name.pop_back();
            } else if (!name.empty() && name.back() == '/') {
                name.pop_back();
            }
            if (img.members.size() >= kMaxMembers) {
                img.truncated = true;
                break;
            }
            const std::uint32_t m = std::uint32_t(img.members.size());
            Member& member = img.members.emplace_back();
            member.name = b.add(name);
            member.offset = at;
            member.size = size;
            memberAt.emplace_back(pos, m);
            const size_t before = img.symbols.size();
            std::string format;
            if (thin) {
                format = "file outside the archive (thin archive)";
            } else {
                const Source body = src.sub(at, size);
                body.read(0, 64, data);
                bool bigObj = false;
                if (isElf(data))
                    format = readElf(body, b, m);
                else if (isShortImport(data))
                    format = readImportEntry(body, b, m), windows = true;
                else if (isCoffObject(data, &bigObj))
                    format = readCoff(body, b, m, bigObj);
                else if (data.rfind("BC\xC0\xDE", 0) == 0 || data.rfind("\xDE\xC0\x17\x0B", 0) == 0)
                    format = "LLVM bitcode";
                else if (data.rfind("\xCF\xFA\xED\xFE", 0) == 0 || data.rfind("\xCE\xFA\xED\xFE", 0) == 0)
                    format = "Mach-O object";
                else
                    format = "data";
            }
            img.members[m].format = b.intern(format);
            img.members[m].symbols = std::uint32_t(img.symbols.size() - before);
        }
        if (next <= pos)
            break;
        pos = next;
    }

    // Members that could not be read (thin archives, bitcode, other formats): their names from the index.
    std::sort(memberAt.begin(), memberAt.end());
    for (const auto& [offset, symbolName] : index) {
        auto it = std::lower_bound(memberAt.begin(), memberAt.end(), std::make_pair(offset, std::uint32_t(0)));
        if (it == memberAt.end() || it->first != offset || (img.members[it->second].symbols != 0 && !thin))
            continue;
        const std::string_view format = img.text(img.members[it->second].format);
        if (!thin && format != "LLVM bitcode" && format != "Mach-O object" && format != "data")
            continue;
        if (Symbol* s = b.symbol(symbolName, Side::Export)) {
            s->member = it->second;
            s->binding = Binding::Global;
            s->detail = b.intern("from the archive's symbol index");
        }
    }
    for (const Symbol& s : img.symbols)
        if (s.member != kNoIndex && s.detail.length && img.text(s.detail) == "from the archive's symbol index")
            ++img.members[s.member].symbols;

    // Overview.
    size_t objects = 0, imports = 0;
    for (const Member& m : img.members) {
        const std::string_view f = img.text(m.format);
        imports += f.rfind("import entry", 0) == 0;
        objects += f.find("object") != std::string_view::npos;
    }
    const bool importLibrary = imports > 0 && imports * 2 >= img.members.size();
    img.summary = importLibrary ? "Windows import library" : windows ? "Windows static library" : thin ? "thin static library" : "static library";
    b.heading("File");
    b.property("Format", thin ? "ar archive, thin (members are separate files)" : "ar archive");
    b.property("Type", importLibrary ? "Import library (stands for DLLs)" : "Static library");
    b.property("Style", windows ? "Windows (.lib)" : longNames.empty() && !thin ? "System V / GNU" : "GNU");
    b.property("Members", number(img.members.size()));
    if (objects)
        b.property("Object files", number(objects));
    if (imports)
        b.property("Import entries", number(imports));
    b.property("Symbol index", linkerMember ? number(indexEntries) + " names" : "none (link with ranlib to add one)");
    if (!img.libraries.empty()) {
        std::string dlls;
        for (size_t i = 0; i < img.libraries.size() && i < 8; ++i)
            dlls += (i ? ", " : "") + std::string(img.text(img.libraries[i].name));
        if (img.libraries.size() > 8)
            dlls += ", ...";
        b.property("DLLs", dlls);
    }
    for (const Member& m : img.members) // the machine, from the first object
        if (const std::string_view f = img.text(m.format); f.find(", ") != std::string_view::npos) {
            b.property("Machine", f.substr(f.rfind(", ") + 2));
            break;
        }
    return img.summary;
}

} // namespace

// ---- Entry points --------------------------------------------------------------------------------

bool recognises(std::string_view head)
{
    bool bigObj = false;
    return isElf(head) || isArchive(head) || isPe(head) || isCoffObject(head, &bigObj);
}

std::string describe(std::string_view head)
{
    if (isArchive(head))
        return "static library";
    if (isElf(head)) {
        const Bytes h{head, head[5] == 2};
        switch (h.u16(16)) {
            case 1: return "ELF object file";
            case 2: return "ELF executable";
            case 3: return "ELF shared library";
            case 4: return "ELF core dump";
        }
        return "ELF file";
    }
    if (isPe(head)) {
        const Bytes h{head};
        const std::uint32_t pe = h.u32(0x3C);
        if (pe + 24 <= head.size() && head.substr(pe, 4) == std::string_view("PE\0\0", 4))
            return (h.u16(pe + 22) & 0x2000) ? "Windows DLL" : "Windows executable";
        return "Windows executable";
    }
    bool bigObj = false;
    if (isCoffObject(head, &bigObj))
        return "COFF object file";
    return {};
}

Image read(const Reader& reader, std::uint64_t fileSize)
{
    Image img;
    Builder b(img);
    Cache cache{reader, kNone, {}, fileSize};
    const Source src(cache, 0, fileSize);
    std::string head;
    src.read(0, 4096, head);
    bool bigObj = false;
    if (isArchive(head)) {
        img.format = Format::Archive;
        readArchive(src, b);
        b.buildLists(false);
    } else if (isElf(head)) {
        img.format = Format::Elf;
        img.summary = readElf(src, b, kNoIndex);
        const bool object = Bytes{head, head[5] == 2}.u16(16) == 1;
        b.buildLists(!object);
    } else if (isPe(head)) {
        img.format = Format::Pe;
        img.summary = readPe(src, b);
        b.buildLists(true);
    } else if (isCoffObject(head, &bigObj)) {
        img.format = Format::Coff;
        img.summary = readCoff(src, b, kNoIndex, bigObj);
        b.buildLists(false);
    } else {
        img.error = "not a library or program this can read (ELF, PE/COFF, ar archive)";
    }
    // The pool and tables were grown piecewise: give back what the growth left over.
    img.strings.shrink_to_fit();
    img.symbols.shrink_to_fit();
    img.overview.shrink_to_fit();
    return img;
}

} // namespace enp::model::bin
