// What a compiled library or program contains: its characteristics, sections, the functions and
// data it exports, what it imports and from where, and (for a static library) its members. Reads
// ELF (Linux .so, executables, .o; 32/64-bit, either byte order), PE/COFF (Windows .dll, .exe,
// .obj, including /bigobj) and ar archives (.a, Windows .lib, short import members included).
// Names mangled by C++ (Itanium or MSVC), Rust or D are demangled; the raw name is kept beside.
//
// The file is read in pieces through a callback (`Reader`), so a large library is never held whole.
// Every name lives in one string pool with offsets, so a hundred thousand symbols are a handful of
// allocations.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::bin {

constexpr std::uint64_t kNone = UINT64_MAX;
constexpr std::uint32_t kNoIndex = UINT32_MAX;

enum class Format : std::uint8_t { Unknown, Elf, Pe, Coff, Archive };

// Where a symbol belongs: offered to others, needed from others, or private to the file.
enum class Side : std::uint8_t { Export, Import, Local };
enum class SymbolKind : std::uint8_t { Function, Data, ThreadLocal, Common, Other };
enum class Binding : std::uint8_t { None, Global, Weak, Unique, Local };
enum class Mangling : std::uint8_t { None, Itanium, Microsoft, Rust, D };

struct Text { // a string in Image::strings
    std::uint32_t at = 0, length = 0;
    bool empty() const { return length == 0; }
};

struct Symbol {
    Text name;    // as stored in the file (mangled)
    Text shown;   // demangled; equal to `name` when not mangled or not demangled
    Text section; // ELF/COFF section name; empty when unknown
    Text detail;  // ELF symbol version, PE forwarder ("NTDLL.RtlAllocateHeap"), import library DLL
    std::uint64_t address = kNone; // ELF: the value (an address, or the offset in its section in an object); PE: the RVA
    std::uint64_t offset = kNone;  // where its bytes are in the file
    std::uint64_t size = 0;        // 0: not recorded
    std::uint32_t library = kNoIndex; // imports: the library it comes from; import library members: their DLL
    std::uint32_t member = kNoIndex;  // archives: the member that holds it
    std::uint32_t ordinal = kNoIndex; // PE exports and imports by ordinal; import library members
    std::uint32_t hint = kNoIndex;    // PE imports by name
    Side side = Side::Export;
    SymbolKind kind = SymbolKind::Other;
    Binding binding = Binding::None;
    Mangling mangling = Mangling::None;
    bool demangled = false; // `shown` came from the demangler
    bool hidden = false;    // ELF: hidden or internal visibility, or a hidden version
    bool delayed = false;   // PE: a delay-loaded import
};

struct Section {
    Text name;
    Text type;                     // "PROGBITS", "NOBITS", "code", "initialized data", ...
    std::uint64_t address = 0;     // ELF: address; PE: RVA
    std::uint64_t memorySize = 0;  // size when loaded
    std::uint64_t offset = kNone;  // in the file; kNone: no bytes in the file
    std::uint64_t fileSize = 0;
    bool read = false, write = false, execute = false;
};

// A library this file needs (ELF DT_NEEDED, PE imports) or, for a Windows import library, the
// DLL its members stand for.
struct Library {
    Text name;
    std::uint32_t symbols = 0; // imports resolved against it
    bool delayed = false;      // PE: delay-loaded
    bool needed = false;       // ELF: listed in DT_NEEDED
};

struct Member { // of an archive
    Text name;
    Text format;              // "ELF object", "COFF object", "import entry", "LLVM bitcode", ...
    std::uint64_t offset = 0; // of its data in the archive
    std::uint64_t size = 0;
    std::uint32_t symbols = 0;
};

struct Property { // a line of the overview; a heading when `value` is empty and `heading` is set
    Text key, value;
    bool heading = false;
};

// The left-hand tree: each node names one list of the right-hand table.
enum class ListType : std::uint8_t { Overview, Sections, Symbols, Libraries, Members };
enum class KindFilter : std::uint8_t { Any, Functions, Data };
struct ListNode {
    Text title;
    ListType type = ListType::Symbols;
    std::uint8_t depth = 0;
    bool sideFilter = false; // only symbols of `side`
    Side side = Side::Export;
    KindFilter kinds = KindFilter::Any;
    std::uint32_t library = kNoIndex; // only symbols of this library
    std::uint32_t member = kNoIndex;  // only symbols of this archive member
    bool noLibrary = false;           // only imports not tied to a library (ELF imports without a version)
    std::uint32_t count = 0;
};

struct Image {
    Format format = Format::Unknown;
    std::string summary;  // "ELF 64-bit shared library, x86-64", "PE32+ DLL, x86-64", ...
    std::string error;    // why it could not be read; empty when it was
    std::string strings;  // every Text points in here
    std::vector<Property> overview;
    std::vector<Section> sections;
    std::vector<Symbol> symbols;
    std::vector<Library> libraries;
    std::vector<Member> members;
    std::vector<ListNode> lists;
    std::uint32_t mangled = 0, demangledCount = 0; // over all symbols
    bool truncated = false; // stopped at a limit (kMaxSymbols, kMaxMembers)

    std::string_view text(Text t) const { return std::string_view(strings).substr(t.at, t.length); }
    // Whether a symbol is listed under a node of the tree.
    static bool matches(const ListNode& node, const Symbol& s);
};

constexpr size_t kMaxSymbols = 4'000'000;
constexpr size_t kMaxMembers = 200'000;

// Reads `size` bytes at `offset` into `out` (fewer at the end of the file); false on an error.
using Reader = std::function<bool(std::uint64_t offset, size_t size, std::string& out)>;

// Whether the first bytes are a format this reads (ELF, PE, COFF object, ar archive).
bool recognises(std::string_view head);
// The format's name for the file kind detection: "ELF shared library", "Windows DLL", "static library", ...
std::string describe(std::string_view head);
Image read(const Reader& reader, std::uint64_t fileSize);

// The scheme a name is mangled with (from its prefix), and the demangled text (empty: it could not
// be demangled). Prefixes that are not part of the mangling ("__imp_", a leading '.' or '_' added by
// the platform, an ELF "@VERSION") are kept around the demangled text.
Mangling manglingOf(std::string_view name);
std::string demangle(std::string_view name, Mangling* scheme = nullptr);
const char* manglingName(Mangling m); // "C++ (Itanium)", "C++ (MSVC)", "Rust", "D", ""

} // namespace enp::model::bin
