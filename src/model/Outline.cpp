#include "enp/model/Outline.h"

#include <algorithm>
#include <cctype>

namespace enp::model {

namespace {

constexpr size_t kMaxTitle = 72;

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

std::string makeTitle(std::string header)
{
    header = trim(header);
    while (!header.empty() && (header.back() == '{' || header.back() == '[' || header.back() == ':'))
        header = trim(header.substr(0, header.size() - 1));
    if (header.size() > kMaxTitle)
        header = header.substr(0, kMaxTitle - 3) + "...";
    return header;
}

OutlineKind classify(const std::string& title)
{
    if (startsWith(title, "namespace"))
        return OutlineKind::Namespace;
    for (const char* t : {"struct ", "class ", "enum ", "union ", "interface ", "trait ", "impl ", "record ", "type "})
        if (startsWith(title, t) || title.find(std::string(" ") + t) != std::string::npos)
            return OutlineKind::Type;
    // "name(" opens a function header, also when the parameter list continues on the next lines.
    const size_t open = title.find('(');
    if (open == std::string::npos)
        return OutlineKind::Block;
    // "name(...)" is a function unless the word before the parenthesis is a control keyword, or
    // the header is an assignment / lambda.
    const std::string head = trim(title.substr(0, open));
    if (head.empty() || head[0] == ':' || head.find_first_of("=[") != std::string::npos)
        return OutlineKind::Block;
    const size_t space = head.find_last_of(" \t}");
    const std::string word = space == std::string::npos ? head : head.substr(space + 1);
    for (const char* keyword : {"if", "for", "while", "switch", "catch", "return"})
        if (word == keyword)
            return OutlineKind::Block;
    return OutlineKind::Function;
}

// "void NotepadViewModel::set(CommandId id)" -> "set(...)"; other kinds keep their title.
std::string shortName(const OutlineNode& n)
{
    if (n.kind != OutlineKind::Function)
        return n.title;
    const size_t open = n.title.find('(');
    std::string head = trim(n.title.substr(0, open));
    const size_t space = head.find_last_of(" \t*&");
    if (space != std::string::npos)
        head = head.substr(space + 1);
    const size_t scope = head.rfind("::");
    if (scope != std::string::npos && scope + 2 < head.size())
        head = head.substr(scope + 2);
    const size_t close = n.title.find(')', open);
    const bool noParams = close != std::string::npos && trim(n.title.substr(open + 1, close - open - 1)).empty();
    return head + (noParams ? "()" : "(\xE2\x80\xA6)");
}

void finalize(std::vector<OutlineNode>& nodes)
{
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(), [](const OutlineNode& n) { return n.endLine <= n.startLine; }),
                nodes.end());
    std::stable_sort(nodes.begin(), nodes.end(), [](const OutlineNode& a, const OutlineNode& b) {
        return a.startLine != b.startLine ? a.startLine < b.startLine : a.endLine > b.endLine;
    });
    // Depth and parent from containment.
    std::vector<int> stack;
    for (int i = 0; i < int(nodes.size()); ++i) {
        while (!stack.empty() && nodes[size_t(stack.back())].endLine < nodes[size_t(i)].startLine)
            stack.pop_back();
        nodes[size_t(i)].parent = stack.empty() ? -1 : stack.back();
        nodes[size_t(i)].depth = int(stack.size());
        nodes[size_t(i)].name = shortName(nodes[size_t(i)]);
        stack.push_back(i);
    }
}

