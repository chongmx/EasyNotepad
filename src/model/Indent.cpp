#include "enp/model/Indent.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <vector>

namespace enp::model {

namespace {

bool isBlank(char c) { return c == ' ' || c == '\t'; }
bool isWordChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; }

std::string trimRight(std::string s)
{
    while (!s.empty() && (isBlank(s.back()) || s.back() == '\r'))
        s.pop_back();
    return s;
}

std::string trimLeft(const std::string& s)
{
    size_t i = 0;
    while (i < s.size() && isBlank(s[i]))
        ++i;
    return s.substr(i);
}

bool startsWith(const std::string& s, std::string_view prefix)
{
    return s.compare(0, prefix.size(), prefix) == 0;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The code of a line: strings blanked out and the trailing line comment removed, right-trimmed.
std::string codeOf(const std::string& line, const LanguageSpec& spec)
{
    std::string out = line;
    char quote = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const char c = out[i];
        if (quote) {
            if (c == '\\' && i + 1 < out.size()) {
                out[i] = out[i + 1] = ' ';
                ++i;
            } else if (c == quote) {
                quote = 0;
            } else {
                out[i] = ' ';
            }
            continue;
        }
        if (!spec.lineComment.empty() && out.compare(i, spec.lineComment.size(), spec.lineComment) == 0 &&
            (spec.lineComment != "#" || i == 0 || isBlank(out[i - 1]))) {
            out.resize(i);
            break;
        }
        if (spec.quotes.find(c) != std::string::npos)
            quote = c;
    }
    return trimRight(out);
}

// Opening minus closing fold words on a line (CMake if( / endif(, Lua function / end, if / fi).
int wordDelta(const std::string& code, const LanguageSpec& spec)
{
    int delta = 0;
    for (size_t i = 0; i < code.size();) {
        if (!(std::isalpha(static_cast<unsigned char>(code[i])) || code[i] == '_') || (i > 0 && (isWordChar(code[i - 1]) || code[i - 1] == '$'))) {
            ++i;
            continue;
        }
        size_t j = i;
        while (j < code.size() && isWordChar(code[j]))
            ++j;
        std::string word = code.substr(i, j - i);
        if (spec.caseInsensitive)
            word = lower(word);
        size_t next = j;
        while (next < code.size() && isBlank(code[next]))
            ++next;
        const bool paren = next < code.size() && code[next] == '(';
        if (!spec.foldOpenNeedsParen || paren) {
            if (std::find(spec.foldOpen.begin(), spec.foldOpen.end(), word) != spec.foldOpen.end())
                ++delta;
            else if (std::find(spec.foldClose.begin(), spec.foldClose.end(), word) != spec.foldClose.end())
                --delta;
        }
        i = j;
    }
    return delta;
}

// Opening minus closing tags on a line (void and self-closing elements do not count).
int tagDelta(const std::string& line, const LanguageSpec& spec)
{
    static const char* const kVoid[] = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                        "param", "source", "track", "wbr"};
    int delta = 0;
    for (size_t i = line.find('<'); i != std::string::npos; i = line.find('<', i + 1)) {
        if (i + 1 >= line.size() || line[i + 1] == '!' || line[i + 1] == '?')
            continue;
        const bool closing = line[i + 1] == '/';
        size_t j = i + (closing ? 2 : 1);
        const size_t start = j;
        while (j < line.size() && (std::isalnum(static_cast<unsigned char>(line[j])) || line[j] == '-' || line[j] == ':' || line[j] == '.'))
            ++j;
        if (j == start)
            continue;
        const std::string name = lower(line.substr(start, j - start));
        if (closing) {
            --delta;
            continue;
        }
        const size_t gt = line.find('>', j);
        const bool selfClosing = gt != std::string::npos && line[gt - 1] == '/';
        const bool isVoid = spec.id == Language::Html && std::find(std::begin(kVoid), std::end(kVoid), name) != std::end(kVoid);
        if (!selfClosing && !isVoid)
            ++delta;
    }
    return delta;
}

// Lines "else", "elif ..." and the like continue the block above at its opener's indent.
std::vector<std::string> middleWords(const LanguageSpec& spec)
{
    switch (spec.id) {
    case Language::CMake: return {"else", "elseif"};
    case Language::Lua: return {"else", "elseif"};
    case Language::Shell: return {"else", "elif", "then", "do"};
    case Language::Sql: return {"else", "when"};
    default: return {};
    }
}

