#include "enp/viewmodel/DummyData.h"

#include "enp/model/TextBuffer.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace enp::viewmodel::dummy {

namespace {

constexpr const char* kMainCpp = R"(// EasyDemo: prints the most frequent words of a text file.
#include "TextUtils.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct Options {
    std::string inputPath;
    int topN = 10;
    bool ignoreCase = true;
};

Options parseArguments(int argc, char** argv)
{
    Options options;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc)
            options.topN = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--case-sensitive") == 0)
            options.ignoreCase = false;
        else
            options.inputPath = argv[i];
    }
    return options;
}

void printReport(const std::map<std::string, int>& counts, int topN)
{
    std::vector<std::pair<std::string, int>> sorted(counts.begin(), counts.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    char line[64];
    for (int i = 0; i < topN && i < (int)sorted.size(); ++i) {
        sprintf(line, "%-20s %d", sorted[i].first.c_str(), sorted[i].second);
        std::cout << line << "\n";
    }
}

} // namespace

int main(int argc, char** argv)
{
    Options options = parseArguments(argc, argv);
    if (options.inputPath.empty()) {
        std::cerr << "usage: easydemo [--top N] [--case-sensitive] <file>\n";
        return 1;
    }

    std::ifstream input(options.inputPath);
    if (!input) {
        std::cerr << "cannot open " << options.inputPath << "\n";
        return 1;
    }

    /* Count every word, normalised according to the options. */
    std::map<std::string, int> counts;
    std::string word;
    while (input >> word) {
        word = text::stripPunctuation(word);
        if (options.ignoreCase)
            word = text::toLower(word);
        if (word.size() > 0)
            counts[word]++;
    }

    printReport(counts, options.topN);
    return 0;
}
)";

constexpr const char* kTextUtilsH = R"(// Small string helpers used by the word counter.
#pragma once

#include <string>

namespace text {

std::string toLower(const std::string& s);
std::string stripPunctuation(const std::string& s);

} // namespace text
)";

constexpr const char* kTextUtilsCpp = R"(#include "TextUtils.h"

#include <cctype>

namespace text {

std::string toLower(const std::string& s)
{
    std::string out = s;
    for (size_t i = 0; i < out.size(); i++)
        out[i] = std::tolower(out[i]);
    return out;
}

std::string stripPunctuation(const std::string& s)
{
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && std::ispunct(s[begin]))
        begin++;
    while (end > begin && std::ispunct(s[end - 1]))
        end--;
    return s.substr(begin, end - begin);
}

} // namespace text
)";

constexpr const char* kBuildIndexPy = R"(#!/usr/bin/env python3
"""Builds a word-count index for the sample corpus."""
import json
import os
import subprocess
import sys


def list_documents(root):
    documents = []
    for dirpath, _, filenames in os.walk(root):
        for name in filenames:
            if name.endswith(".txt"):
                documents.append(os.path.join(dirpath, name))
    return documents


def word_count(path):
    output = subprocess.check_output("wc -w " + path, shell=True)
    return int(output.split()[0])


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "corpus"
    index = {path: word_count(path) for path in list_documents(root)}
    with open("index.json", "w") as f:
        json.dump(index, f, indent=2)
    print("indexed %d documents in %s, writing the result to index.json in the current directory" % (len(index), root))


if __name__ == "__main__":
    main()
)";

constexpr const char* kConfigJson = R"({
  "corpus": "corpus",
  "topN": 20,
  "caseSensitive": false,
  "telemetry": {
    "endpoint": "https://example.invalid/collect",
    "api_key": "sk-demo-0000000000000000"
  }
}
)";

constexpr const char* kCMakeLists = R"(cmake_minimum_required(VERSION 3.20)
project(EasyDemo CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(easydemo src/main.cpp src/TextUtils.cpp)

enable_testing()
add_executable(easydemo_tests tests/TextUtilsTest.cpp src/TextUtils.cpp)
add_test(NAME easydemo_tests COMMAND easydemo_tests)
)";

constexpr const char* kReadme = R"(# EasyDemo

A tiny word-frequency counter used as the sample workspace of EasyNotepad.

## Usage

    easydemo [--top N] [--case-sensitive] <file>

## Layout

- `src/main.cpp` - command line and report
- `src/TextUtils.*` - string helpers
- `scripts/build_index.py` - corpus indexer
)";

constexpr const char* kWelcome = R"(# Welcome to EasyNotepad

