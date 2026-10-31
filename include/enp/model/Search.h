// Text search shared by Find in Files: plain or regular-expression queries, case and whole-word
// options, and replacement (with $1-style groups for regular expressions).
#pragma once

#include <string>
#include <vector>

namespace enp::model {

struct SearchOptions {
    bool matchCase = false;
    bool wholeWord = false;
    bool regex = false; // ECMAScript syntax
};

struct TextMatch {
    int line = 0;   // 0-based
    int column = 0; // byte offset in the line
    int length = 0; // bytes
};

// Matches in `text` (lines split on \n, \r\n or \r); a match never spans lines. Empty with an
// error message when the regular expression is invalid.
std::vector<TextMatch> searchText(const std::string& text, const std::string& query, const SearchOptions& options,
                                  std::string* error = nullptr);

// `text` with every match replaced; `count` gets the number of replacements. Line endings are kept.
std::string replaceText(const std::string& text, const std::string& query, const std::string& replacement,
                        const SearchOptions& options, int* count = nullptr, std::string* error = nullptr);

// "*.cpp;*.h" style filters on file names (empty: everything). '*' and '?' wildcards.
bool matchesFilter(const std::string& fileName, const std::string& filter);

} // namespace enp::model
