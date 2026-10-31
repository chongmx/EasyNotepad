#include "enp/model/Search.h"

#include <algorithm>
#include <cctype>
#include <regex>

namespace enp::model {

namespace {


std::string escapeRegex(const std::string& s)
{
    std::string out;
    for (char c : s) {
        if (std::string("\\^$.|?*+()[]{}").find(c) != std::string::npos)
            out += '\\';
        out += c;
    }
    return out;
}

// One regex for every option: plain text is escaped, whole words get \b.
bool buildRegex(const std::string& query, const SearchOptions& options, std::regex& out, std::string* error)
{
    std::string pattern = options.regex ? query : escapeRegex(query);
    if (options.wholeWord)
        pattern = "\\b(?:" + pattern + ")\\b";
    auto flags = std::regex::ECMAScript;
    if (!options.matchCase)
        flags |= std::regex::icase;
    try {
        out = std::regex(pattern, flags);
        return true;
    } catch (const std::regex_error& e) {
        if (error)
            *error = std::string("Invalid regular expression: ") + e.what();
        return false;
    }
}

// Lines with their offsets; the terminators are not part of the line.
struct Line {
    size_t start;
    size_t length;
};

std::vector<Line> splitLines(const std::string& text)
{
    std::vector<Line> lines;
    size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n' || text[i] == '\r') {
            lines.push_back({start, i - start});
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            start = i + 1;
        }
    lines.push_back({start, text.size() - start});
    return lines;
}

} // namespace

std::vector<TextMatch> searchText(const std::string& text, const std::string& query, const SearchOptions& options,
                                  std::string* error)
{
    std::vector<TextMatch> matches;
    if (query.empty())
        return matches;
    // Plain, case-sensitive, not whole-word: a fast find.
    const bool simple = !options.regex && !options.wholeWord && options.matchCase;
    std::regex re;
    if (!simple && !buildRegex(query, options, re, error))
        return matches;
    const std::vector<Line> lines = splitLines(text);
    for (int l = 0; l < int(lines.size()); ++l) {
        const std::string line = text.substr(lines[size_t(l)].start, lines[size_t(l)].length);
        if (simple) {
            for (size_t at = line.find(query); at != std::string::npos; at = line.find(query, at + query.size()))
                matches.push_back({l, int(at), int(query.size())});
            continue;
        }
        for (auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator(); ++it) {
            if (it->length(0) == 0)
                continue; // empty matches (e.g. "a*") are not results
            matches.push_back({l, int(it->position(0)), int(it->length(0))});
        }
    }
    return matches;
}

std::string replaceText(const std::string& text, const std::string& query, const std::string& replacement,
                        const SearchOptions& options, int* count, std::string* error)
{
    if (count)
        *count = 0;
    std::regex re;
    if (query.empty() || !buildRegex(query, options, re, error))
        return text;
    // Line by line, so a match never spans lines and the terminators stay as they were.
    const std::vector<Line> lines = splitLines(text);
    std::string out;
    out.reserve(text.size());
    size_t copied = 0;
    for (const Line& line : lines) {
        out.append(text, copied, line.start - copied); // terminator(s) before this line
        const std::string s = text.substr(line.start, line.length);
        int n = 0;
        for (auto it = std::sregex_iterator(s.begin(), s.end(), re); it != std::sregex_iterator(); ++it)
            n += it->length(0) > 0 ? 1 : 0;
        if (n == 0) {
            out += s;
        } else {
            // Plain text replaces literally ($ means $); a regex may use $1 groups.
            const std::string with = options.regex ? replacement : std::regex_replace(replacement, std::regex("\\$"), "$$$$");
            out += std::regex_replace(s, re, with);
            if (count)
                *count += n;
        }
        copied = line.start + line.length;
    }
    out.append(text, copied, std::string::npos);
    return out;
}

bool matchesFilter(const std::string& fileName, const std::string& filter)
{
    if (filter.empty())
        return true;
    auto lower = [](std::string s) {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string name = lower(fileName);
    // Glob match with '*' and '?'.
    auto glob = [](const std::string& pattern, const std::string& s) {
        size_t p = 0, i = 0, star = std::string::npos, mark = 0;
        while (i < s.size()) {
            if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == s[i])) {
                ++p;
                ++i;
            } else if (p < pattern.size() && pattern[p] == '*') {
                star = p++;
                mark = i;
            } else if (star != std::string::npos) {
                p = star + 1;
                i = ++mark;
            } else {
                return false;
            }
        }
        while (p < pattern.size() && pattern[p] == '*')
            ++p;
        return p == pattern.size();
    };
    size_t start = 0;
    const std::string f = lower(filter);
    while (start <= f.size()) {
        size_t end = f.find_first_of(";,", start);
        if (end == std::string::npos)
            end = f.size();
        std::string pattern = f.substr(start, end - start);
        pattern.erase(0, pattern.find_first_not_of(' '));
        while (!pattern.empty() && pattern.back() == ' ')
            pattern.pop_back();
        if (!pattern.empty() && glob(pattern, name))
            return true;
        start = end + 1;
    }
    return false;
}

} // namespace enp::model