std::string firstWord(const std::string& trimmed, const LanguageSpec& spec)
{
    size_t j = 0;
    while (j < trimmed.size() && isWordChar(trimmed[j]))
        ++j;
    std::string word = trimmed.substr(0, j);
    return spec.caseInsensitive ? lower(word) : word;
}

bool contains(const std::vector<std::string>& words, const std::string& w)
{
    return std::find(words.begin(), words.end(), w) != words.end();
}

// Line of the bracket that `closer` (at line, column) closes, scanning backwards; -1 if none.
int matchingBracketLine(const TextBuffer& buffer, int line, size_t column, char closer, const LanguageSpec& spec)
{
    const char opener = closer == '}' ? '{' : closer == ']' ? '[' : '(';
    int depth = 1;
    for (int l = line; l >= 0 && line - l < 5000; --l) {
        const std::string code = codeOf(buffer.line(l), spec);
        size_t end = l == line ? std::min(column, code.size()) : code.size();
        for (size_t i = end; i-- > 0;) {
            if (code[i] == closer)
                ++depth;
            else if (code[i] == opener && --depth == 0)
                return l;
        }
    }
    return -1;
}

} // namespace

std::string indentUnit(IndentStyle style)
{
    return style.tabs ? std::string("\t") : std::string(size_t(std::max(1, style.width)), ' ');
}

std::string leadingWhitespace(std::string_view line)
{
    size_t n = 0;
    while (n < line.size() && isBlank(line[n]))
        ++n;
    return std::string(line.substr(0, n));
}

int indentColumns(std::string_view whitespace, int tabWidth)
{
    int columns = 0;
    for (char c : whitespace) {
        if (c == '\t')
            columns = (columns / tabWidth + 1) * tabWidth;
        else if (c == ' ')
            ++columns;
        else
            break;
    }
    return columns;
}

std::string makeIndent(int columns, IndentStyle style)
{
    columns = std::max(0, columns);
    if (!style.tabs)
        return std::string(size_t(columns), ' ');
    return std::string(size_t(columns / style.width), '\t') + std::string(size_t(columns % style.width), ' ');
}

std::string outdented(const std::string& indent, IndentStyle style)
{
    const int columns = indentColumns(indent, style.width);
    return makeIndent(std::max(0, ((columns - 1) / style.width) * style.width), style);
}

std::optional<IndentStyle> detectIndent(const TextBuffer& buffer)
{
    int tabLines = 0, spaceLines = 0;
    std::map<int, int> steps; // indent increase between consecutive lines -> count
    int previous = 0;
    bool inBlockComment = false;
    for (int l = 0; l < buffer.lineCount() && l < 4000; ++l) {
        const std::string& s = buffer.line(l);
        const std::string t = trimLeft(s);
        if (t.empty())
            continue;
        // " * text" lines of block comments are aligned, not indented.
        if (inBlockComment || startsWith(t, "* ") || t == "*" || startsWith(t, "*/")) {
            inBlockComment = s.find("*/") == std::string::npos && inBlockComment;
            continue;
        }
        if (startsWith(t, "/*") && t.find("*/") == std::string::npos)
            inBlockComment = true;
        if (!s.empty() && s[0] == '\t')
            ++tabLines;
        else if (!s.empty() && s[0] == ' ')
            ++spaceLines;
        if (s[0] == ' ') {
            const int columns = int(leadingWhitespace(s).size());
            if (columns > previous && columns - previous <= 8)
                ++steps[columns - previous];
            previous = columns;
        } else if (s[0] != '\t') {
            previous = 0;
        }
    }
    if (tabLines == 0 && spaceLines == 0)
        return std::nullopt;
    if (tabLines > spaceLines)
        return IndentStyle{true, 4};
    int best = 4, bestCount = 0;
    for (int width : {2, 4, 3, 8}) {
        const auto it = steps.find(width);
        if (it != steps.end() && it->second > bestCount) {
            best = width;
            bestCount = it->second;
        }
    }
    return IndentStyle{false, best};
}

IndentStyle defaultIndent(const LanguageSpec& spec)
{
    switch (spec.id) {
    case Language::Go: return {true, 4};
    case Language::Yaml:
    case Language::Json:
    case Language::Html:
    case Language::Xml:
    case Language::Markdown: return {false, 2};
    default: return {false, 4};
    }
}