EasyNotepad is a native text editor with two workspaces. Switch with the
**Workspace** block on the left of the ribbon, or press Ctrl+Shift+M.

## Agent workspace

Describe what you want in the conversation. The agent plans the work, edits
files and runs the review tools; you review each proposed change side by side
and accept or reject it.

## Code workspace

A classic editor in the spirit of Notepad++: tabs, line operations,
bookmarks, find, encodings and line endings. The agent stays available in
a side panel for questions about the code you are looking at.

## Review tools

- Lint, Static Analysis, Security, Style and Complexity checks
- AI review of the change
- Side-by-side diff against the saved file or the agent's proposal
- Inline review comments and a review checklist

> This build runs on dummy data: files, the agent and the review tools
> are not connected yet.
)";

constexpr const char* kTextUtilsTest = R"(#include "../src/TextUtils.h"

#include <cassert>
#include <cstdio>

int main()
{
    assert(text::toLower("HeLLo") == "hello");
    assert(text::toLower("\xC3\x84pfel") == "\xC3\x84pfel"); // non-ASCII bytes stay untouched
    assert(text::stripPunctuation("(word),") == "word");
    assert(text::stripPunctuation("...") == "");
    assert(text::stripPunctuation("") == "");
    std::puts("TextUtils: all tests passed");
    return 0;
}
)";

std::string replaceAll(std::string s, const std::string& from, const std::string& to)
{
    for (size_t pos = s.find(from); pos != std::string::npos; pos = s.find(from, pos + to.size()))
        s.replace(pos, from.size(), to);
    return s;
}

model::ReviewFinding finding(model::ReviewTool tool, model::Severity severity, const std::string& file,
                             const std::string& marker, const char* rule, const char* message)
{
    model::ReviewFinding f;
    f.tool = tool;
    f.severity = severity;
    f.file = file;
    f.line = lineOf(file, marker);
    f.rule = rule;
    f.message = message;
    return f;
}

model::FileChange change(const std::string& path, std::string before, std::string after)
{
    model::FileChange c;
    c.path = path;
    c.before = std::move(before);
    c.after = std::move(after);
    c.diff = model::diffLines(c.before.empty() ? std::vector<std::string>() : model::splitLines(c.before),
                              model::splitLines(c.after));
    c.stats = model::diffStats(c.diff);
    return c;
}

} // namespace

ExplorerItem workspace()
{
    auto file = [](const char* path) {
        std::string p = path;
        return ExplorerItem{p.substr(p.rfind('/') + 1), p, false, {}};
    };
    auto dir = [](const char* name, std::vector<ExplorerItem> children) {
        return ExplorerItem{name, name, true, std::move(children)};
    };
    return dir(kWorkspaceName, {
        dir("docs", {file("docs/README.md")}),
        dir("scripts", {file("scripts/build_index.py")}),
        dir("src", {file("src/main.cpp"), file("src/TextUtils.cpp"), file("src/TextUtils.h")}),
        file("CMakeLists.txt"),
        file("config.json"),
    });
}

std::string fileContent(const std::string& path)
{
    if (path == "src/main.cpp")
        return kMainCpp;
    if (path == "src/TextUtils.h")
        return kTextUtilsH;
    if (path == "src/TextUtils.cpp")
        return kTextUtilsCpp;
    if (path == "scripts/build_index.py")
        return kBuildIndexPy;
    if (path == "config.json")
        return kConfigJson;
    if (path == "CMakeLists.txt")
        return kCMakeLists;
    if (path == "docs/README.md")
        return kReadme;
    if (path == kWelcomePath)
        return kWelcome;
    if (path == "tests/TextUtilsTest.cpp")
        return kTextUtilsTest;
    return "This is placeholder content for\n    " + path + "\n\nFile loading arrives with the document service.\n";
}

int lineOf(const std::string& path, const std::string& marker)
{
    const model::TextBuffer buffer(fileContent(path));
    for (int i = 0; i < buffer.lineCount(); ++i)
        if (buffer.line(i).find(marker) != std::string::npos)
            return i;
    return 0;
}