// Regions between { } (and [ ] in JSON), skipping strings and comments as the language defines them.
std::vector<OutlineNode> braceOutline(const TextBuffer& buffer, const LanguageSpec& spec)
{
    const bool json = spec.id == Language::Json;
    struct Open {
        int line;
        int headerLine;
    };
    std::vector<OutlineNode> nodes;
    std::vector<Open> open;
    bool inBlockComment = false;
    for (int l = 0; l < buffer.lineCount(); ++l) {
        const std::string& s = buffer.line(l);
        char quote = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (inBlockComment) {
                if (s.compare(i, spec.blockCommentEnd.size(), spec.blockCommentEnd) == 0) {
                    inBlockComment = false;
                    i += spec.blockCommentEnd.size() - 1;
                }
                continue;
            }
            if (quote) {
                if (c == '\\')
                    ++i;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (!spec.blockCommentStart.empty() && s.compare(i, spec.blockCommentStart.size(), spec.blockCommentStart) == 0) {
                inBlockComment = true;
                i += spec.blockCommentStart.size() - 1;
            } else if (!spec.lineComment.empty() && s.compare(i, spec.lineComment.size(), spec.lineComment) == 0) {
                break;
            } else if (spec.quotes.find(c) != std::string::npos) {
                quote = c;
            } else if (c == '{' || (json && c == '[')) {
                // A brace alone on its line belongs to the previous non-blank line (Allman style).
                int header = l;
                if (trim(s.substr(0, i)).empty()) {
                    for (int h = l - 1; h >= 0; --h)
                        if (!trim(buffer.line(h)).empty()) {
                            header = h;
                            break;
                        }
                }
                // Constructor initializer lists: the signature is above the ": member(...)" lines.
                while (header > 0) {
                    const std::string t = trim(buffer.line(header));
                    if (t.empty() || (t[0] != ':' && t[0] != ','))
                        break;
                    --header;
                }
                open.push_back({l, header});
            } else if ((c == '}' || (json && c == ']')) && !open.empty()) {
                const Open o = open.back();
                open.pop_back();
                OutlineNode node;
                node.startLine = o.headerLine;
                node.endLine = l;
                node.title = makeTitle(buffer.line(o.headerLine));
                node.kind = json || spec.id == Language::Css ? OutlineKind::Block : classify(node.title);
                if (node.title.empty())
                    node.title = json ? "{...}" : "block";
                nodes.push_back(std::move(node));
            }
        }
    }
    return nodes;
}

std::string lowerCase(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Regions between opener and closer words (if ... endif, function ... end, if ... fi), plus
// multi-line ( ) lists and { } blocks where the language asks for them.
std::vector<OutlineNode> keywordOutline(const TextBuffer& buffer, const LanguageSpec& spec)
{
    struct Open {
        int line;
        bool function;
    };
    std::vector<OutlineNode> nodes;
    std::vector<Open> words, parens, braces;
    bool inBlockComment = false;
    auto add = [&](int a, int b, OutlineKind kind) {
        OutlineNode node;
        node.startLine = a;
        node.endLine = b;
        node.title = makeTitle(buffer.line(a));
        node.kind = kind;
        nodes.push_back(std::move(node));
    };
    auto isWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (int l = 0; l < buffer.lineCount(); ++l) {
        const std::string& s = buffer.line(l);
        char quote = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            const char c = s[i];
            if (inBlockComment) {
                if (s.compare(i, spec.blockCommentEnd.size(), spec.blockCommentEnd) == 0) {
                    inBlockComment = false;
                    i += spec.blockCommentEnd.size() - 1;
                }
                continue;
            }
            if (quote) {
                if (c == '\\')
                    ++i;
                else if (c == quote)
                    quote = 0;
                continue;
            }
            if (!spec.blockCommentStart.empty() && s.compare(i, spec.blockCommentStart.size(), spec.blockCommentStart) == 0) {
                inBlockComment = true;
                i += spec.blockCommentStart.size() - 1;
                continue;
            }
            if (!spec.lineComment.empty() && s.compare(i, spec.lineComment.size(), spec.lineComment) == 0 &&
                (spec.lineComment != "#" || i == 0 || !isWord(s[i - 1])) && !(i > 0 && s[i - 1] == '$'))
                break;
            if (spec.quotes.find(c) != std::string::npos) {
                quote = c;
                continue;
            }
            if (spec.foldParens && c == '(') {
                parens.push_back({l, false});
                continue;
            }
            if (spec.foldParens && c == ')' && !parens.empty()) {
                const Open o = parens.back();
                parens.pop_back();
                if (l > o.line)
                    add(o.line, l, OutlineKind::Block);
                continue;
            }
            if (c == '$' && i + 1 < s.size() && s[i + 1] == '{') { // ${variable}: not a block
                const size_t close = s.find('}', i);
                i = close == std::string::npos ? s.size() : close;
                continue;
            }
            if (spec.foldBraces && c == '{') {
                braces.push_back({l, false});
                continue;
            }
            if (spec.foldBraces && c == '}' && !braces.empty()) {
                const Open o = braces.back();
                braces.pop_back();
                const std::string title = makeTitle(buffer.line(o.line));
                add(o.line, l, classify(title));
                continue;
            }
            if (!(std::isalpha(static_cast<unsigned char>(c)) || c == '_') || (i > 0 && (isWord(s[i - 1]) || s[i - 1] == '$')))
                continue;
            size_t j = i;
            while (j < s.size() && isWord(s[j]))
                ++j;
            std::string word = s.substr(i, j - i);
            if (spec.caseInsensitive)
                word = lowerCase(word);
            size_t next = j;
            while (next < s.size() && (s[next] == ' ' || s[next] == '\t'))
                ++next;
            const bool paren = next < s.size() && s[next] == '(';
            if (!spec.foldOpenNeedsParen || paren) {
                if (std::find(spec.foldOpen.begin(), spec.foldOpen.end(), word) != spec.foldOpen.end()) {
                    words.push_back({l, word == "function" || word == "macro"});
                } else if (std::find(spec.foldClose.begin(), spec.foldClose.end(), word) != spec.foldClose.end() &&
                           !words.empty()) {
                    const Open o = words.back();
                    words.pop_back();
                    add(o.line, l, o.function ? OutlineKind::Function : OutlineKind::Block);
                }
            }
            i = j - 1;
        }
    }
    return nodes;
}