NewlineEdit smartNewline(const TextBuffer& buffer, TextPos caret, const LanguageSpec& spec, IndentStyle style)
{
    NewlineEdit edit;
    const std::string& line = buffer.line(caret.line);
    const size_t column = std::min(size_t(caret.column), line.size());
    const std::string before = line.substr(0, column);
    const std::string after = line.substr(column);
    std::string base = leadingWhitespace(line);
    if (base.size() > column)
        base = before; // Enter inside the indentation keeps what is left of the caret
    const std::string beforeTrimmed = trimRight(before);
    if (beforeTrimmed.size() > base.size())
        edit.eraseBefore = int(before.size() - beforeTrimmed.size()); // no trailing blanks left behind
    edit.eraseAfter = int(after.size() - trimLeft(after).size());
    const std::string rest = trimLeft(after);
    const std::string content = trimLeft(beforeTrimmed);

    std::string indent = base;
    std::string extra;  // text after the indent: list bullet, " * "
    bool increase = false;
    bool split = false; // the closer right of the caret moves to its own line below

    if (spec.lexer == Lexer::Markdown) {
        // Lists and quotes continue; Enter on an empty item ends the list.
        const std::string raw = before.substr(std::min(base.size(), before.size()));
        std::string bullet;
        size_t marker = 0;
        if (raw.size() >= 2 && (raw[0] == '-' || raw[0] == '*' || raw[0] == '+') && raw[1] == ' ') {
            bullet = raw.substr(0, 2);
            marker = 2;
            if (raw.compare(2, 4, "[ ] ") == 0 || raw.compare(2, 4, "[x] ") == 0) {
                bullet += "[ ] ";
                marker = 6;
            }
        } else if (startsWith(raw, "> ")) {
            bullet = "> ";
            marker = 2;
        } else {
            size_t d = 0;
            while (d < raw.size() && d < 9 && std::isdigit(static_cast<unsigned char>(raw[d])))
                ++d;
            if (d > 0 && d + 1 < raw.size() && (raw[d] == '.' || raw[d] == ')') && raw[d + 1] == ' ') {
                bullet = std::to_string(std::stoi(raw.substr(0, d)) + 1) + raw[d] + " ";
                marker = d + 2;
            }
        }
        if (!bullet.empty() && trimLeft(raw.substr(marker)).empty() && rest.empty()) {
            edit.eraseBefore = int(raw.size()); // remove the empty item's marker, stay on the line
            edit.eraseAfter = 0;
            return edit;
        }
        extra = bullet;
    } else if (spec.lexer == Lexer::Markup) {
        increase = tagDelta(beforeTrimmed, spec) > 0 && !startsWith(content, "</");
        split = increase && startsWith(rest, "</");
    } else if (spec.lexer == Lexer::Code && spec.id != Language::PlainText) {
        const std::string code = codeOf(beforeTrimmed, spec);
        const char last = code.empty() ? 0 : code.back();
        if (spec.blockCommentStart == "/*" && (startsWith(content, "/*") || startsWith(content, "* ") || content == "*") &&
            content.find("*/") == std::string::npos) {
            extra = startsWith(content, "/*") ? " * " : "* "; // block comments continue
        } else if (spec.blockCommentStart == "/*" && startsWith(content, "*") && content.find("*/") != std::string::npos) {
            // After " */" (aligned one space in), code continues on the indent grid.
            indent = makeIndent((indentColumns(base, style.width) / style.width) * style.width, style);
        } else if (last == '{' || last == '[' || last == '(') {
            increase = true;
            const char closer = last == '{' ? '}' : last == '[' ? ']' : ')';
            split = !rest.empty() && rest[0] == closer;
        } else if ((spec.fold == FoldStyle::Python || spec.id == Language::Yaml) && last == ':') {
            increase = true;
        } else if (spec.id == Language::Yaml && (last == '|' || last == '>') && code.find(':') != std::string::npos) {
            increase = true; // block scalar
        } else if (spec.foldParens && std::count(code.begin(), code.end(), '(') > std::count(code.begin(), code.end(), ')')) {
            increase = true; // CMake / SQL argument lists continue indented: "set(SOURCES"
        } else if (spec.fold == FoldStyle::Keywords) {
            const int delta = wordDelta(code, spec);
            increase = delta > 0 || (delta == 0 && contains(middleWords(spec), firstWord(content, spec)));
        }
        if (spec.fold == FoldStyle::Python && !increase) {
            // The block ends after these.
            const std::string word = firstWord(content, spec);
            if (word == "return" || word == "pass" || word == "break" || word == "continue" || word == "raise")
                indent = outdented(base, style);
        }
    }
    if (increase)
        indent = base + indentUnit(style);
    edit.text = "\n" + indent + extra;
    edit.caretOffset = int(edit.text.size());
    if (split)
        edit.text += "\n" + base;
    return edit;
}