std::vector<model::ReviewFinding> findings(model::ReviewTool tool)
{
    using model::ReviewTool;
    using model::Severity;
    const std::string main = "src/main.cpp", utils = "src/TextUtils.cpp", py = "scripts/build_index.py";
    switch (tool) {
    case ReviewTool::Lint:
        return {
            finding(tool, Severity::Warning, main, "std::atoi", "cert-err34-c",
                    "'atoi' does not report conversion errors; use std::from_chars or std::stoi"),
            finding(tool, Severity::Warning, main, "std::sort(", "misc-include-cleaner",
                    "'std::sort' is used without including <algorithm>"),
            finding(tool, Severity::Info, main, "(int)sorted.size()", "cppcoreguidelines-pro-type-cstyle-cast",
                    "C-style cast; use static_cast<int>"),
            finding(tool, Severity::Info, main, "word.size() > 0", "readability-container-size-empty",
                    "use '!word.empty()' to check for emptiness"),
        };
    case ReviewTool::StaticAnalysis:
        return {
            finding(tool, Severity::Error, main, "sprintf(line", "bufferOverrun",
                    "'%-20s' writes an unbounded word into the 64-byte buffer 'line'"),
            finding(tool, Severity::Warning, utils, "std::tolower(out[i])", "charSignedness",
                    "passing a possibly negative 'char' to std::tolower is undefined; cast to unsigned char"),
            finding(tool, Severity::Warning, utils, "std::ispunct(s[begin])", "charSignedness",
                    "passing a possibly negative 'char' to std::ispunct is undefined; cast to unsigned char"),
        };
    case ReviewTool::Security:
        return {
            finding(tool, Severity::Error, py, "shell=True", "B602 subprocess-shell-injection",
                    "file name is concatenated into a shell command; pass an argument list without shell=True"),
            finding(tool, Severity::Error, "config.json", "api_key", "hardcoded-secret",
                    "a credential-like value is committed in plain text"),
            finding(tool, Severity::Warning, main, "sprintf(line", "unsafe-api",
                    "'sprintf' has no bounds check; use snprintf or std::format"),
        };
    case ReviewTool::Style:
        return {
            finding(tool, Severity::Info, main, "#include \"TextUtils.h\"", "include-order",
                    "project header placed before system headers differs from the configured include order"),
            finding(tool, Severity::Info, py, "print(\"indexed", "E501 line-too-long",
                    "line exceeds 88 characters"),
            finding(tool, Severity::Info, utils, "i++)", "prefer-pre-increment",
                    "prefer '++i' in loop increments"),
        };
    case ReviewTool::Complexity:
        return {
            finding(tool, Severity::Warning, main, "int main(", "function-complexity",
                    "'main' has cyclomatic complexity 7 and 30 lines (threshold 5 / 25)"),
            finding(tool, Severity::Info, main, "Options parseArguments", "function-complexity",
                    "'parseArguments' has cyclomatic complexity 5"),
        };
    case ReviewTool::AgentReview:
        return {
            finding(tool, Severity::Warning, main, "options.inputPath = argv[i]", "logic",
                    "a second positional argument silently replaces the first input path"),
            finding(tool, Severity::Info, main, "a.second > b.second", "determinism",
                    "words with equal counts are printed in unspecified order; break ties by word"),
            finding(tool, Severity::Info, main, "printReport(counts", "edge-case",
                    "an empty input file prints nothing; consider reporting '0 words'"),
        };
    }
    return {};
}

std::vector<model::ReviewComment> comments()
{
    return {
        {"Dana", "src/main.cpp", lineOf("src/main.cpp", "char line[64]"),
         "Can we drop the fixed buffer entirely and stream with std::setw?", false},
        {"Agent", "src/TextUtils.cpp", lineOf("src/TextUtils.cpp", "std::string toLower"),
         "toLower is ASCII only; fine for now, but worth a note in the header.", false},
        {"You", "scripts/build_index.py", lineOf("scripts/build_index.py", "def word_count"),
         "Could count words in Python instead of shelling out to wc.", true},
    };
}

std::vector<model::ChecklistItem> checklist()
{
    return {
        {"Change builds without warnings", true},
        {"Unit tests cover the new behaviour", false},
        {"No secrets or credentials committed", false},
        {"Error paths are handled and reported", false},
        {"Public functions are documented", true},
        {"Review findings are resolved or justified", false},
    };
}