// YAML-style: a line followed by deeper-indented lines opens a region that ends before the next
// line at its own indentation or less.
std::vector<OutlineNode> indentOutline(const TextBuffer& buffer, const LanguageSpec& spec)
{
    std::vector<OutlineNode> nodes;
    auto indent = [](const std::string& s) {
        int k = 0;
        for (char c : s) {
            if (c == ' ')
                ++k;
            else if (c == '\t')
                k += 4;
            else
                break;
        }
        return k;
    };
    auto skip = [&](const std::string& s) {
        const std::string t = trim(s);
        return t.empty() || (!spec.lineComment.empty() && t.rfind(spec.lineComment, 0) == 0);
    };
    for (int l = 0; l < buffer.lineCount(); ++l) {
        if (skip(buffer.line(l)))
            continue;
        const int base = indent(buffer.line(l));
        int end = l;
        for (int k = l + 1; k < buffer.lineCount(); ++k) {
            if (skip(buffer.line(k)))
                continue;
            if (indent(buffer.line(k)) <= base)
                break;
            end = k;
        }
        if (end > l) {
            OutlineNode node;
            node.startLine = l;
            node.endLine = end;
            node.title = makeTitle(buffer.line(l));
            node.kind = base == 0 ? OutlineKind::Section : OutlineKind::Block;
            nodes.push_back(std::move(node));
        }
    }
    return nodes;
}

// TOML / INI: [section] ... up to the next section.
std::vector<OutlineNode> sectionOutline(const TextBuffer& buffer)
{
    std::vector<int> headers;
    for (int l = 0; l < buffer.lineCount(); ++l)
        if (startsWith(trim(buffer.line(l)), "["))
            headers.push_back(l);
    std::vector<OutlineNode> nodes;
    for (size_t i = 0; i < headers.size(); ++i) {
        int end = i + 1 < headers.size() ? headers[i + 1] - 1 : buffer.lineCount() - 1;
        while (end > headers[i] && trim(buffer.line(end)).empty())
            --end;
        OutlineNode node;
        node.startLine = headers[i];
        node.endLine = end;
        node.title = trim(buffer.line(headers[i]));
        node.kind = OutlineKind::Section;
        nodes.push_back(std::move(node));
    }
    return nodes;
}