std::optional<std::string> closingIndent(const TextBuffer& buffer, int line, const LanguageSpec& spec, IndentStyle style)
{
    if (line <= 0 || line >= buffer.lineCount())
        return std::nullopt;
    const std::string& text = buffer.line(line);
    const std::string ws = leadingWhitespace(text);
    const std::string t = trimRight(text.substr(ws.size()));
    if (t.empty())
        return std::nullopt;

    if (spec.lexer == Lexer::Code && (t[0] == '}' || t[0] == ']' || t[0] == ')')) {
        const int opener = matchingBracketLine(buffer, line, ws.size(), t[0], spec);
        if (opener < 0 || opener == line)
            return std::nullopt;
        return leadingWhitespace(buffer.line(opener));
    }
    if (spec.lexer == Lexer::Markup && startsWith(t, "</")) {
        int depth = 1;
        for (int l = line - 1; l >= 0 && line - l < 5000; --l) {
            depth -= tagDelta(buffer.line(l), spec);
            if (depth <= 0)
                return leadingWhitespace(buffer.line(l));
        }
        return std::nullopt;
    }
    if (spec.fold == FoldStyle::Keywords) {
        const std::string word = firstWord(t, spec);
        const bool closer = contains(spec.foldClose, word) && (!spec.foldOpenNeedsParen || t.find('(') != std::string::npos);
        const bool middle = contains(middleWords(spec), word);
        if (!closer && !middle)
            return std::nullopt;
        if (middle && (word == "then" || word == "do")) {
            // "then" / "do" on their own line sit with their "if" / "for", which is the line above.
            for (int l = line - 1; l >= 0; --l)
                if (!trimLeft(buffer.line(l)).empty())
                    return leadingWhitespace(buffer.line(l));
            return std::nullopt;
        }
        int depth = 1;
        for (int l = line - 1; l >= 0 && line - l < 5000; --l) {
            depth -= wordDelta(codeOf(buffer.line(l), spec), spec);
            if (depth <= 0)
                return leadingWhitespace(buffer.line(l));
        }
        return std::nullopt;
    }
    if (spec.fold == FoldStyle::Python) {
        const std::string word = firstWord(t, spec);
        if ((word != "else" && word != "elif" && word != "except" && word != "finally") || t.back() != ':')
            return std::nullopt;
        // The nearest statement above, at this indent or less, that this one continues.
        static const std::map<std::string, std::vector<std::string>> kOpeners = {
            {"else", {"if", "elif", "for", "while", "try", "except"}},
            {"elif", {"if", "elif"}},
            {"except", {"try", "except"}},
            {"finally", {"try", "except", "else"}}};
        const std::vector<std::string>& openers = kOpeners.at(word);
        const int columns = indentColumns(ws, style.width);
        for (int l = line - 1; l >= 0; --l) {
            const std::string& above = buffer.line(l);
            const std::string aws = leadingWhitespace(above);
            const std::string at = trimLeft(above);
            if (at.empty() || indentColumns(aws, style.width) > columns)
                continue;
            if (contains(openers, firstWord(at, spec)))
                return aws;
            if (indentColumns(aws, style.width) < columns && firstWord(at, spec) != "else")
                break; // left the block without finding its start
        }
        return std::nullopt;
    }
    return std::nullopt;
}

bool closesIndent(char c, const LanguageSpec& spec)
{
    switch (spec.lexer) {
    case Lexer::Code:
        if (c == '}' || c == ']' || c == ')')
            return true;
        if (spec.fold == FoldStyle::Python)
            return c == ':';
        if (spec.fold == FoldStyle::Keywords)
            return c == '(' || c == ' ' || c == ';';
        return false;
    case Lexer::Markup: return c == '/' || c == '>';
    case Lexer::Markdown:
    case Lexer::Delimited: return false;
    }
    return false;
}

} // namespace enp::model
