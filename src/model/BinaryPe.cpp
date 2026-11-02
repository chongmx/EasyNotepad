// PE/COFF: Windows DLLs and executables, and COFF object files (.obj, /bigobj).
#include "BinaryInternal.h"

namespace enp::model::bin::detail {

namespace {

constexpr unsigned char kBigObjClass[16] = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B,
                                            0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

enum : std::uint32_t {
    SCN_CODE = 0x20, SCN_INITIALIZED = 0x40, SCN_UNINITIALIZED = 0x80,
    SCN_DISCARDABLE = 0x02000000, SCN_EXECUTE = 0x20000000, SCN_READ = 0x40000000, SCN_WRITE = 0x80000000,
};

struct PeSection {
    std::string name;
    std::uint32_t virtualSize = 0, virtualAddress = 0, rawSize = 0, rawPointer = 0, characteristics = 0;
};

const char* subsystemName(std::uint16_t s)
{
    switch (s) {
        case 1: return "Native (driver or system process)";
        case 2: return "Windows GUI";
        case 3: return "Windows console";
        case 5: return "OS/2 console";
        case 7: return "POSIX console";
        case 9: return "Windows CE GUI";
        case 10: return "EFI application";
        case 11: return "EFI boot service driver";
        case 12: return "EFI runtime driver";
        case 13: return "EFI ROM";
        case 14: return "Xbox";
        case 16: return "Windows boot application";
    }
    return nullptr;
}

// Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm): no C library time zone involved.
std::string utcDate(std::uint64_t seconds)
{
    const std::int64_t days = std::int64_t(seconds / 86400);
    const std::int64_t z = days + 719468, era = z / 146097, doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const std::int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    const std::uint64_t s = seconds % 86400;
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%04lld-%02lld-%02lld %02llu:%02llu:%02llu UTC", static_cast<long long>(y), static_cast<long long>(m),
                  static_cast<long long>(d), static_cast<unsigned long long>(s / 3600), static_cast<unsigned long long>(s / 60 % 60),
                  static_cast<unsigned long long>(s % 60));
    return buf;
}

std::vector<PeSection> readSections(const Source& src, std::uint64_t at, std::uint32_t count, std::string_view stringTable)
{
    std::vector<PeSection> secs;
    std::string data;
    count = std::min<std::uint32_t>(count, 65536);
    if (!src.read(at, size_t(count) * 40, data))
        return secs;
    const Bytes s{data};
    for (size_t i = 0; i < count && (i + 1) * 40 <= data.size(); ++i) {
        const size_t o = i * 40;
        PeSection x;
        x.name = std::string(Bytes{data.substr(o, 8)}.cstr(0));
        if (x.name.size() > 1 && x.name[0] == '/') { // a long name, in the string table
            size_t at = 0;
            for (char c : x.name.substr(1))
                at = at * 10 + size_t(c >= '0' && c <= '9' ? c - '0' : 0);
            if (at < stringTable.size())
                x.name = std::string(Bytes{stringTable}.cstr(at));
        }
        x.virtualSize = s.u32(o + 8);
        x.virtualAddress = s.u32(o + 12);
        x.rawSize = s.u32(o + 16);
        x.rawPointer = s.u32(o + 20);
        x.characteristics = s.u32(o + 36);
        secs.push_back(std::move(x));
    }
    return secs;
}

void addSections(Builder& b, const std::vector<PeSection>& secs, bool image)
{
    for (const PeSection& s : secs) {
        Section x;
        x.name = b.intern(s.name);
        const std::uint32_t c = s.characteristics;
        x.type = b.intern((c & SCN_CODE) ? "code" : (c & SCN_UNINITIALIZED) ? "uninitialized data" : (c & SCN_INITIALIZED) ? "initialized data"
                                                                                                                          : "other");
        x.address = image ? s.virtualAddress : 0;
        x.memorySize = image ? s.virtualSize : 0;
        x.offset = s.rawSize ? s.rawPointer : kNone;
        x.fileSize = s.rawSize;
        x.read = (c & SCN_READ) != 0;
        x.write = (c & SCN_WRITE) != 0;
        x.execute = (c & SCN_EXECUTE) != 0;
        b.image().sections.push_back(x);
    }
}

// The COFF symbol table: an object's symbols, or (in an image built by MinGW and not stripped)
// the linker's full table. Returns the source file names it records (.file).
std::vector<std::string> readCoffSymbols(const Source& src, Builder& b, std::uint64_t at, std::uint32_t count, bool bigObj,
                                         const std::vector<PeSection>& secs, bool image, std::uint32_t member, std::string& stringTable)
{
    std::vector<std::string> files;
    const size_t record = bigObj ? 20 : 18;
    std::string syms;
    const std::uint64_t bytes = std::min<std::uint64_t>(std::uint64_t(count) * record, kMaxTable);
    if (!at || !count || !src.read(at, size_t(bytes), syms))
        return files;
    const Bytes t{syms}, strs{stringTable};
    const size_t n = syms.size() / record;
    for (size_t i = 0; i < n; ++i) {
        const size_t o = i * record;
        std::string_view name = t.u32(o) == 0 ? strs.cstr(t.u32(o + 4)) : Bytes{std::string_view(syms).substr(o, 8)}.cstr(0);
        const std::uint32_t value = t.u32(o + 8);
        const std::int32_t section = bigObj ? std::int32_t(t.u32(o + 12)) : std::int16_t(t.u16(o + 12));
        const std::uint16_t type = t.u16(o + (bigObj ? 16 : 14));
        const std::uint8_t storage = t.u8(o + (bigObj ? 18 : 16));
        const std::uint8_t aux = t.u8(o + (bigObj ? 19 : 17));
        if (storage == 103 && aux) // .file: the name is in the auxiliary records
            files.emplace_back(Bytes{std::string_view(syms).substr(o + record, std::min<size_t>(size_t(aux) * record, syms.size() - std::min(syms.size(), o + record)))}.cstr(0));
        i += aux;
        const bool external = storage == 2, weak = storage == 105, isStatic = storage == 3;
        if (!external && !weak && !isStatic)
            continue;
        if (name.empty())
            continue;
        const PeSection* in = section > 0 && size_t(section) <= secs.size() ? &secs[size_t(section) - 1] : nullptr;
        if (isStatic && (aux > 0 || (in && name == in->name) || name[0] == '$' || name[0] == '.' || name == "@feat.00"))
            continue; // section definitions, compiler labels ($LN5, .L...)
        const bool undefined = section == 0 && value == 0;
        Side side = image ? Side::Local : undefined ? Side::Import : isStatic ? Side::Local : Side::Export;
        if (image && undefined)
            continue;
        Symbol* s = b.symbol(name, side);
        if (!s)
            break;
        s->member = member;
        s->binding = isStatic ? Binding::Local : weak ? Binding::Weak : Binding::Global;
        if (section == 0 && value != 0) {
            s->kind = SymbolKind::Common;
            s->size = value;
            s->section = b.intern("(common)");
        } else if ((type & 0x30) == 0x20) {
            s->kind = SymbolKind::Function;
        } else if (in) {
            s->kind = (in->characteristics & (SCN_EXECUTE | SCN_CODE)) ? SymbolKind::Function : SymbolKind::Data;
        }
        if (in) {
            s->section = b.intern(in->name);
            s->address = image ? std::uint64_t(in->virtualAddress) + value : value;
            if (value < in->rawSize)
                s->offset = std::uint64_t(in->rawPointer) + value;
        } else if (section == -1) {
            s->address = value;
            s->section = b.intern("(absolute)");
        }
    }
    return files;
}

std::string readStringTable(const Source& src, std::uint64_t symbols, std::uint32_t count, size_t record)
{
    std::string table;
    if (!symbols)
        return table;
    const std::uint64_t at = symbols + std::uint64_t(count) * record;
    std::string size;
    if (src.read(at, 4, size) && size.size() == 4) {
        const std::uint32_t n = Bytes{size}.u32(0);
        if (n > 4)
            src.read(at, size_t(std::min<std::uint64_t>(n, kMaxTable)), table);
    }
    return table;
}

} // namespace