// HTML / XML elements that span lines, and multi-line comments.
std::vector<OutlineNode> tagOutline(const TextBuffer& buffer, const LanguageSpec& spec)
{
    static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                        "param", "source", "track", "wbr"};
    const bool html = spec.id == Language::Html;
    struct Open {
        std::string name;
        int line;
    };
    std::vector<OutlineNode> nodes;
    std::vector<Open> open;
    int commentStart = -1;
    std::string rawUntil; // inside <script> / <style>: skip to the closing tag
    for (int l = 0; l < buffer.lineCount(); ++l) {
        const std::string& s = buffer.line(l);
        size_t i = 0;
        while (i < s.size()) {
            if (commentStart >= 0) {
                const size_t e = s.find("-->", i);
                if (e == std::string::npos)
                    break;
                if (l > commentStart) {
                    OutlineNode node;
                    node.startLine = commentStart;
                    node.endLine = l;
                    node.title = makeTitle(buffer.line(commentStart));
                    node.kind = OutlineKind::Block;
                    nodes.push_back(std::move(node));
                }
                commentStart = -1;
                i = e + 3;
                continue;
            }
            const size_t lt = s.find('<', i);
            if (lt == std::string::npos)
                break;
            if (!rawUntil.empty() && lowerCase(s.substr(lt, rawUntil.size())) != rawUntil) {
                i = lt + 1;
                continue;
            }
            if (s.compare(lt, 4, "<!--") == 0) {
                commentStart = l;
                i = lt + 4;
                continue;
            }
            const bool closing = lt + 1 < s.size() && s[lt + 1] == '/';
            size_t j = lt + (closing ? 2 : 1);
            const size_t nameStart = j;
            while (j < s.size() && (std::isalnum(static_cast<unsigned char>(s[j])) || s[j] == '-' || s[j] == ':' || s[j] == '_' || s[j] == '.'))
                ++j;
            if (j == nameStart) {
                i = lt + 1;
                continue;
            }
            std::string name = s.substr(nameStart, j - nameStart);
            if (html)
                name = lowerCase(name);
            const size_t gt = s.find('>', j);
            const bool selfClosing = gt != std::string::npos && gt > 0 && s[gt - 1] == '/';
            if (closing) {
                rawUntil.clear();
                for (int k = int(open.size()) - 1; k >= 0; --k)
                    if (open[size_t(k)].name == name) {
                        if (l > open[size_t(k)].line) {
                            OutlineNode node;
                            node.startLine = open[size_t(k)].line;
                            node.endLine = l;
                            node.title = makeTitle(buffer.line(open[size_t(k)].line));
                            node.kind = OutlineKind::Block;
                            nodes.push_back(std::move(node));
                        }
                        open.resize(size_t(k));
                        break;
                    }
            } else if (!selfClosing && !(html && std::find(std::begin(kVoid), std::end(kVoid), name) != std::end(kVoid))) {
                open.push_back({name, l});
                if (html && (name == "script" || name == "style"))
                    rawUntil = "</" + name;
            }
            i = gt == std::string::npos ? s.size() : gt + 1;
        }
    }
    return nodes;
}

// "#region Name" / "#pragma region" / "// region Name" ... "#endregion", in any language.
void addRegions(const TextBuffer& buffer, const LanguageSpec& spec, std::vector<OutlineNode>& nodes)
{
    std::vector<std::pair<int, std::string>> open;
    for (int l = 0; l < buffer.lineCount(); ++l) {
        std::string t = trim(buffer.line(l));
        const bool comment = !spec.lineComment.empty() && startsWith(t, spec.lineComment.c_str());
        if (comment)
            t = trim(t.substr(spec.lineComment.size()));
        else if (!startsWith(t, "#"))
            continue;
        if (startsWith(t, "#pragma "))
            t = trim(t.substr(8));
        if (startsWith(t, "#"))
            t = t.substr(1);
        if (startsWith(t, "region") && (t.size() == 6 || t[6] == ' ')) {
            open.emplace_back(l, trim(t.substr(6)));
        } else if (startsWith(t, "endregion") && !open.empty()) {
            OutlineNode node;
            node.startLine = open.back().first;
            node.endLine = l;
            node.title = open.back().second.empty() ? "region" : open.back().second;
            node.kind = OutlineKind::Section;
            nodes.push_back(std::move(node));
            open.pop_back();
        }
    }
}

int indentOf(const std::string& s)
{
    int n = 0;
    for (char c : s) {
        if (c == ' ')
            ++n;
        else if (c == '\t')
            n += 4;
        else
            break;
    }
    return n;
}