std::vector<model::ChatMessage> conversation()
{
    using model::ChatRole;
    return {
        {ChatRole::User, "The word counter crashes on very long words and --top accepts garbage. "
                         "Can you harden it and add tests?"},
        {ChatRole::Agent, "I read src/main.cpp and src/TextUtils.cpp. Three problems:\n"
                          "1. printReport() uses sprintf into a 64-byte buffer, so long words overflow it.\n"
                          "2. --top is parsed with atoi, which turns garbage into 0 silently.\n"
                          "3. tolower/ispunct receive plain char, which is undefined for non-ASCII bytes."},
        {ChatRole::Tool, "Ran Lint, Static Analysis and Security: 3 errors, 6 warnings."},
        {ChatRole::Agent, "I've proposed changes to 3 files: bounded formatting, validated --top, "
                          "unsigned char casts, and a new tests/TextUtilsTest.cpp. "
                          "Review them on the right and accept what you like."},
    };
}

std::vector<model::AgentTask> tasks()
{
    using model::TaskState;
    return {
        {"Read main.cpp and TextUtils.cpp", TaskState::Done},
        {"Replace sprintf with bounded output", TaskState::Done},
        {"Validate the --top argument", TaskState::Done},
        {"Fix char signedness in TextUtils", TaskState::Done},
        {"Add unit tests for TextUtils", TaskState::Running},
        {"Build, run tests and re-run reviews", TaskState::Pending},
    };
}

std::vector<model::FileChange> changes()
{
    std::string main = kMainCpp;
    std::string newMain = replaceAll(main, "#include <cstdio>\n", "#include <algorithm>\n#include <charconv>\n");
    newMain = replaceAll(newMain, "#include <iostream>\n", "#include <iomanip>\n#include <iostream>\n");
    newMain = replaceAll(newMain, "            options.topN = std::atoi(argv[++i]);\n",
                         "        {\n"
                         "            const char* value = argv[++i];\n"
                         "            auto [end, error] = std::from_chars(value, value + std::strlen(value), options.topN);\n"
                         "            if (error != std::errc() || *end != '\\0' || options.topN <= 0)\n"
                         "                options.topN = 10;\n"
                         "        }\n");
    newMain = replaceAll(newMain, "    char line[64];\n", "");
    newMain = replaceAll(newMain, "(int)sorted.size()", "static_cast<int>(sorted.size())");
    newMain = replaceAll(newMain,
                         "        sprintf(line, \"%-20s %d\", sorted[i].first.c_str(), sorted[i].second);\n"
                         "        std::cout << line << \"\\n\";\n",
                         "        std::cout << std::left << std::setw(20) << sorted[i].first << ' ' << sorted[i].second << '\\n';\n");
    newMain = replaceAll(newMain, "word.size() > 0", "!word.empty()");

    std::string utils = kTextUtilsCpp;
    std::string newUtils = replaceAll(utils, "out[i] = std::tolower(out[i]);",
                                      "out[i] = char(std::tolower(static_cast<unsigned char>(out[i])));");
    newUtils = replaceAll(newUtils, "std::ispunct(s[begin])", "std::ispunct(static_cast<unsigned char>(s[begin]))");
    newUtils = replaceAll(newUtils, "std::ispunct(s[end - 1])", "std::ispunct(static_cast<unsigned char>(s[end - 1]))");
    newUtils = replaceAll(newUtils, "i++)", "++i)");

    return {
        change("src/main.cpp", main, newMain),
        change("src/TextUtils.cpp", utils, newUtils),
        change("tests/TextUtilsTest.cpp", "", kTextUtilsTest),
    };
}

