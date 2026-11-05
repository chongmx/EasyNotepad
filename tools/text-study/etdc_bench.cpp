// Part of tools/text-study (docs/plan/24-compact-text.md). Build: g++ -O2 -std=c++17 -o etdc_bench etdc_bench.cpp; run: ./etdc_bench <tree>
// Prototype: the tree's text in End-Tagged Dense Code (word-based, byte-aligned, last byte of each
// codeword has the high bit set), one vocabulary for the workspace. Measures size, search inside the
// compressed bytes, and decoding lines for display, against the plain text scanned in place.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }
static bool wordChar(unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; }

struct Vocabulary {
    std::vector<std::string> words;               // by rank
    std::unordered_map<std::string_view, uint32_t> rank;
    std::string pool;                             // storage of the words
};
static void encodeRank(uint32_t i, std::string& out) {  // ETDC: 128 one-byte, 128^2 two-byte, ...
    unsigned char buf[4]; int n = 0;
    buf[n++] = 0x80 | (i % 128); i /= 128;
    while (i > 0) { --i; buf[n++] = i % 128; i /= 128; }
    for (int k = n - 1; k >= 0; --k) out += char(buf[k]);
}
template <class F> static void tokens(std::string_view t, F&& f) {
    size_t i = 0;
    while (i < t.size()) {
        size_t j = i; const bool w = wordChar((unsigned char)t[i]);
        while (j < t.size() && wordChar((unsigned char)t[j]) == w) ++j;
        f(t.substr(i, j - i)); i = j;
    }
}
int main(int argc, char** argv) {
    std::vector<std::string> texts; size_t bytes = 0;
    for (auto& e : fs::recursive_directory_iterator(argv[1], fs::directory_options::skip_permission_denied))
        if (e.is_regular_file() && e.file_size() <= 2 * 1024 * 1024 && e.path().string().find("/.") == std::string::npos) {
            std::ifstream f(e.path(), std::ios::binary); std::string s((std::istreambuf_iterator<char>(f)), {});
            if (s.find('\0') == std::string::npos) { bytes += s.size(); texts.push_back(std::move(s)); }
        }
    // Vocabulary by frequency.
    auto t0 = Clock::now();
    std::unordered_map<std::string, uint64_t> freq;
    for (auto& t : texts) tokens(t, [&](std::string_view w) { ++freq[std::string(w)]; });
    std::vector<std::pair<uint64_t, std::string>> byFreq;
    for (auto& [w, n] : freq) byFreq.push_back({n, w});
    std::sort(byFreq.begin(), byFreq.end(), [](auto& a, auto& b) { return a.first > b.first; });
    Vocabulary v;
    size_t poolBytes = 0; for (auto& p : byFreq) poolBytes += p.second.size();
    v.pool.reserve(poolBytes);
    for (auto& p : byFreq) v.words.push_back(p.second);
    for (uint32_t i = 0; i < v.words.size(); ++i) v.rank[v.words[i]] = i;
    // Encode every file.
    std::vector<std::string> coded; size_t codedBytes = 0;
    for (auto& t : texts) { std::string c; c.reserve(t.size() / 2); tokens(t, [&](std::string_view w) { encodeRank(v.rank[w], c); }); codedBytes += c.size(); coded.push_back(std::move(c)); }
    const double buildMs = ms(t0);
    size_t vocabBytes = 0; for (auto& w : v.words) vocabBytes += w.size() + 4; // text + offset
    std::printf("%zu files, text %.1f MB -> codes %.1f MB (%.1f%%) + vocabulary %zu words %.1f MB = %.1f%%  (built in %.0f ms)\n",
                texts.size(), bytes / 1048576.0, codedBytes / 1048576.0, 100.0 * codedBytes / bytes, v.words.size(),
                vocabBytes / 1048576.0, 100.0 * (codedBytes + vocabBytes) / bytes, buildMs);

    // Search a whole word: in place on the text vs its codeword in the codes (a match must start
    // right after a codeword's last byte, which the tag bit tells).
    auto searchPlain = [&](const std::string& q, bool word) {
        size_t n = 0;
        for (auto& t : texts) {
            for (size_t at = t.find(q); at != std::string::npos; at = t.find(q, at + 1))
                if (!word || ((at == 0 || !wordChar((unsigned char)t[at - 1])) && (at + q.size() == t.size() || !wordChar((unsigned char)t[at + q.size()])))) ++n;
        }
        return n;
    };
    auto searchCodes = [&](const std::vector<std::string>& codewords) {
        size_t n = 0;
        for (auto& c : coded)
            for (auto& cw : codewords)
                for (size_t at = c.find(cw); at != std::string::npos; at = c.find(cw, at + 1))
                    if (at == 0 || (unsigned char)c[at - 1] & 0x80) ++n;
        return n;
    };
    for (const char* q : {"ImGui", "memory", "Scope", "size_t"}) {
        auto a = Clock::now(); size_t hp = searchPlain(q, true); double mp = ms(a);
        auto b = Clock::now(); std::string cw; auto it = v.rank.find(q); if (it != v.rank.end()) encodeRank(it->second, cw);
        size_t hc = cw.empty() ? 0 : searchCodes({cw}); double mc = ms(b);
        std::printf("  whole word %-8s  text in place %6.1f ms (%zu)   codes %6.1f ms (%zu), codeword %zu byte(s)\n", q, mp, hp, mc, hc, cw.size());
    }
    // A substring ("emor" in "memory"): every vocabulary word containing it, then all their codewords.
    for (const char* q : {"emor", "Imgui"}) {
        auto a = Clock::now(); size_t hp = searchPlain(q, false); double mp = ms(a);
        auto b = Clock::now(); std::vector<std::string> cws;
        for (uint32_t i = 0; i < v.words.size(); ++i) if (v.words[i].find(q) != std::string::npos) { std::string cw; encodeRank(i, cw); cws.push_back(cw); }
        double scanMs = ms(b);
        size_t hc = searchCodes(cws); double mc = ms(b);
        // One pass: decode each codeword's rank and test a bitmap of the matching words.
        auto e = Clock::now();
        std::vector<bool> hit(v.words.size());
        for (uint32_t i = 0; i < v.words.size(); ++i) if (v.words[i].find(q) != std::string::npos) hit[i] = true;
        size_t h1 = 0;
        for (auto& c : coded) {
            uint32_t r = 0, base = 0, span = 128;
            for (unsigned char b : c) {
                if (b & 0x80) { if (hit[r * 128 + (b & 0x7f) + base]) ++h1; r = 0; base = 0; span = 128; }
                else { r = r * 128 + b; base += span; span *= 128; }
            }
        }
        double m1 = ms(e);
        std::printf("  substring  %-8s  text in place %6.1f ms (%zu)   codes, codeword by codeword %7.1f ms   codes, one pass + bitmap %6.1f ms (%zu words, %zu occurrences)\n", q, mp, hp, mc, m1, cws.size(), h1);
    }
    // Drawing needs text: decode a screen of 60 lines from the largest file, and the whole tree.
    auto decode = [&](const std::string& c, std::string& out) {
        out.clear(); uint32_t r = 0, base = 0, span = 128;
        for (unsigned char b : c) {
            if (b & 0x80) { r = r * 128 + (b & 0x7f); out += v.words[r + base]; r = 0; base = 0; span = 128; }
            else { r = r * 128 + b; base += span; span *= 128; }
        }
    };
    // (rank decode check on the first file)
    std::string back; decode(coded[0], back);
    std::printf("  decode check: %s\n", back == texts[0] ? "identical" : "MISMATCH");
    auto d = Clock::now(); size_t out = 0; for (auto& c : coded) { decode(c, back); out += back.size(); }
    double dm = ms(d);
    std::printf("  decode the whole tree: %.0f ms (%.0f MB/s); a 60-line screen of about 3 KB: %.1f us\n", dm, out / 1048576.0 / (dm / 1000), dm * 1000 * 3072 / out);
}