std::vector<OutlineNode> pythonOutline(const TextBuffer& buffer)
{
    std::vector<OutlineNode> nodes;
    for (int l = 0; l < buffer.lineCount(); ++l) {
        const std::string t = trim(buffer.line(l));
        const bool def = startsWith(t, "def ") || startsWith(t, "async def ");
        const bool cls = startsWith(t, "class ");
        const bool block = startsWith(t, "if __name__");
        if (!def && !cls && !block)
            continue;
        const int indent = indentOf(buffer.line(l));
        int end = l;
        for (int k = l + 1; k < buffer.lineCount(); ++k) {
            const std::string& s = buffer.line(k);
            if (trim(s).empty())
                continue;
            if (indentOf(s) <= indent)
                break;
            end = k;
        }
        OutlineNode node;
        node.startLine = l;
        node.endLine = end;
        node.title = makeTitle(t);
        node.kind = cls ? OutlineKind::Type : def ? OutlineKind::Function : OutlineKind::Block;
        nodes.push_back(std::move(node));
    }
    return nodes;
}

std::vector<OutlineNode> markdownOutline(const TextBuffer& buffer)
{
    struct Heading {
        int line;
        int level;
    };
    std::vector<Heading> headings;
    std::vector<OutlineNode> fences;
    int fenceStart = -1;
    for (int l = 0; l < buffer.lineCount(); ++l) {
        const std::string& s = buffer.line(l);
        const std::string t = trim(s);
        if (startsWith(t, "```") || startsWith(t, "~~~")) {
            if (fenceStart < 0) {
                fenceStart = l;
            } else {
                OutlineNode fence;
                fence.startLine = fenceStart;
                fence.endLine = l;
                const std::string info = trim(trim(buffer.line(fenceStart)).substr(3));
                fence.title = info.empty() ? "code" : info + " code";
                fence.kind = OutlineKind::Block;
                fences.push_back(std::move(fence));
                fenceStart = -1;
            }
            continue;
        }
        if (fenceStart >= 0)
            continue; // "# comment" inside a code block is not a heading
        int level = 0;
        while (level < int(s.size()) && s[size_t(level)] == '#')
            ++level;
        if (level > 0 && level < int(s.size()) && s[size_t(level)] == ' ')
            headings.push_back({l, level});
    }
    std::vector<OutlineNode> nodes;
    for (size_t i = 0; i < headings.size(); ++i) {
        int end = buffer.lineCount() - 1;
        for (size_t k = i + 1; k < headings.size(); ++k)
            if (headings[k].level <= headings[i].level) {
                end = headings[k].line - 1;
                break;
            }
        while (end > headings[i].line && trim(buffer.line(end)).empty())
            --end;
        OutlineNode node;
        node.startLine = headings[i].line;
        node.endLine = end;
        node.title = trim(buffer.line(headings[i].line).substr(size_t(headings[i].level)));
        node.kind = OutlineKind::Section;
        nodes.push_back(std::move(node));
    }
    for (OutlineNode& fence : fences)
        nodes.push_back(std::move(fence));
    return nodes;
}

bool isCommentLine(const std::string& t, const LanguageSpec& spec)
{
    if (!spec.lineComment.empty() && startsWith(t, spec.lineComment.c_str()))
        return true;
    if (!spec.blockCommentStart.empty() && startsWith(t, spec.blockCommentStart.c_str()))
        return true;
    return spec.blockCommentStart == "/*" && startsWith(t, "*");
}

std::string commentText(std::string t)
{
    for (const char* prefix : {"///", "//", "/*", "--", "#", "*"})
        if (startsWith(t, prefix)) {
            t = t.substr(std::string(prefix).size());
            break;
        }
    if (t.size() >= 2 && t.compare(t.size() - 2, 2, "*/") == 0)
        t.resize(t.size() - 2);
    return trim(t);
}