const char* coffMachine(std::uint16_t machine)
{
    switch (machine) {
        case 0x14c: return "x86";
        case 0x8664: return "x86-64";
        case 0xaa64: return "ARM64";
        case 0xa641: return "ARM64EC";
        case 0xa64e: return "ARM64X";
        case 0x1c0: return "ARM";
        case 0x1c4: return "ARM Thumb-2";
        case 0x200: return "IA-64";
        case 0x5032: return "RISC-V 32";
        case 0x5064: return "RISC-V 64";
        case 0x6232: return "LoongArch 32";
        case 0x6264: return "LoongArch 64";
        case 0x1f0: return "PowerPC";
        case 0x166: return "MIPS";
        case 0xebc: return "EFI byte code";
        case 0: return "any machine";
    }
    return "unknown machine";
}

bool isCoffObject(std::string_view head, bool* bigObj)
{
    const Bytes h{head};
    *bigObj = false;
    if (head.size() >= 56 && h.u16(0) == 0 && h.u16(2) == 0xFFFF && h.u16(4) >= 2 && std::memcmp(head.data() + 12, kBigObjClass, 16) == 0) {
        *bigObj = true;
        return true;
    }
    if (head.size() < 20 || h.u16(16) != 0) // objects have no optional header
        return false;
    const std::uint16_t machine = h.u16(0);
    if (machine == 0 || std::strcmp(coffMachine(machine), "unknown machine") == 0)
        return false;
    const std::uint16_t sections = h.u16(2);
    return sections > 0 && sections < 0xFF00 && (h.u16(18) & 0x2002) == 0; // not an image, not a DLL
}