std::string summarize(const std::string& path, const model::OutlineNode& node, const model::TextBuffer& buffer,
                      const std::vector<std::string>& parts)
{
    static const std::pair<const char*, const char*> canned[] = {
        {"struct Options", "Command-line settings: input path, how many words to show (default 10) and case folding."},
        {"Options parseArguments", "Reads --top N and --case-sensitive; any other argument becomes the input path. --top uses atoi, so bad numbers silently become 0."},
        {"void printReport", "Sorts words by count, highest first, and prints the top N as aligned columns through a 64-byte sprintf buffer."},
        {"int main", "Checks the arguments, opens the file, counts normalised words in a std::map, then prints the report; exits with 1 on usage or open errors."},
        {"std::string toLower", "Returns a lower-case copy, converting each byte with std::tolower (ASCII only)."},
        {"std::string stripPunctuation", "Trims punctuation from both ends of a word and returns the middle."},
        {"namespace text", "String helpers used by the word counter: toLower and stripPunctuation."},
        {"def list_documents", "Walks the corpus folder and collects the path of every .txt file."},
        {"def word_count", "Counts words by running `wc -w` through the shell, which is unsafe for untrusted paths."},
        {"def main", "Indexes every document's word count into index.json and prints a one-line summary."},
        {"if __name__", "Runs main() when the file is executed as a script."},
        {"Welcome to EasyNotepad", "Introduces the two workspaces, the reading tools and the review tools."},
        {"Agent workspace", "Describe the goal; the agent plans, edits and reviews, and you accept each change."},
        {"Code workspace", "Classic editing in the spirit of Notepad++, with the agent in a side panel."},
        {"Review tools", "Analyzers, AI review, side-by-side diffs, inline comments and a checklist."},
    };
    if (!node.hint.empty())
        return node.hint; // the author's own explanation (a step's leading comment)
    for (const auto& [key, text] : canned)
        if (node.title.find(key) != std::string::npos)
            return text;
    if (node.title.rfind("namespace", 0) == 0 && path == "src/main.cpp")
        return "File-local helpers: the Options struct, argument parsing and the frequency report.";

    const int body = node.endLine - node.startLine;
    // Real code usually explains itself in the comment right above a definition: use it.
    {
        std::vector<std::string> comment;
        for (int l = node.startLine - 1; l >= 0 && l >= node.startLine - 8; --l) {
            std::string t = buffer.line(l);
            const size_t b = t.find_first_not_of(" 	");
            if (b == std::string::npos)
                break;
            t = t.substr(b);
            if (t.rfind("//", 0) == 0)
                t = t.substr(t.rfind("///", 0) == 0 ? 3 : 2);
            else if (t.rfind("#", 0) == 0 && node.kind != model::OutlineKind::Section)
                t = t.substr(1);
            else
                break;
            const size_t c = t.find_first_not_of(" ");
            comment.insert(comment.begin(), c == std::string::npos ? std::string() : t.substr(c));
        }
        std::string text;
        for (const std::string& part : comment)
            if (!part.empty() && part.find("----") == std::string::npos)
                text += (text.empty() ? "" : " ") + part;
        if (!text.empty()) {
            if (text.size() > 160)
                text = text.substr(0, 157) + "...";
            return text;
        }
    }
    if ((node.kind == model::OutlineKind::Namespace || node.kind == model::OutlineKind::Type) && !parts.empty()) {
        std::string out = std::to_string(parts.size()) + (parts.size() == 1 ? " part: " : " parts: ");
        for (size_t i = 0; i < parts.size() && i < 4; ++i)
            out += (i ? ", " : "") + parts[i];
        return out + (parts.size() > 4 ? ", ..." : ".");
    }
    if (node.kind == model::OutlineKind::Section) {
        for (int l = node.startLine + 1; l <= node.endLine; ++l) {
            std::string text = buffer.line(l);
            if (text.empty() || text[0] == '#')
                continue;
            if (text.size() > 110)
                text = text.substr(0, 107) + "...";
            return text;
        }
        return std::to_string(body) + " lines.";
    }
    // Heuristic stand-in: the functions it calls.
    static const std::set<std::string> keywords = {"if", "for", "while", "switch", "return", "sizeof", "catch", "print"};
    std::vector<std::string> calls;
    for (int l = node.startLine + 1; l <= node.endLine && calls.size() < 3; ++l) {
        const std::string& s = buffer.line(l);
        for (size_t i = 0; i < s.size(); ++i) {
            if (s[i] != '(' || i == 0)
                continue;
            size_t b = i;
            while (b > 0 && (std::isalnum(static_cast<unsigned char>(s[b - 1])) || s[b - 1] == '_' || s[b - 1] == ':' || s[b - 1] == '.'))
                --b;
            const std::string name = s.substr(b, i - b);
            if (!name.empty() && !keywords.count(name) &&
                std::find(calls.begin(), calls.end(), name) == calls.end() && calls.size() < 3)
                calls.push_back(name);
        }
    }
    // The line count is shown next to every summary, so the text sticks to what the code does.
    if (calls.empty())
        return node.kind == model::OutlineKind::Type ? "Declarations and data members." : "Straight-line code, no calls.";
    std::string out = "Calls ";
    for (size_t i = 0; i < calls.size(); ++i)
        out += (i ? ", " : "") + calls[i];
    return out + ".";
}