// Logical steps of one function body: paragraphs separated by blank lines at any depth. A
// paragraph led by a comment uses it as the step's title and summary; single-line paragraphs stay
// as they are (nothing to fold).
void addSteps(const TextBuffer& buffer, const LanguageSpec& spec, const OutlineNode& fn, std::vector<OutlineNode>& out)
{
    const bool braces = spec.fold != FoldStyle::Python;
    int first = fn.startLine + 1;
    if (braces) {
        // Skip to the line after the opening brace.
        while (first <= fn.endLine && buffer.line(first - 1).find('{') == std::string::npos)
            ++first;
    }
    const int last = braces ? fn.endLine - 1 : fn.endLine;
    std::vector<std::pair<int, int>> paragraphs;
    for (int l = first; l <= last; ++l) {
        if (trim(buffer.line(l)).empty())
            continue;
        int e = l;
        while (e + 1 <= last && !trim(buffer.line(e + 1)).empty())
            ++e;
        paragraphs.emplace_back(l, e);
        l = e;
    }
    if (paragraphs.size() < 2 && (paragraphs.empty() || paragraphs[0].second - paragraphs[0].first < 12))
        return; // one short paragraph: the function is already a single step
    for (const auto& [a, b] : paragraphs) {
        if (b == a)
            continue;
        OutlineNode step;
        step.kind = OutlineKind::Step;
        step.startLine = a;
        step.endLine = b;
        // Comment lines at the top of the paragraph become the title; the header line stays the
        // first comment line so the step reads as its explanation when folded.
        std::string comment;
        int c = a;
        while (c <= b && isCommentLine(trim(buffer.line(c)), spec)) {
            const std::string text = commentText(trim(buffer.line(c)));
            if (!text.empty())
                comment += (comment.empty() ? "" : " ") + text;
            ++c;
        }
        if (c > b)
            continue; // a comment block alone is not a step
        step.hint = comment;
        step.title = comment.empty() ? makeTitle(buffer.line(a)) : makeTitle(comment);
        out.push_back(std::move(step));
    }
}

} // namespace

const char* outlineKindName(OutlineKind kind)
{
    switch (kind) {
    case OutlineKind::Namespace: return "Namespace";
    case OutlineKind::Type: return "Type";
    case OutlineKind::Function: return "Function";
    case OutlineKind::Block: return "Block";
    case OutlineKind::Section: return "Section";
    case OutlineKind::Step: return "Step";
    }
    return "";
}

std::vector<OutlineNode> buildOutline(const TextBuffer& buffer, Language language, OutlineMode mode)
{
    const LanguageSpec& spec = languageSpec(language);
    std::vector<OutlineNode> nodes;
    switch (spec.fold) {
    case FoldStyle::Braces: nodes = braceOutline(buffer, spec); break;
    case FoldStyle::Python: nodes = pythonOutline(buffer); break;
    case FoldStyle::Indent: nodes = indentOutline(buffer, spec); break;
    case FoldStyle::Keywords: nodes = keywordOutline(buffer, spec); break;
    case FoldStyle::Headings: nodes = markdownOutline(buffer); break;
    case FoldStyle::Tags: nodes = tagOutline(buffer, spec); break;
    case FoldStyle::Sections: nodes = sectionOutline(buffer); break;
    case FoldStyle::None: break;
    }
    if (spec.lexer == Lexer::Code && spec.id != Language::PlainText)
        addRegions(buffer, spec, nodes);
    if (mode == OutlineMode::Logical && spec.program) {
        // Inside functions, logical steps replace the syntactic blocks.
        std::vector<std::pair<int, int>> functions;
        for (const OutlineNode& n : nodes)
            if (n.kind == OutlineKind::Function)
                functions.emplace_back(n.startLine, n.endLine);
        auto insideFunction = [&](const OutlineNode& n) {
            for (const auto& [a, b] : functions)
                if (n.startLine > a && n.endLine <= b)
                    return true;
            return false;
        };
        std::vector<OutlineNode> logical;
        for (const OutlineNode& n : nodes)
            if (!(n.kind == OutlineKind::Block && insideFunction(n)))
                logical.push_back(n);
        for (const OutlineNode& n : nodes)
            if (n.kind == OutlineKind::Function && !insideFunction(n))
                addSteps(buffer, spec, n, logical);
        nodes = std::move(logical);
    }
    finalize(nodes);
    return nodes;
}

} // namespace enp::model
