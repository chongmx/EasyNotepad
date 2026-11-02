// IntelliSense without a compiler: the declarations of a source file found by reading its text
// (types, functions, namespaces, macros, constants and variables at type or file level), a fuzzy
// matcher that ranks names against what was typed, and an index of a workspace's declarations.
// Everything here is plain data: the index is owned by IntellisenseViewModel (scope "intellisense",
// released when the folder closes; docs/MEMORY.md).
#pragma once

#include "enp/model/Language.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enp::model {

enum class SymbolKind : unsigned char { Keyword, Word, Namespace, Type, Function, Variable, Constant, Macro };
const char* symbolKindName(SymbolKind kind); // "function"

struct Symbol {
    std::string name;
    std::string container; // "Foo" for a member of Foo (or Foo::bar); empty at file level
    std::string detail;    // the declaring line, trimmed, at most kMaxSymbolDetail bytes
    int file = -1;         // SymbolIndex::path(file)
    int line = 0;          // 0-based
    int column = 0;        // byte offset of the name in the line
    SymbolKind kind = SymbolKind::Variable;
    bool definition = false; // the body is here (not only a declaration such as a prototype)
};

constexpr size_t kMaxSymbolDetail = 160;

inline bool isIdentifierStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || (static_cast<unsigned char>(c) >= 0x80);
}
inline bool isIdentifierChar(char c)
{
    return isIdentifierStart(c) || (c >= '0' && c <= '9');
}

// The identifier around byte `column` of `line` ([begin, end), empty when there is none). The
// caret may sit just after the last character of the name.
std::pair<int, int> identifierAround(std::string_view line, int column);

// Languages whose declarations extractSymbols() understands.
bool canExtractSymbols(Language language);
// Appends the declarations of `text` (a whole file, any line endings) to `out`, numbered `file`.
void extractSymbols(std::string_view text, Language language, int file, std::vector<Symbol>& out);

// How well `word` matches what was typed: -1 when it does not, higher is better. Every typed
// character must appear in order (case-insensitive); the first at the start of the word or of one
// of its parts (camelCase, snake_case). Exact-case prefixes, word starts and runs score higher;
// longer words lower. An empty pattern matches everything with 0.
int matchScore(std::string_view pattern, std::string_view word);

// One declaration of the index, viewed in place: the text belongs to the index and stays valid
// until it changes (use it within the frame, copy what is kept).
struct SymbolView {
    std::string_view name;
    std::string_view container;
    std::string_view detail;
    int file = -1;
    int line = 0;
    int column = 0;
    SymbolKind kind = SymbolKind::Variable;
    bool definition = false;
};

// The declarations of a workspace, sorted by name. Compact on purpose: every name, container
// and declaring line is kept in one text pool and every declaration in one fixed-size entry, so
// an index of hundreds of thousands of names is a handful of large blocks. Those are mappings of
// their own (docs/MEMORY.md rule 9): freeing the index gives the memory straight back to the
// system, whichever thread built it.
class SymbolIndex {
public:
    // Files are numbered in the order they are added.
    int addFile(std::string_view path);
    // Copies the symbols in (their `file` set by addFile) and empties `symbols` (its capacity
    // stays, to be reused for the next file).
    void add(std::vector<Symbol>& symbols);
    // A file was saved: its symbols are replaced (the file is added when it is new), the text of
    // the old ones is dropped from the pool, and the index is sorted again.
    void replaceFile(std::string_view path, std::vector<Symbol>& symbols);
    void finish(); // sorts by name, then file and line: after adding
    void release(); // empties it and frees the storage (docs/MEMORY.md rule 5)

    bool empty() const { return entries_.empty(); }
    size_t size() const { return entries_.size(); }
    size_t fileCount() const { return pathEnds_.size(); }
    SymbolView at(size_t i) const;
    std::string_view path(int file) const;
    int fileIndex(std::string_view path) const; // -1 when unknown
    // The positions of every symbol named exactly `name`: [first, second). Allocates nothing.
    std::pair<size_t, size_t> named(std::string_view name) const;
    size_t bytes() const; // storage held, for the status bar and tests

private:
    struct Entry {
        std::uint32_t name = 0;   // offsets into text_
        std::uint32_t container = 0;
        std::uint32_t detail = 0;
        std::uint16_t nameLength = 0;
        std::uint16_t containerLength = 0;
        std::uint16_t detailLength = 0;
        SymbolKind kind = SymbolKind::Variable;
        bool definition = false;
        std::int32_t file = -1;
        std::int32_t line = 0;
        std::int32_t column = 0;
    };
    std::string_view nameOf(const Entry& e) const { return std::string_view(text_).substr(e.name, e.nameLength); }
    std::uint32_t store(std::string_view text, std::uint16_t& length);

    std::string text_;                 // every name, container and declaring line
    std::vector<Entry> entries_;
    std::string paths_;                // every file's path, one after another
    std::vector<std::uint32_t> pathEnds_;
};

} // namespace enp::model
