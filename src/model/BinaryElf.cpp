// ELF: Linux and other Unix shared libraries, executables and object files.
#include "BinaryInternal.h"

#include <unordered_set>

namespace enp::model::bin::detail {

namespace {

enum : std::uint32_t {
    SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_DYNAMIC = 6, SHT_NOTE = 7, SHT_NOBITS = 8, SHT_DYNSYM = 11,
    SHT_GNU_VERDEF = 0x6ffffffd, SHT_GNU_VERNEED = 0x6ffffffe, SHT_GNU_VERSYM = 0x6fffffff,
    PT_LOAD = 1, PT_DYNAMIC = 2, PT_INTERP = 3, PT_NOTE = 4, PT_TLS = 7,
    PT_GNU_STACK = 0x6474e551, PT_GNU_RELRO = 0x6474e552,
    SHF_WRITE = 1, SHF_ALLOC = 2, SHF_EXECINSTR = 4,
};

struct Sec {
    std::uint32_t name = 0, type = 0, link = 0;
    std::uint64_t flags = 0, addr = 0, offset = 0, size = 0, entsize = 0;
};
struct Seg {
    std::uint32_t type = 0, flags = 0;
    std::uint64_t offset = 0, vaddr = 0, filesz = 0, memsz = 0;
};

const char* machineName(std::uint16_t m)
{
    switch (m) {
        case 2: return "SPARC";
        case 3: return "x86 (i386)";
        case 4: return "Motorola 68000";
        case 8: return "MIPS";
        case 20: return "PowerPC";
        case 21: return "PowerPC 64";
        case 22: return "IBM S/390";
        case 40: return "ARM";
        case 42: return "SuperH";
        case 43: return "SPARC v9";
        case 50: return "IA-64";
        case 62: return "x86-64";
        case 183: return "AArch64";
        case 243: return "RISC-V";
        case 247: return "BPF";
        case 258: return "LoongArch";
    }
    return nullptr;
}

const char* osAbiName(std::uint8_t a)
{
    switch (a) {
        case 0: return "System V";
        case 1: return "HP-UX";
        case 2: return "NetBSD";
        case 3: return "GNU/Linux";
        case 6: return "Solaris";
        case 9: return "FreeBSD";
        case 12: return "OpenBSD";
        case 97: return "ARM";
        case 255: return "standalone";
    }
    return nullptr;
}

const char* sectionTypeName(std::uint32_t t)
{
    switch (t) {
        case 0: return "NULL";
        case 1: return "PROGBITS";
        case 2: return "SYMTAB";
        case 3: return "STRTAB";
        case 4: return "RELA";
        case 5: return "HASH";
        case 6: return "DYNAMIC";
        case 7: return "NOTE";
        case 8: return "NOBITS";
        case 9: return "REL";
        case 11: return "DYNSYM";
        case 14: return "INIT_ARRAY";
        case 15: return "FINI_ARRAY";
        case 16: return "PREINIT_ARRAY";
        case 17: return "GROUP";
        case 18: return "SYMTAB_SHNDX";
        case 19: return "RELR";
        case 0x6ffffff6: return "GNU_HASH";
        case 0x6ffffffd: return "VERDEF";
        case 0x6ffffffe: return "VERNEED";
        case 0x6fffffff: return "VERSYM";
        case 0x70000001: return "ARM_EXIDX / X86_64_UNWIND";
        case 0x70000003: return "ARM_ATTRIBUTES";
    }
    return nullptr;
}

std::string machineText(std::uint16_t m)
{
    const char* name = machineName(m);
    return name ? name : "machine " + std::to_string(m);
}

} // namespace

std::string readElf(const Source& src, Builder& b, std::uint32_t member)
{
    Image& img = b.image();
    const bool whole = member == kNoIndex;
    std::string head;
    if (!src.read(0, 64, head) || head.size() < 52) {
        if (whole)
            img.error = "the ELF header is cut short";
        return "ELF (damaged)";
    }
    const bool is64 = head[4] == 2;
    const bool big = head[5] == 2;
    const Bytes h{head, big};
    const std::uint16_t type = h.u16(16), machine = h.u16(18);
    const std::uint64_t entry = h.word(24, is64);
    const std::uint64_t phoff = is64 ? h.u64(32) : h.u32(28);
    const std::uint64_t shoff = is64 ? h.u64(40) : h.u32(32);
    const std::uint32_t flags = h.u32(is64 ? 48 : 36);
    const std::uint16_t phentsize = h.u16(is64 ? 54 : 42), phnum = h.u16(is64 ? 56 : 44);
    const std::uint16_t shentsize = h.u16(is64 ? 58 : 46);
    std::uint64_t shnum = h.u16(is64 ? 60 : 48);
    std::uint32_t shstrndx = h.u16(is64 ? 62 : 50);
    const char* typeName = type == 1 ? "object" : type == 2 ? "executable" : type == 3 ? "shared library" : type == 4 ? "core dump" : "file";
    const std::string description = std::string("ELF ") + (is64 ? "64" : "32") + "-bit " + typeName + ", " + machineText(machine);

    // Section headers (the count and the name table's index move to section 0 when they overflow).
    std::vector<Sec> secs;
    std::string data;
    const size_t secSize = is64 ? 64 : 40;
    auto parseSection = [&](const Bytes& s, size_t o) {
        Sec x;
        x.name = s.u32(o);
        x.type = s.u32(o + 4);
        x.flags = s.word(o + 8, is64);
        x.addr = is64 ? s.u64(o + 16) : s.u32(o + 12);
        x.offset = is64 ? s.u64(o + 24) : s.u32(o + 16);
        x.size = is64 ? s.u64(o + 32) : s.u32(o + 20);
        x.link = s.u32(is64 ? o + 40 : o + 24);
        x.entsize = is64 ? s.u64(o + 56) : s.u32(o + 36);
        return x;
    };
    if (shoff && shentsize >= secSize) {
        if (shnum == 0 || shstrndx == 0xffff) {
            src.read(shoff, secSize, data);
            const Sec zero = parseSection(Bytes{data, big}, 0);
            if (shnum == 0)
                shnum = zero.size;
            if (shstrndx == 0xffff)
                shstrndx = zero.link;
        }
        shnum = std::min<std::uint64_t>(shnum, kMaxTable / shentsize);
        if (src.read(shoff, size_t(shnum * shentsize), data)) {
            const Bytes s{data, big};
            for (std::uint64_t i = 0; i < shnum && (i + 1) * shentsize <= data.size(); ++i)
                secs.push_back(parseSection(s, size_t(i * shentsize)));
        }
    }
    auto content = [&](const Sec& s, std::string& out) {
        out.clear();
        if (s.type == SHT_NOBITS || s.size == 0)
            return false;
        return src.read(s.offset, size_t(std::min<std::uint64_t>(s.size, kMaxTable)), out);
    };
    std::string shstr;
    if (shstrndx < secs.size())
        content(secs[shstrndx], shstr);
    const Bytes names{shstr, big};
    auto secName = [&](size_t i) { return i < secs.size() ? names.cstr(secs[i].name) : std::string_view(); };

    // Program headers.
    std::vector<Seg> segs;
    if (phoff && phentsize >= (is64 ? 56 : 32) && phnum && src.read(phoff, size_t(phnum) * phentsize, data)) {
        const Bytes p{data, big};
        for (size_t i = 0; i < phnum && (i + 1) * phentsize <= data.size(); ++i) {
            const size_t o = i * phentsize;
            Seg g;
            g.type = p.u32(o);
            if (is64) {
                g.flags = p.u32(o + 4);
                g.offset = p.u64(o + 8);
                g.vaddr = p.u64(o + 16);
                g.filesz = p.u64(o + 32);
                g.memsz = p.u64(o + 40);
            } else {
                g.offset = p.u32(o + 4);
                g.vaddr = p.u32(o + 8);
                g.filesz = p.u32(o + 16);
                g.memsz = p.u32(o + 20);
                g.flags = p.u32(o + 24);
            }
            segs.push_back(g);
        }
    }
    auto addressToOffset = [&](std::uint64_t va) -> std::uint64_t {
        for (const Seg& g : segs)
            if (g.type == PT_LOAD && va >= g.vaddr && va - g.vaddr < g.filesz)
                return g.offset + (va - g.vaddr);
        return kNone;
    };

    // The dynamic section: needed libraries, SONAME, search paths, flags.
    std::vector<std::string> needed;
    std::string soname, rpath, runpath;
    std::uint64_t dtFlags = 0, dtFlags1 = 0;
    bool hasDynamic = false;
    {
        std::string dyn, dynstr;
        for (const Sec& s : secs)
            if (s.type == SHT_DYNAMIC && content(s, dyn)) {
                if (s.link < secs.size())
                    content(secs[s.link], dynstr);
                break;
            }
        if (dyn.empty()) // no section headers: through the program headers
            for (const Seg& g : segs)
                if (g.type == PT_DYNAMIC)
                    src.read(g.offset, size_t(std::min<std::uint64_t>(g.filesz, kMaxTable)), dyn);
        hasDynamic = !dyn.empty();
        const Bytes d{dyn, big};
        const size_t entrySize = is64 ? 16 : 8;
        if (dynstr.empty()) {
            std::uint64_t strtab = 0, strsz = 0;
            for (size_t o = 0; o + entrySize <= dyn.size(); o += entrySize) {
                const std::uint64_t tag = d.word(o, is64), val = d.word(o + entrySize / 2, is64);
                if (tag == 5)
                    strtab = val;
                else if (tag == 10)
                    strsz = val;
            }
            if (const std::uint64_t at = addressToOffset(strtab); at != kNone)
                src.read(at, size_t(std::min<std::uint64_t>(strsz ? strsz : 1u << 20, kMaxTable)), dynstr);
        }
        const Bytes ds{dynstr, big};
        for (size_t o = 0; o + entrySize <= dyn.size(); o += entrySize) {
            const std::uint64_t tag = d.word(o, is64), val = d.word(o + entrySize / 2, is64);
            if (tag == 0)
                break;
            switch (tag) {
                case 1: needed.emplace_back(ds.cstr(size_t(val))); break;
                case 14: soname = ds.cstr(size_t(val)); break;
                case 15: rpath = ds.cstr(size_t(val)); break;
                case 29: runpath = ds.cstr(size_t(val)); break;
                case 30: dtFlags = val; break;
                case 0x6ffffffb: dtFlags1 = val; break;
                default: break;
            }
        }
    }
    if (whole)
        for (const std::string& n : needed)
            img.libraries[b.library(n)].needed = true;

    // Symbol versions: what each dynamic symbol's version index names, and (for imports) which
    // library it was linked against.
    std::vector<std::uint16_t> versym;
    std::unordered_map<std::uint32_t, std::pair<std::string, std::string>> versions; // index -> name, library
    for (const Sec& s : secs) {
        if (s.type == SHT_GNU_VERSYM && content(s, data)) {
            const Bytes v{data, big};
            versym.resize(data.size() / 2);
            for (size_t i = 0; i < versym.size(); ++i)
                versym[i] = v.u16(i * 2);
        } else if ((s.type == SHT_GNU_VERDEF || s.type == SHT_GNU_VERNEED) && content(s, data) && s.link < secs.size()) {
            std::string strs;
            content(secs[s.link], strs);
            const Bytes v{data, big}, str{strs, big};
            size_t o = 0;
            for (int guard = 0; guard < 65536 && o + 16 <= data.size(); ++guard) {
                if (s.type == SHT_GNU_VERDEF) { // vd_version vd_flags vd_ndx vd_cnt vd_hash vd_aux vd_next
                    const std::uint16_t ndx = v.u16(o + 4), flagsV = v.u16(o + 2);
                    const std::uint32_t aux = v.u32(o + 12), nextV = v.u32(o + 16);
                    if (!(flagsV & 1)) // not the file's own name (VER_FLG_BASE)
                        versions[ndx] = {std::string(str.cstr(v.u32(o + aux))), {}};
                    if (!nextV)
                        break;
                    o += nextV;
                } else { // vn_version vn_cnt vn_file vn_aux vn_next; vernaux: hash flags other name next
                    const std::uint16_t cnt = v.u16(o + 2);
                    const std::string file(str.cstr(v.u32(o + 4)));
                    size_t a = o + v.u32(o + 8);
                    for (std::uint16_t k = 0; k < cnt && a + 16 <= data.size(); ++k) {
                        versions[v.u16(a + 6)] = {std::string(str.cstr(v.u32(a + 8))), file};
                        const std::uint32_t nextA = v.u32(a + 12);
                        if (!nextA)
                            break;
                        a += nextA;
                    }
                    const std::uint32_t nextN = v.u32(o + 12);
                    if (!nextN)
                        break;
                    o += nextN;
                }
            }
        }
    }

    // Symbols: the dynamic table (what a shared library offers and needs), then the full table.
    const Sec* dynsym = nullptr;
    const Sec* symtab = nullptr;
    bool debugInfo = false;
    std::string debugLink;
    for (size_t i = 0; i < secs.size(); ++i) {
        if (secs[i].type == SHT_DYNSYM && !dynsym)
            dynsym = &secs[i];
        else if (secs[i].type == SHT_SYMTAB && !symtab)
            symtab = &secs[i];
        const std::string_view n = secName(i);
        debugInfo = debugInfo || n == ".debug_info" || n == ".zdebug_info";
        if (n == ".gnu_debuglink" && content(secs[i], data))
            debugLink = Bytes{data}.cstr(0);
    }
    std::unordered_set<std::string> exported; // names in the dynamic table, so the full table does not repeat them
    size_t stackChk = 0, fortified = 0;
    auto readTable = [&](const Sec& table, bool dynamic) {
        std::string syms, strs;
        if (!content(table, syms) || table.link >= secs.size())
            return;
        content(secs[table.link], strs);
        const Bytes t{syms, big}, str{strs, big};
        const size_t entry = is64 ? 24 : 16;
        const size_t count = syms.size() / entry;
        for (size_t i = 1; i < count; ++i) {
            const size_t o = i * entry;
            const std::uint32_t nameAt = t.u32(o);
            std::uint8_t info, other;
            std::uint16_t shndx;
            std::uint64_t value, size;
            if (is64) {
                info = t.u8(o + 4), other = t.u8(o + 5), shndx = t.u16(o + 6), value = t.u64(o + 8), size = t.u64(o + 16);
            } else {
                value = t.u32(o + 4), size = t.u32(o + 8), info = t.u8(o + 12), other = t.u8(o + 13), shndx = t.u16(o + 14);
            }
            const int stType = info & 0xf, stBind = info >> 4, visibility = other & 3;
            if (stType == 3 || stType == 4) // section, file
                continue;
            const std::string_view name = str.cstr(nameAt);
            if (name.empty())
                continue;
            const bool undefined = shndx == 0;
            if (!dynamic && stBind == 0 && (name.rfind(".L", 0) == 0 || (name.size() <= 3 && name[0] == '$')))
                continue; // assembler-local labels, ARM/AArch64 mapping symbols ($x, $d, $t)
            Side side;
            if (dynamic) {
                if (undefined)
                    side = Side::Import;
                else if (stBind == 0 || visibility == 1 || visibility == 2)
                    side = Side::Local;
                else
                    side = Side::Export;
                if (side == Side::Export)
                    exported.emplace(name);
            } else if (dynsym && type != 1) {
                // A linked file with a dynamic table: what it exports is listed there already; the full
                // table adds only its internal symbols.
                if (undefined || exported.count(std::string(name)))
                    continue;
                side = Side::Local;
            } else {
                side = undefined ? Side::Import : stBind == 0 ? Side::Local : Side::Export;
            }
            Symbol* s = b.symbol(name, side);
            if (!s)
                return;
            s->member = member;
            s->binding = stBind == 0 ? Binding::Local : stBind == 1 ? Binding::Global : stBind == 2 ? Binding::Weak
                       : stBind == 10 ? Binding::Unique : Binding::None;
            s->kind = stType == 2 || stType == 10 ? SymbolKind::Function
                    : stType == 1 ? SymbolKind::Data
                    : stType == 6 ? SymbolKind::ThreadLocal
                    : stType == 5 || shndx == 0xfff2 ? SymbolKind::Common
                    : SymbolKind::Other;
            s->hidden = visibility == 1 || visibility == 2;
            s->size = size;
            if (!undefined) {
                s->address = value;
                if (shndx < secs.size() && shndx != 0) {
                    const Sec& in = secs[shndx];
                    s->section = b.intern(secName(shndx));
                    if (in.type != SHT_NOBITS) {
                        const std::uint64_t delta = type == 1 ? value : value - in.addr;
                        if (delta < in.size)
                            s->offset = in.offset + delta;
                    }
                    if (s->kind == SymbolKind::Other && (in.flags & SHF_EXECINSTR) && side != Side::Import)
                        s->kind = SymbolKind::Function;
                } else if (shndx == 0xfff1) {
                    s->section = b.intern("(absolute)");
                }
            }
            if (s->kind == SymbolKind::Common)
                s->section = b.intern("(common)");
            if (dynamic && i < versym.size()) {
                const std::uint16_t v = versym[i];
                if (auto it = versions.find(v & 0x7fff); it != versions.end()) {
                    s->detail = b.intern((v & 0x8000) ? it->second.first + " (hidden)" : it->second.first);
                    if (undefined && !it->second.second.empty() && whole) {
                        s->library = b.library(it->second.second);
                        ++img.libraries[s->library].symbols;
                    }
                }
            }
            if (undefined && whole) {
                if (name == "__stack_chk_fail" || name == "__stack_chk_guard")
                    ++stackChk;
                if (name.size() > 6 && name.substr(0, 2) == "__" && name.substr(name.size() - 4) == "_chk")
                    ++fortified;
            }
        }
    };
    if (dynsym)
        readTable(*dynsym, true);
    if (symtab)
        readTable(*symtab, false);

    if (!whole)
        return description;

    // Sections.
    for (size_t i = 0; i < secs.size(); ++i) {
        const Sec& s = secs[i];
        if (s.type == 0 && i == 0)
            continue;
        Section x;
        x.name = b.intern(secName(i));
        const char* tn = sectionTypeName(s.type);
        x.type = b.intern(tn ? tn : hex(s.type));
        x.address = s.addr;
        x.memorySize = (s.flags & SHF_ALLOC) ? s.size : 0;
        x.offset = s.type == SHT_NOBITS ? kNone : s.offset;
        x.fileSize = s.type == SHT_NOBITS ? 0 : s.size;
        x.read = (s.flags & SHF_ALLOC) != 0;
        x.write = (s.flags & SHF_WRITE) != 0;
        x.execute = (s.flags & SHF_EXECINSTR) != 0;
        img.sections.push_back(x);
    }

    // Notes: the build ID and the minimum kernel.
    std::string buildId, abiTag;
    auto readNotes = [&](std::string_view notes) {
        const Bytes n{notes, big};
        size_t o = 0;
        while (o + 12 <= notes.size()) {
            const std::uint32_t namesz = n.u32(o), descsz = n.u32(o + 4), ntype = n.u32(o + 8);
            const size_t nameAt = o + 12, descAt = nameAt + ((size_t(namesz) + 3) & ~size_t(3));
            if (descAt + descsz > notes.size())
                break;
            if (notes.substr(nameAt, namesz).substr(0, 3) == "GNU") {
                if (ntype == 3) {
                    static const char digits[] = "0123456789abcdef";
                    buildId.clear();
                    for (size_t k = 0; k < descsz; ++k) {
                        const auto c = static_cast<unsigned char>(notes[descAt + k]);
                        buildId += digits[c >> 4];
                        buildId += digits[c & 15];
                    }
                } else if (ntype == 1 && descsz >= 16) {
                    static const char* const oses[] = {"Linux", "GNU Hurd", "Solaris", "FreeBSD"};
                    const std::uint32_t os = n.u32(descAt);
                    abiTag = std::string(os < 4 ? oses[os] : "OS " + std::to_string(os)) + " " + std::to_string(n.u32(descAt + 4)) + "." +
                             std::to_string(n.u32(descAt + 8)) + "." + std::to_string(n.u32(descAt + 12)) + " or later";
                }
            }
            o = descAt + ((size_t(descsz) + 3) & ~size_t(3));
        }
    };
    bool notesRead = false;
    for (const Sec& s : secs)
        if (s.type == SHT_NOTE && content(s, data))
            readNotes(data), notesRead = true;
    if (!notesRead)
        for (const Seg& g : segs)
            if (g.type == PT_NOTE && src.read(g.offset, size_t(std::min<std::uint64_t>(g.filesz, 1u << 20)), data))
                readNotes(data);

    std::string interpreter;
    bool execStack = false, gnuStack = false, relro = false, tls = false;
    for (const Seg& g : segs) {
        if (g.type == PT_INTERP && src.read(g.offset, size_t(std::min<std::uint64_t>(g.filesz, 4096)), data))
            interpreter = Bytes{data}.cstr(0);
        if (g.type == PT_GNU_STACK)
            gnuStack = true, execStack = (g.flags & 1) != 0;
        relro = relro || g.type == PT_GNU_RELRO;
        tls = tls || g.type == PT_TLS;
    }

    // The overview.
    size_t functions = 0, dataExports = 0, imports = 0, locals = 0;
    for (const Symbol& s : img.symbols) {
        if (s.side == Side::Export)
            (s.kind == SymbolKind::Function ? functions : dataExports)++;
        imports += s.side == Side::Import;
        locals += s.side == Side::Local;
    }
    const bool pie = type == 3 && ((dtFlags1 & 0x08000000) || (!interpreter.empty() && soname.empty())); // libc.so.6 has an interpreter too
    const bool bindNow = (dtFlags & 8) || (dtFlags1 & 1);
    b.heading("File");
    b.property("Format", std::string("ELF ") + (is64 ? "64-bit" : "32-bit") + ", " + (big ? "big-endian" : "little-endian"));
    b.property("Type", type == 1 ? "Relocatable object (.o)" : type == 2 ? "Executable" : type == 3 ? (pie ? "Position-independent executable (PIE)" : "Shared library (.so)")
                     : type == 4 ? "Core dump" : "Type " + std::to_string(type));
    b.property("Machine", machineText(machine));
    if (const char* abi = osAbiName(std::uint8_t(head[7])))
        b.property("OS / ABI", abi);
    if (flags)
        b.property("Processor flags", hex(flags));
    if (entry)
        b.property("Entry point", hex(entry));
    if (type != 1) {
        b.heading("Loading");
        b.property("Interpreter", interpreter);
        b.property("SONAME", soname);
        b.property("Needed libraries", needed.empty() ? "none" : number(needed.size()));
        b.property("RPATH", rpath);
        b.property("RUNPATH", runpath);
        b.property("Binding", hasDynamic ? (bindNow ? "Immediate (BIND_NOW)" : "Lazy") : "Static (no dynamic section)");
        b.property("Build ID", buildId);
        b.property("Minimum OS", abiTag);
        b.property("Thread-local storage", tls ? "Yes" : "");
        b.heading("Security");
        if (type == 2 || pie)
            b.property("PIE (address randomisation)", pie ? "Yes" : "No: loads at a fixed address");
        b.property("Non-executable stack", !gnuStack ? "Unknown (no GNU_STACK)" : execStack ? "No: the stack is executable" : "Yes");
        b.property("RELRO", relro ? (bindNow ? "Full" : "Partial") : "None");
        if (hasDynamic) {
            b.property("Stack protector", stackChk ? "Yes (uses __stack_chk_fail)" : "Not seen");
            b.property("FORTIFY_SOURCE", fortified ? "Yes (" + number(fortified) + " checked functions)" : "Not seen");
        }
    }
    b.heading("Contents");
    b.property("Sections", number(img.sections.size()));
    b.property(type == 1 ? "Defined functions" : "Exported functions", number(functions));
    b.property(type == 1 ? "Defined data" : "Exported data", number(dataExports));
    b.property(type == 1 ? "Undefined symbols" : "Imports", number(imports));
    b.property("Symbol table", symtab ? number(locals) + " local symbols" : "Stripped (no .symtab)");
    b.property("Debug information", debugInfo ? "Yes (DWARF in the file)" : !debugLink.empty() ? "Separate file: " + debugLink : "None");
    if (img.mangled)
        b.property("Mangled names", number(img.mangled) + " (" + number(img.demangledCount) + " demangled)");
    return description;
}

} // namespace enp::model::bin::detail