std::string readCoff(const Source& src, Builder& b, std::uint32_t member, bool bigObj)
{
    Image& img = b.image();
    std::string head;
    src.read(0, 56, head);
    const Bytes h{head};
    std::uint16_t machine;
    std::uint32_t sections, symbols, count, timestamp;
    std::uint64_t sectionsAt;
    if (bigObj) {
        machine = h.u16(6), timestamp = h.u32(8), sections = h.u32(44), symbols = h.u32(48), count = h.u32(52), sectionsAt = 56;
    } else {
        machine = h.u16(0), sections = h.u16(2), timestamp = h.u32(4), symbols = h.u32(8), count = h.u32(12), sectionsAt = 20 + h.u16(16);
    }
    std::string strings = readStringTable(src, symbols, count, bigObj ? 20 : 18);
    const std::vector<PeSection> secs = readSections(src, sectionsAt, sections, strings);
    const std::vector<std::string> files = readCoffSymbols(src, b, symbols, count, bigObj, secs, false, member, strings);
    const std::string description = std::string(bigObj ? "COFF big object, " : "COFF object, ") + coffMachine(machine);
    if (member != kNoIndex)
        return description;

    addSections(b, secs, false);
    size_t functions = 0, data = 0, undefined = 0, locals = 0;
    for (const Symbol& s : img.symbols) {
        if (s.side == Side::Export)
            (s.kind == SymbolKind::Function ? functions : data)++;
        undefined += s.side == Side::Import;
        locals += s.side == Side::Local;
    }
    b.heading("File");
    b.property("Format", bigObj ? "COFF object, /bigobj (more than 65,279 sections)" : "COFF object (.obj)");
    b.property("Machine", coffMachine(machine));
    if (timestamp)
        b.property("Timestamp", utcDate(timestamp));
    if (!files.empty())
        b.property("Source file", files.front());
    b.heading("Contents");
    b.property("Sections", number(secs.size()));
    b.property("Defined functions", number(functions));
    b.property("Defined data", number(data));
    b.property("Undefined symbols", number(undefined));
    b.property("Local symbols", number(locals));
    if (img.mangled)
        b.property("Mangled names", number(img.mangled) + " (" + number(img.demangledCount) + " demangled)");
    return description;
}

