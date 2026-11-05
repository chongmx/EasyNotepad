// Part of tools/text-study (docs/plan/24-compact-text.md). Build: g++ -O2 -std=c++17 -o substring_bench substring_bench.cpp;
// run: ./substring_bench <tree> [block bytes, 1024] [camel: split identifiers into pieces]
// Substring search over End-Tagged Dense Code with similarity baked in: (1) a trigram signature per
// block of codes lets a query skip blocks that cannot hold it; (2) a trigram index of the vocabulary
// finds the matching words without scanning it; (3) per word, the blocks it occurs in (an inverted
// list) jumps straight to them. Measured against the plain text scanned in place.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
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
static bool g_camel = false;
// Words, or with g_camel the pieces of identifiers: "TreeNodeEx" -> Tree Node Ex, "get_width" -> get _ width,
// "HTTPServer" -> HTTP Server, digits apart from letters.
template <class F> static void tokens(std::string_view t, F&& f) {
    size_t i = 0;
    while (i < t.size()) {
        size_t j = i; const bool w = wordChar((unsigned char)t[i]);
        while (j < t.size() && wordChar((unsigned char)t[j]) == w) ++j;
        if (!w || !g_camel) { f(t.substr(i, j - i)); i = j; continue; }
        size_t a = i;
        for (size_t k = i + 1; k <= j; ++k) {
            auto cls = [&](size_t x) { unsigned char c = (unsigned char)t[x]; return c == '_' ? 0 : std::isdigit(c) ? 1 : std::isupper(c) ? 2 : 3; };
            bool cut = k == j;
            if (!cut) {
                int p = cls(k - 1), c = cls(k);
                cut = (p != c && !(p == 2 && c == 3)) || (p == 2 && c == 2 && k + 1 < j && cls(k + 1) == 3);
            }
            if (cut) { f(t.substr(a, k - a)); a = k; }
        }
        i = j;
    }
}
static void encodeRank(uint32_t i, std::string& out) {
    unsigned char buf[4]; int n = 0; buf[n++] = 0x80 | (i % 128); i /= 128;
    while (i > 0) { --i; buf[n++] = i % 128; i /= 128; }
    for (int k = n - 1; k >= 0; --k) out += char(buf[k]);
}
static uint32_t tri(unsigned char a, unsigned char b, unsigned char c) { return (uint32_t(a) * 0x9E3779B1u ^ uint32_t(b) * 0x85EBCA77u ^ uint32_t(c) * 0xC2B2AE3Du) ; }
constexpr int kSigBits = 512;              // signature per block
using Sig = std::array<uint64_t, kSigBits / 64>;
static void sigAdd(Sig& s, uint32_t h) { h %= kSigBits; s[h / 64] |= 1ull << (h % 64); }
int main(int argc, char** argv) {
    const size_t kBlock = argc > 2 ? size_t(std::atoi(argv[2])) : 1024;
    g_camel = argc > 3 && std::string(argv[3]) == "camel";
    std::vector<std::string> texts; size_t bytes = 0;
    for (auto& e : fs::recursive_directory_iterator(argv[1], fs::directory_options::skip_permission_denied))
        if (e.is_regular_file() && e.file_size() <= 2 * 1024 * 1024 && e.path().string().find("/.") == std::string::npos) {
            std::ifstream f(e.path(), std::ios::binary); std::string s((std::istreambuf_iterator<char>(f)), {});
            if (s.find('\0') == std::string::npos) { bytes += s.size(); texts.push_back(std::move(s)); }
        }
    std::unordered_map<std::string, uint64_t> freq;
    for (auto& t : texts) tokens(t, [&](std::string_view w) { ++freq[std::string(w)]; });
    std::vector<std::pair<uint64_t, std::string>> v2; v2.reserve(freq.size());
    for (auto& [w, n] : freq) v2.push_back({n, w});
    std::sort(v2.begin(), v2.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::string> words; std::unordered_map<std::string_view, uint32_t> rank;
    for (auto& p : v2) words.push_back(p.second);
    for (uint32_t i = 0; i < words.size(); ++i) rank[words[i]] = i;
    // One stream of codes; blocks end on a codeword boundary.
    std::string codes; codes.reserve(bytes / 2);
    std::vector<uint32_t> blockStart{0};
    for (auto& t : texts) tokens(t, [&](std::string_view w) {
        encodeRank(rank[w], codes);
        if (codes.size() - blockStart.back() >= kBlock) blockStart.push_back(uint32_t(codes.size()));
    });
    blockStart.push_back(uint32_t(codes.size()));
    const size_t blocks = blockStart.size() - 1;
    // (1) Signatures: trigrams of the decoded text of each block (across word edges too).
    // (3) Inverted lists: for each word, the blocks it occurs in (delta varints).
    std::vector<Sig> sigs(blocks);
    std::vector<std::vector<uint32_t>> occurs(words.size());
    auto forEachRank = [&](size_t b, auto&& f) {
        uint32_t r = 0, base = 0, span = 128;
        for (size_t i = blockStart[b]; i < blockStart[b + 1]; ++i) {
            const unsigned char c = (unsigned char)codes[i];
            if (c & 0x80) { f(r * 128 + (c & 0x7f) + base); r = 0; base = 0; span = 128; } else { r = r * 128 + c; base += span; span *= 128; }
        }
    };
    auto t0 = Clock::now();
    for (size_t b = 0; b < blocks; ++b) {
        unsigned char p1 = 0, p2 = 0; int have = 0;
        forEachRank(b, [&](uint32_t r) {
            if (occurs[r].empty() || occurs[r].back() != b) occurs[r].push_back(uint32_t(b));
            for (unsigned char c : words[r]) { if (have >= 2) sigAdd(sigs[b], tri(p1, p2, c)); p1 = p2; p2 = c; ++have; }
        });
    }
    size_t invertedBytes = 0;
    for (auto& l : occurs) { uint32_t prev = 0; for (uint32_t b : l) { uint32_t d = b - prev; prev = b; do { ++invertedBytes; d >>= 7; } while (d); } }
    // (2) Trigram index of the vocabulary: trigram -> words containing it (sorted ranks).
    std::unordered_map<uint32_t, std::vector<uint32_t>> vocabTri;
    size_t vocabTriBytes = 0;
    for (uint32_t r = 0; r < words.size(); ++r) {
        const std::string& w = words[r];
        for (size_t i = 0; i + 3 <= w.size(); ++i) { auto& l = vocabTri[tri(w[i], w[i + 1], w[i + 2])]; if (l.empty() || l.back() != r) { l.push_back(r); vocabTriBytes += 3; } }
    }
    const double buildMs = ms(t0);
    size_t vocabBytes = 0; for (auto& w : words) vocabBytes += w.size() + 4;
    std::printf("%s: %zu words, vocabulary %.1f MB, codes %.1f MB, inverted %.1f MB -> codes + vocabulary + inverted %.1f%% of the text\n", g_camel ? "identifier pieces" : "whole words",
                words.size(), vocabBytes / 1048576.0, codes.size() / 1048576.0, invertedBytes / 1048576.0, 100.0 * (codes.size() + vocabBytes + invertedBytes) / bytes);
    std::printf("text %.1f MB, codes %.1f MB in %zu blocks of ~%zu B; signatures %.0f KB (%.1f%% of codes), "
                "inverted lists %.0f KB, vocabulary trigram index ~%.0f KB (built in %.0f ms)\n",
                bytes / 1048576.0, codes.size() / 1048576.0, blocks, kBlock, blocks * sizeof(Sig) / 1024.0,
                100.0 * blocks * sizeof(Sig) / codes.size(), invertedBytes / 1024.0, vocabTriBytes / 1024.0, buildMs);
    for (const char* q : {"emor", "Imgui", "ndow", "Breadcrumb", "ollapsi", "TreeNodeEx", "utf8"}) {
        const std::string qs = q;
        auto a = Clock::now(); size_t plain = 0;
        for (auto& t : texts) for (size_t at = t.find(qs); at != std::string::npos; at = t.find(qs, at + 1)) ++plain;
        const double plainMs = ms(a);
        // Matching words: vocabulary trigram index, then verify.
        auto b = Clock::now();
        std::vector<uint32_t> cand;
        if (qs.size() >= 3) {
            const std::vector<uint32_t>* smallest = nullptr;
            for (size_t i = 0; i + 3 <= qs.size(); ++i) { auto it = vocabTri.find(tri(qs[i], qs[i + 1], qs[i + 2])); if (it == vocabTri.end()) { smallest = nullptr; cand.clear(); goto none; } if (!smallest || it->second.size() < smallest->size()) smallest = &it->second; }
            for (uint32_t r : *smallest) if (words[r].find(qs) != std::string::npos) cand.push_back(r);
        }
        none:
        const double vocabMs = ms(b);
        std::vector<char> hit(words.size(), 0); for (uint32_t r : cand) hit[r] = 1;
        auto countIn = [&](size_t blk) { size_t n = 0; forEachRank(blk, [&](uint32_t r) { if (hit[r]) { size_t at = 0; while ((at = words[r].find(qs, at)) != std::string::npos) { ++n; ++at; } } }); return n; };
        // (a) every block, one pass
        auto c = Clock::now(); size_t n1 = 0; for (size_t blk = 0; blk < blocks; ++blk) n1 += countIn(blk); const double fullMs = ms(c);
        // (b) blocks whose signature holds every trigram of the query
        auto d = Clock::now(); Sig need{}; for (size_t i = 0; i + 3 <= qs.size(); ++i) sigAdd(need, tri(qs[i], qs[i + 1], qs[i + 2]));
        size_t n2 = 0, visited = 0;
        for (size_t blk = 0; blk < blocks; ++blk) {
            bool ok = true; for (int k = 0; k < kSigBits / 64; ++k) if ((sigs[blk][k] & need[k]) != need[k]) { ok = false; break; }
            if (ok) { ++visited; n2 += countIn(blk); }
        }
        const double sigMs = ms(d);
        // (c) inverted lists of the matching words: union of their blocks
        auto e = Clock::now(); std::vector<uint32_t> bl; for (uint32_t r : cand) bl.insert(bl.end(), occurs[r].begin(), occurs[r].end());
        std::sort(bl.begin(), bl.end()); bl.erase(std::unique(bl.begin(), bl.end()), bl.end());
        size_t n3 = 0; for (uint32_t blk : bl) n3 += countIn(blk);
        const double invMs = ms(e);
        std::printf("  %-11s hits %5zu/%5zu/%5zu/%5zu  text in place %6.1f ms | words %4zu (vocab %.2f ms) | codes all blocks %6.1f ms | signatures %6.1f ms (%5.1f%% of blocks) | inverted %6.1f ms (%5.1f%% of blocks)\n",
                    q, plain, n1, n2, n3, plainMs, cand.size(), vocabMs, fullMs + vocabMs, sigMs + vocabMs, 100.0 * visited / blocks, invMs + vocabMs, 100.0 * bl.size() / blocks);
    }
}