std::string summarizeFile(const std::string& path, const std::vector<model::OutlineNode>& outline)
{
    static const std::pair<const char*, const char*> canned[] = {
        {"src/main.cpp", "Command-line word counter: parses the options, counts normalised words and prints the top N."},
        {"src/TextUtils.cpp", "Implements the string helpers: ASCII lower-casing and punctuation trimming."},
        {"src/TextUtils.h", "Declares toLower and stripPunctuation for the word counter."},
        {"scripts/build_index.py", "Python script that writes each corpus document's word count to index.json."},
        {"docs/README.md", "Explains what EasyDemo does, how to run it and how the code is laid out."},
        {"CMakeLists.txt", "Builds the easydemo executable and its unit-test target."},
        {"config.json", "Run settings (corpus folder, top N, case) and a telemetry endpoint with a committed API key."},
        {"Welcome.md", "Introduction to EasyNotepad: the two workspaces, reading tools and review tools."},
    };
    for (const auto& [file, text] : canned)
        if (path == file)
            return text;
    if (outline.empty())
        return "No nested structure.";
    std::vector<const model::OutlineNode*> top;
    for (const model::OutlineNode& n : outline)
        if (n.depth == 0)
            top.push_back(&n);
    std::string out = top.size() == 1 ? "One top-level part: " : std::to_string(top.size()) + " top-level parts: ";
    for (size_t i = 0; i < top.size() && i < 3; ++i)
        out += (i ? ", " : "") + top[i]->title;
    return out + (top.size() > 3 ? ", ..." : ".");
}

std::string fileHeaderSummary(const std::string& path, const model::TextBuffer& buffer)
{
    const std::string canned = summarizeFile(path, {});
    if (canned != "No nested structure.")
        return canned;
    const bool markdown = model::languageFromPath(path) == model::Language::Markdown;
    std::string text;
    for (int l = 0; l < buffer.lineCount() && l < 40; ++l) {
        std::string t = buffer.line(l);
        const size_t b = t.find_first_not_of(" \t");
        t = b == std::string::npos ? std::string() : t.substr(b);
        if (markdown) {
            if (t.empty() || t[0] == '#' || t.rfind("```", 0) == 0) {
                if (!text.empty())
                    break;
                continue;
            }
        } else if (t.rfind("//", 0) == 0) {
            t = t.substr(t.rfind("///", 0) == 0 ? 3 : 2);
        } else if (t.rfind("#!", 0) == 0 || t.rfind("#include", 0) == 0 || t.rfind("#pragma", 0) == 0 || t.empty()) {
            if (!text.empty())
                break;
            continue;
        } else if (t[0] == '#') {
            t = t.substr(1);
        } else {
            break; // code before any comment
        }
        const size_t c = t.find_first_not_of(" ");
        if (c != std::string::npos)
            text += (text.empty() ? "" : " ") + t.substr(c);
        if (text.size() > 200)
            break;
    }
    if (text.size() > 160)
        text = text.substr(0, 157) + "...";
    return text;
}

std::string summarizeFolder(const std::string& name, const std::vector<std::string>& children)
{
    if (name == kWorkspaceName)
        return "Sample word-frequency counter in C++, with a Python indexer, docs and build files.";
    if (name == "src")
        return "The C++ program: entry point and string helpers.";
    if (name == "scripts")
        return "Helper scripts for preparing the corpus.";
    if (name == "docs")
        return "Project documentation.";
    std::string out = "Contains ";
    for (size_t i = 0; i < children.size() && i < 4; ++i)
        out += (i ? ", " : "") + children[i];
    return out + (children.size() > 4 ? ", ..." : ".");
}

std::string agentReply(int n, const std::string& prompt)
{
    static const char* const replies[] = {
        "Understood. I'd start by reading the files involved and running the review tools, then propose "
        "the edits as a diff for you to accept.",
        "Here is my plan: 1) reproduce the issue, 2) make the smallest fix, 3) add a regression test, "
        "4) re-run Lint and Static Analysis.",
        "Done thinking. The change touches one function; I'd keep the public interface unchanged.",
    };
    std::string reply = replies[n % 3];
    if (prompt.find("explain") != std::string::npos || prompt.find("Explain") != std::string::npos)
        reply = "This code reads words from the input stream, normalises them, and counts them in a std::map. "
                "The map keeps words sorted, and printReport() re-sorts them by frequency.";
    return reply + "\n\n(Dummy reply: the agent service is not connected yet.)";
}

} // namespace enp::viewmodel::dummy
