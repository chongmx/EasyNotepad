// Part of tools/text-study (docs/plan/24-compact-text.md). Build: g++ -O2 -std=c++17 -o search_bench search_bench.cpp; run: ./search_bench <tree>
// Find in Files over a tree: the current algorithm vs faster ones, same files, same queries.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <string>
#include <string_view>
#include <vector>
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Line { size_t start, length; };
static std::vector<Line> splitLines(const std::string& text) {
    std::vector<Line> lines; size_t start = 0;
    for (size_t i = 0; i < text.size(); ++i)
        if (text[i] == '\n' || text[i] == '\r') {
            lines.push_back({start, i - start});
            if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            start = i + 1;
        }
    lines.push_back({start, text.size() - start});
    return lines;
}
// Now: Search.cpp's searchText (per-line copies; std::regex for anything but plain case-sensitive).
static size_t current(const std::string& text, const std::string& q, bool regex, bool icase, bool word) {
    size_t n = 0;
    const bool simple = !regex && !word && !icase;
    std::regex re;
    if (!simple) {
        std::string p = q;
        if (!regex) { p.clear(); for (char c : q) { if (std::strchr("\\^$.|?*+()[]{}", c)) p += '\\'; p += c; } }
        if (word) p = "\\b" + p + "\\b";
        re = std::regex(p, icase ? std::regex::ECMAScript | std::regex::icase : std::regex::ECMAScript);
    }
    for (const Line& l : splitLines(text)) {
        const std::string line = text.substr(l.start, l.length);
        if (simple) { for (size_t at = line.find(q); at != std::string::npos; at = line.find(q, at + q.size())) ++n; continue; }
        for (auto it = std::sregex_iterator(line.begin(), line.end(), re); it != std::sregex_iterator(); ++it) if (it->length(0)) ++n;
    }
    return n;
}
static bool ident(char c) { return std::isalnum((unsigned char)c) || c == '_'; }
// Faster, no new dependency: the whole buffer is scanned in place (memchr for the first byte / a
// case-folded first byte, then memcmp), whole words checked at the edges, lines counted only where a
// hit is; a regex runs only on lines holding its required literal (the ripgrep prefilter).
static size_t fast(const std::string& text, const std::string& q, bool regex, bool icase, bool word, const std::string& literal, const std::regex* re) {
    size_t n = 0;
    std::string_view t(text);
    if (regex) {
        // Lines containing the literal, then the regex on those lines only (no copies: cregex on pointers).
        size_t from = 0;
        while ((from = t.find(literal, from)) != std::string_view::npos) {
            size_t b = t.rfind('\n', from); b = b == std::string_view::npos ? 0 : b + 1;
            size_t e = t.find('\n', from); e = e == std::string_view::npos ? t.size() : e;
            for (std::cregex_iterator it(text.data() + b, text.data() + e, *re), end; it != end; ++it) if (it->length(0)) ++n;
            from = e;
        }
        return n;
    }
    const size_t m = q.size();
    auto eq = [&](size_t at) {
        if (!icase) return std::memcmp(text.data() + at, q.data(), m) == 0;
        for (size_t k = 0; k < m; ++k) if (std::tolower((unsigned char)text[at + k]) != std::tolower((unsigned char)q[k])) return false;
        return true;
    };
    const char a = q[0], b = icase ? char(std::isupper((unsigned char)a) ? std::tolower(a) : std::toupper(a)) : a;
    const char* p = text.data(); const char* end = p + text.size();
    while (p + m <= end) {
        const char* x = (const char*)std::memchr(p, a, size_t(end - p));
        if (a != b) { const char* y = (const char*)std::memchr(p, b, size_t(end - p)); if (!x || (y && y < x)) x = y; }
        if (!x || x + m > end) break;
        const size_t at = size_t(x - text.data());
        if (eq(at) && (!word || ((at == 0 || !ident(text[at - 1])) && (at + m == text.size() || !ident(text[at + m]))))) { ++n; p = x + m; }
        else p = x + 1;
    }
    return n;
}
int main(int argc, char** argv) {
    std::vector<std::string> paths;
    for (auto& e : fs::recursive_directory_iterator(argv[1], fs::directory_options::skip_permission_denied))
        if (e.is_regular_file() && e.file_size() <= 2 * 1024 * 1024 && e.path().string().find("/.") == std::string::npos) paths.push_back(e.path().string());
    struct Q { const char* name; std::string q; bool regex, icase, word; std::string literal; };
    std::vector<Q> qs = {{"plain, match case", "memory", false, false, false, ""},
                         {"plain, ignore case", "memory", false, true, false, ""},
                         {"whole word, match case", "ImGui", false, false, true, ""},
                         {"regex", "Scope\\(\"[a-z]+\"\\)", true, false, false, "Scope(\""}};
    // Read once (the search itself is what is compared); also time reading per file as Find in Files does.
    std::vector<std::string> texts; size_t bytes = 0;
    auto r0 = Clock::now();
    std::string buffer; buffer.reserve(2 << 20);
    for (auto& p : paths) { std::ifstream f(p, std::ios::binary); std::string s((std::istreambuf_iterator<char>(f)), {}); if (s.find('\0') != std::string::npos) continue; bytes += s.size(); texts.push_back(std::move(s)); }
    double readMs = std::chrono::duration<double, std::milli>(Clock::now() - r0).count();
    std::printf("%zu text files, %.1f MB (read in %.0f ms)\n", texts.size(), bytes / 1048576.0, readMs);
    for (auto& q : qs) {
        auto t0 = Clock::now(); size_t a = 0;
        for (auto& t : texts) a += current(t, q.q, q.regex, q.icase, q.word);
        double ms0 = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        std::regex re(q.q);
        auto t1 = Clock::now(); size_t b = 0;
        for (auto& t : texts) b += fast(t, q.q, q.regex, q.icase, q.word, q.literal, &re);
        double ms1 = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();
        std::printf("  %-24s now %8.0f ms (%zu hits)   in place %6.0f ms (%zu hits)   %5.0fx\n", q.name, ms0, a, ms1, b, ms0 / ms1);
    }
}