std::string readPe(const Source& src, Builder& b)
{
    Image& img = b.image();
    std::string data;
    src.read(0, 64, data);
    const std::uint32_t peAt = Bytes{data}.u32(0x3C);
    std::string head;
    if (!src.read(peAt, 24, head) || head.substr(0, 4) != std::string_view("PE\0\0", 4)) {
        img.error = "an MZ program without a PE header (a DOS program): it has no exports or imports to list";
        return "DOS executable";
    }
    const Bytes h{head};
    const std::uint16_t machine = h.u16(4), sectionCount = h.u16(6), optSize = h.u16(20), characteristics = h.u16(22);
    const std::uint32_t timestamp = h.u32(8), symbolsAt = h.u32(12), symbolCount = h.u32(16);
    std::string opt;
    src.read(peAt + 24, optSize, opt);
    const Bytes o{opt};
    const std::uint16_t magic = o.u16(0);
    const bool is64 = magic == 0x20b;
    const std::uint32_t entry = o.u32(16);
    const std::uint64_t imageBase = is64 ? o.u64(24) : o.u32(28);
    const std::uint32_t sizeOfImage = o.u32(56), sizeOfHeaders = o.u32(60), checksum = o.u32(64);
    const std::uint16_t subsystem = o.u16(68), dllCharacteristics = o.u16(70);
    const std::uint32_t dirCount = std::min<std::uint32_t>(o.u32(is64 ? 108 : 92), 16);
    const size_t dirAt = is64 ? 112 : 96;
    auto dir = [&](std::uint32_t i) -> std::pair<std::uint32_t, std::uint32_t> {
        return i < dirCount ? std::make_pair(o.u32(dirAt + i * 8), o.u32(dirAt + i * 8 + 4)) : std::make_pair(0u, 0u);
    };

    std::string strings = readStringTable(src, symbolsAt, symbolCount, 18);
    const std::vector<PeSection> secs = readSections(src, peAt + 24 + optSize, sectionCount, strings);
    auto sectionOf = [&](std::uint32_t rva) -> const PeSection* {
        for (const PeSection& s : secs)
            if (rva >= s.virtualAddress && rva - s.virtualAddress < std::max(s.virtualSize, s.rawSize))
                return &s;
        return nullptr;
    };
    auto rvaToOffset = [&](std::uint32_t rva) -> std::uint64_t {
        if (rva < sizeOfHeaders)
            return rva;
        const PeSection* s = sectionOf(rva);
        return s && rva - s->virtualAddress < s->rawSize ? std::uint64_t(s->rawPointer) + (rva - s->virtualAddress) : kNone;
    };
    auto readRva = [&](std::uint32_t rva, size_t size, std::string& out) {
        const std::uint64_t at = rvaToOffset(rva);
        return at != kNone && src.read(at, size, out);
    };
    std::string text;
    auto stringAt = [&](std::uint32_t rva) -> std::string {
        if (!readRva(rva, 1024, text))
            return {};
        std::string s(Bytes{text}.cstr(0));
        if (s.size() == text.size() && readRva(rva, kMaxName, text)) // longer than the first read
            s = std::string(Bytes{text}.cstr(0));
        return s;
    };

    // x64 unwind data gives each function's extent: the size of an export.
    std::vector<std::pair<std::uint32_t, std::uint32_t>> extents;
    if (machine == 0x8664 && dir(3).second && readRva(dir(3).first, std::min<size_t>(dir(3).second, kMaxTable), data)) {
        const Bytes p{data};
        for (size_t k = 0; k + 12 <= data.size(); k += 12)
            extents.emplace_back(p.u32(k), p.u32(k + 4));
        std::sort(extents.begin(), extents.end());
    }
    auto extentOf = [&](std::uint32_t rva) -> std::uint64_t {
        auto it = std::upper_bound(extents.begin(), extents.end(), std::make_pair(rva, UINT32_MAX));
        if (it == extents.begin())
            return 0;
        --it;
        return it->first == rva && it->second > rva ? it->second - rva : 0;
    };

    // Exports: by name and by ordinal; an address inside the export directory is a forwarder.
    std::string exportName;
    size_t forwarders = 0, byOrdinalOnly = 0;
    // (Plain variables, not a structured binding: the lambda below captures them, which C++17 forbids for bindings.)
    if (const std::uint32_t rva = dir(0).first, size = dir(0).second; rva && readRva(rva, 40, data) && data.size() >= 40) {
        const Bytes e{data};
        exportName = stringAt(e.u32(12));
        const std::uint32_t base = e.u32(16);
        const std::uint32_t functions = std::min<std::uint32_t>(e.u32(20), 1u << 20);
        const std::uint32_t names = std::min<std::uint32_t>(e.u32(24), 1u << 20);
        std::string addressTable, nameTable, ordinalTable;
        readRva(e.u32(28), size_t(functions) * 4, addressTable);
        readRva(e.u32(32), size_t(names) * 4, nameTable);
        readRva(e.u32(36), size_t(names) * 2, ordinalTable);
        const Bytes at{addressTable}, nt{nameTable}, ot{ordinalTable};
        std::vector<std::uint8_t> named(functions, 0);
        auto add = [&](std::uint32_t index, std::string_view name) {
            const std::uint32_t target = at.u32(size_t(index) * 4);
            if (!target)
                return;
            Symbol* s = b.symbol(name, Side::Export);
            if (!s)
                return;
            s->ordinal = base + index;
            s->binding = Binding::Global;
            if (target >= rva && target - rva < size) {
                s->detail = b.add("→ " + stringAt(target));
                s->kind = SymbolKind::Function;
                ++forwarders;
                return;
            }
            s->address = target;
            s->offset = rvaToOffset(target);
            const PeSection* in = sectionOf(target);
            s->kind = in && (in->characteristics & (SCN_EXECUTE | SCN_CODE)) ? SymbolKind::Function : SymbolKind::Data;
            if (in)
                s->section = b.intern(in->name);
            if (s->kind == SymbolKind::Function)
                s->size = extentOf(target);
        };
        for (std::uint32_t j = 0; j < names && size_t(j) * 4 + 4 <= nameTable.size(); ++j) {
            const std::uint16_t index = ot.u16(size_t(j) * 2);
            if (index >= functions)
                continue;
            named[index] = 1;
            add(index, stringAt(nt.u32(size_t(j) * 4)));
        }
        for (std::uint32_t k = 0; k < functions && size_t(k) * 4 + 4 <= addressTable.size(); ++k)
            if (!named[k] && at.u32(size_t(k) * 4)) {
                add(k, "#" + std::to_string(base + k));
                ++byOrdinalOnly;
            }
    }

    // Imports, and delay-loaded imports: per DLL, by name (with the hint) or by ordinal.
    const size_t thunk = is64 ? 8 : 4;
    auto readThunks = [&](std::uint32_t lookup, std::uint32_t slots, std::uint32_t library, bool delayed, bool virtualAddresses) {
        std::string table;
        for (std::uint32_t k = 0; k < (1u << 20); ++k) {
            if (k % 4096 == 0 && !readRva(lookup + std::uint32_t(k * thunk), 4096 * thunk, table))
                return;
            const Bytes t{table};
            const size_t at = (k % 4096) * thunk;
            if (at + thunk > table.size())
                return;
            std::uint64_t v = t.word(at, is64);
            if (!v)
                return;
            const bool byOrdinal = is64 ? (v >> 63) != 0 : (v >> 31) != 0;
            Symbol* s;
            std::uint32_t hint = kNoIndex;
            if (byOrdinal) {
                s = b.symbol("#" + std::to_string(v & 0xffff), Side::Import);
                if (s)
                    s->ordinal = std::uint32_t(v & 0xffff);
            } else {
                if (virtualAddresses)
                    v -= imageBase;
                std::string entry;
                readRva(std::uint32_t(v), 2, entry);
                hint = Bytes{entry}.u16(0);
                s = b.symbol(stringAt(std::uint32_t(v) + 2), Side::Import);
            }
            if (!s)
                return;
            s->hint = hint;
            s->library = library;
            s->delayed = delayed;
            s->binding = Binding::Global;
            s->address = slots + std::uint32_t(k * thunk); // the slot the loader fills
            s->offset = rvaToOffset(std::uint32_t(s->address));
            ++img.libraries[library].symbols;
        }
    };
    size_t importedDlls = 0, delayedDlls = 0;
    if (const auto [rva, size] = dir(1); rva) {
        for (std::uint32_t k = 0; k < 65536; ++k) {
            if (!readRva(rva + k * 20, 20, data) || data.size() < 20)
                break;
            const Bytes d{data};
            const std::uint32_t lookup = d.u32(0), name = d.u32(12), slots = d.u32(16);
            if (!name && !slots)
                break;
            const std::uint32_t library = b.library(stringAt(name));
            ++importedDlls;
            readThunks(lookup ? lookup : slots, slots, library, false, false);
        }
    }
    if (const auto [rva, size] = dir(13); rva) {
        for (std::uint32_t k = 0; k < 65536; ++k) {
            if (!readRva(rva + k * 32, 32, data) || data.size() < 32)
                break;
            const Bytes d{data};
            const bool virtualAddresses = (d.u32(0) & 1) == 0; // old (Visual C++ 6) descriptors hold addresses, not RVAs
            const std::uint32_t adjust = virtualAddresses ? std::uint32_t(imageBase) : 0;
            const std::uint32_t name = d.u32(4), slots = d.u32(12), lookup = d.u32(16);
            if (!name)
                break;
            const std::uint32_t library = b.library(stringAt(name - adjust));
            img.libraries[library].delayed = true;
            ++delayedDlls;
            readThunks(lookup - adjust, slots - adjust, library, true, virtualAddresses);
        }
    }

    // The COFF symbol table (images linked by MinGW keep it unless stripped).
    readCoffSymbols(src, b, symbolsAt, symbolCount, false, secs, true, kNoIndex, strings);
    addSections(b, secs, true);

    // Debug directory: the PDB, a reproducible build, CET.
    std::string pdb, pdbId;
    bool reproducible = false, cet = false;
    if (const auto [rva, size] = dir(6); rva && readRva(rva, std::min<size_t>(size, 28 * 64), data)) {
        const Bytes d{data};
        std::string cv;
        for (size_t k = 0; k + 28 <= data.size(); k += 28) {
            const std::uint32_t type = d.u32(k + 12), length = d.u32(k + 16), pointer = d.u32(k + 24);
            if (type == 2 && length >= 24 && src.read(pointer, std::min<size_t>(length, 4096), cv) && cv.substr(0, 4) == "RSDS") {
                const Bytes c{cv};
                char guid[64];
                std::snprintf(guid, sizeof(guid), "%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X, age %u", c.u32(4), c.u16(8), c.u16(10),
                              c.u8(12), c.u8(13), c.u8(14), c.u8(15), c.u8(16), c.u8(17), c.u8(18), c.u8(19), c.u32(20));
                pdbId = guid;
                pdb = std::string(c.cstr(24));
            } else if (type == 16) {
                reproducible = true;
            } else if (type == 20 && length >= 4 && src.read(pointer, 4, cv)) {
                cet = (Bytes{cv}.u32(0) & 1) != 0;
            }
        }
    }

    // The overview.
    size_t functions = 0, dataExports = 0, imports = 0, locals = 0;
    for (const Symbol& s : img.symbols) {
        if (s.side == Side::Export)
            (s.kind == SymbolKind::Function ? functions : dataExports)++;
        imports += s.side == Side::Import;
        locals += s.side == Side::Local;
    }
    const bool dll = (characteristics & 0x2000) != 0;
    const bool driver = subsystem == 1 && !dll;
    const char* kind = dll ? "DLL" : driver ? "driver or native program" : "executable";
    b.heading("File");
    b.property("Format", is64 ? "PE32+ (64-bit)" : magic == 0x10b ? "PE32 (32-bit)" : "PE (optional header " + hex(magic) + ")");
    b.property("Type", dll ? "Dynamic-link library (DLL)" : driver ? "Driver or native program" : "Executable (EXE)");
    b.property("Machine", coffMachine(machine));
    if (timestamp)
        b.property("Timestamp", reproducible ? hex(timestamp) + " (a hash: reproducible build)" : utcDate(timestamp));
    if (const char* sub = subsystemName(subsystem))
        b.property("Subsystem", std::string(sub) + " " + std::to_string(o.u16(48)) + "." + std::to_string(o.u16(50)));
    b.property("Linker version", std::to_string(o.u8(2)) + "." + std::to_string(o.u8(3)));
    b.property("Minimum Windows", std::to_string(o.u16(40)) + "." + std::to_string(o.u16(42)));
    b.heading("Loading");
    b.property("Export name", exportName);
    b.property("Image base", hex(imageBase));
    b.property("Entry point (RVA)", entry ? hex(entry) : "None");
    b.property("Size in memory", number(sizeOfImage) + " bytes");
    b.property("Checksum", checksum ? hex(checksum) : "Not set");
    b.property("Imported DLLs", number(importedDlls) + (delayedDlls ? " (and " + number(delayedDlls) + " delay-loaded)" : ""));
    b.heading("Security");
    b.property("ASLR (dynamic base)", (dllCharacteristics & 0x40) ? "Yes" : "No");
    if (is64)
        b.property("High-entropy ASLR", (dllCharacteristics & 0x20) ? "Yes" : "No");
    b.property("DEP (NX compatible)", (dllCharacteristics & 0x100) ? "Yes" : "No");
    b.property("Control Flow Guard", (dllCharacteristics & 0x4000) ? "Yes" : "No");
    b.property("Structured exception handlers", (dllCharacteristics & 0x400) ? "None (NO_SEH)" : is64 ? "Table-based (x64)" : "Allowed");
    if (cet)
        b.property("CET shadow stack", "Compatible");
    b.property("Large address aware", (characteristics & 0x20) ? "Yes" : "No");
    b.property("Signature", dir(4).second ? "Authenticode signature present (" + number(dir(4).second) + " bytes; not verified here)" : "Not signed");
    b.heading("Contents");
    if (dir(14).first)
        b.property(".NET", "Managed assembly: its methods are in .NET metadata, not in the export table");
    b.property("Sections", number(secs.size()));
    b.property("Exported functions", number(functions) + (forwarders ? " (" + number(forwarders) + " forwarded)" : ""));
    b.property("Exported data", number(dataExports));
    if (byOrdinalOnly)
        b.property("Exported by ordinal only", number(byOrdinalOnly));
    b.property("Imports", number(imports));
    b.property("Resources", dir(2).first ? "Yes" : "None");
    b.property("Thread-local storage", dir(9).first ? "Yes (TLS callbacks may run on load)" : "");
    b.property("Symbol table", locals ? number(locals) + " symbols (not stripped)" : "None (stripped)");
    b.property("Debug information", pdb.empty() ? "" : "PDB: " + pdb);
    b.property("PDB signature", pdbId);
    if (img.mangled)
        b.property("Mangled names", number(img.mangled) + " (" + number(img.demangledCount) + " demangled)");
    return std::string(is64 ? "PE32+ " : "PE32 ") + kind + ", " + coffMachine(machine);
}

} // namespace enp::model::bin::detail
