// Part of tools/text-study (docs/plan/24-compact-text.md). Build from the repository root after a Linux build:
//   g++ -O2 -std=c++17 -Iinclude -o index_parts tools/text-study/index_parts.cpp build/linux/libenp_model.a build/linux/libenp_os.a -lpthread -ldl
// What the IntelliSense index of a tree is made of: the same extraction the app runs, then the
// parts' sizes, and what the declaring lines would cost compressed.
#include "enp/model/Language.h"
#include "enp/model/Symbols.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>
namespace fs = std::filesystem;
using namespace enp::model;
int main(int argc, char** argv) {
    size_t files = 0, sourceBytes = 0, symbols = 0, nameBytes = 0, containerBytes = 0, detailBytes = 0;
    std::set<std::string> distinctNames, distinctContainers, distinctDetails;
    size_t pathBytes = 0;
    std::vector<Symbol> out;
    for (auto& e : fs::recursive_directory_iterator(argv[1], fs::directory_options::skip_permission_denied)) {
        if (!e.is_regular_file() || e.file_size() > 2 * 1024 * 1024 || e.path().string().find("/.") != std::string::npos) continue;
        const Language lang = languageFromPath(e.path().string());
        if (lang == Language::PlainText) continue;
        std::ifstream f(e.path(), std::ios::binary); std::string s((std::istreambuf_iterator<char>(f)), {});
        if (s.find('\0') != std::string::npos) continue;
        out.clear();
        extractSymbols(s, lang, int(files), out);
        ++files; sourceBytes += s.size(); pathBytes += fs::relative(e.path(), argv[1]).string().size();
        for (const Symbol& sym : out) {
            ++symbols; nameBytes += sym.name.size(); containerBytes += sym.container.size(); detailBytes += sym.detail.size();
            distinctNames.insert(sym.name); distinctContainers.insert(sym.container); distinctDetails.insert(sym.detail);
        }
    }
    auto sum = [](const std::set<std::string>& s) { size_t n = 0; for (auto& x : s) n += x.size(); return n; };
    std::printf("%zu files with symbols, %.1f MB of source, %zu declarations\n", files, sourceBytes / 1048576.0, symbols);
    std::printf("  entries (32 bytes each)           %8.0f KB\n", symbols * 32 / 1024.0);
    std::printf("  names            %8.0f KB  (distinct %7zu, %6.0f KB if each stored once)\n", nameBytes / 1024.0, distinctNames.size(), sum(distinctNames) / 1024.0);
    std::printf("  containers       %8.0f KB  (distinct %7zu, %6.0f KB if each stored once)\n", containerBytes / 1024.0, distinctContainers.size(), sum(distinctContainers) / 1024.0);
    std::printf("  declaring lines  %8.0f KB  (distinct %7zu, %6.0f KB if each stored once)\n", detailBytes / 1024.0, distinctDetails.size(), sum(distinctDetails) / 1024.0);
    std::printf("  paths            %8.0f KB\n", pathBytes / 1024.0);
    // The declaring lines (each distinct one once) in End-Tagged Dense Code, own vocabulary.
    std::unordered_map<std::string, size_t> freq;
    auto words = [](const std::string& t, auto&& f) {
        size_t i = 0;
        while (i < t.size()) {
            auto w = [](unsigned char c) { return std::isalnum(c) || c == '_' || c >= 0x80; };
            size_t j = i; const bool k = w((unsigned char)t[i]);
            while (j < t.size() && w((unsigned char)t[j]) == k) ++j;
            f(t.substr(i, j - i)); i = j;
        }
    };
    for (auto& d : distinctDetails) words(d, [&](std::string w) { ++freq[w]; });
    std::vector<size_t> ranked; size_t vocab = 0;
    for (auto& [w, n] : freq) { ranked.push_back(n); vocab += w.size() + 4; }
    std::sort(ranked.rbegin(), ranked.rend());
    size_t coded = 0;
    for (size_t i = 0; i < ranked.size(); ++i) coded += ranked[i] * (i < 128 ? 1 : i < 128 + 128 * 128 ? 2 : 3);
    std::printf("  declaring lines in ETDC: codes %.0f KB + vocabulary %zu words %.0f KB = %.0f KB (%.0f%% of %.0f KB distinct)\n",
                coded / 1024.0, ranked.size(), vocab / 1024.0, (coded + vocab) / 1024.0, 100.0 * (coded + vocab) / sum(distinctDetails), sum(distinctDetails) / 1024.0);
}
