// What C and C++ code can use without the workspace declaring it: the standard libraries' names
// (which header declares each, since which standard), the keywords of each standard, the
// standards themselves, and the #include lines of a file. All of it is compiled-in constant data:
// nothing here allocates except scanIncludes(), which fills the caller's vector.
#pragma once

#include "enp/model/Symbols.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model {

// Standards are named by year: C++ 1998, 2003, 2011, 2014, 2017, 2020, 2023, 2026; C 1989, 1999,
// 2011, 2017, 2023. 0 means "not chosen" (detect it from the project, else the default).
constexpr int kDefaultCppStandard = 2017;
constexpr int kDefaultCStandard = 2017;
// The choices, oldest first (static arrays: nothing is allocated, not even on first use).
const int* cppStandards(size_t& count);
const int* cStandards(size_t& count);
// "C++17", "C99", "C++98" (2003 is "C++03")
const char* standardName(int year, bool c);

// One name of a standard library.
struct LibraryName {
    const char* name;
    const char* scope;    // "std", or the class whose member it is ("vector"); "" for C's global names
    const char* header;   // the C++ header ("vector", "cstdio"); nullptr when C++ has none
    const char* cHeader;  // the C header ("stdio.h"); nullptr when C has none
    SymbolKind kind;
    short cppSince;       // the C++ standard that added it; 0: not in C++
    short cSince;         // the C standard; 0: not in C
    const char* signature;
};
// The whole catalog (a static table).
const LibraryName* libraryNames(size_t& count);
// The first entry named `name` (members after namespace names); null when the catalog has none.
const LibraryName* findLibraryName(std::string_view name, bool c);
// Available in this language and standard.
bool libraryNameAvailable(const LibraryName& n, bool c, int standard);
// The header a C or C++ file includes for it: "<vector>", "<stdio.h>".
std::string_view libraryHeader(const LibraryName& n, bool c);

// Keywords of the language in this standard (C++: nullptr since 2011, concept since 2020...; C:
// _Bool since 1999, bool since 2023...). `visit` gets each one.
template <class F>
void forEachKeyword(bool c, int standard, F&& visit);
// Since which standard a word is a keyword (0: always, -1: not a keyword of that language).
int keywordSince(std::string_view word, bool c);

// A file is compiled as C (not C++): ".c" files.
bool isCSource(std::string_view path);

// The standard a project asks for, read from a CMakeLists.txt, Makefile, meson.build or
// compile_commands.json: CMAKE_CXX_STANDARD 20, cxx_std_17, -std=c++14 / gnu++11, and the C
// forms. 0 when the text names none.
int detectCppStandard(std::string_view buildFile);
int detectCStandard(std::string_view buildFile);

// #include lines (model::Include, in Symbols.h).
void scanIncludes(std::string_view text, std::vector<Include>& out);
void scanIncludes(const TextBuffer& text, std::vector<Include>& out); // an open document, in place

// ---- Implementation of the template ---------------------------------------------------------

struct KeywordSince {
    const char* word;
    short cppSince; // -1: not a C++ keyword
    short cSince;   // -1: not a C keyword
};
const KeywordSince* keywordTable(size_t& count);

template <class F>
void forEachKeyword(bool c, int standard, F&& visit)
{
    size_t count = 0;
    const KeywordSince* table = keywordTable(count);
    for (size_t i = 0; i < count; ++i) {
        const int since = c ? table[i].cSince : table[i].cppSince;
        if (since >= 0 && since <= standard)
            visit(std::string_view(table[i].word));
    }
}

} // namespace enp::model
