#include "enp/model/Highlighter.h"

#include <algorithm>
#include <cctype>

namespace enp::model {

namespace {

// Line-spanning states.
enum State : int {
    Normal = 0,
    InBlockComment,
    InTripleDouble, // """
    InTripleSingle, // '''
    InBacktick,     // `template` / Go raw string
    InFence,        // Markdown ``` block
    InTag,          // markup tag whose attributes continue on the next line
};

bool isBlank(char c) { return c == ' ' || c == '\t'; }
bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80); }
bool isIdent(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || (static_cast<unsigned char>(c) & 0x80); }

void fill(std::vector<Token>& out, size_t begin, size_t end, Token token)
{
    std::fill(out.begin() + std::ptrdiff_t(std::min(begin, out.size())), out.begin() + std::ptrdiff_t(std::min(end, out.size())),
              token);
}

bool at(const std::string& s, size_t i, const std::string& what)
{
    return !what.empty() && s.compare(i, what.size(), what) == 0;
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// End (exclusive) of a quoted string starting at `i`; npos when it does not close on this line.
size_t stringEnd(const std::string& s, size_t i, char quote, bool escapes = true)
{
    for (size_t j = i + 1; j < s.size(); ++j) {
        if (escapes && s[j] == '\\') {
            ++j;
            continue;
        }
        if (s[j] == quote)
            return j + 1;
    }
    return std::string::npos;
}

// ---- Code ---------------------------------------------------------------------------------------

void lexCode(const std::string& line, const LanguageSpec& spec, int& state, std::vector<Token>& out)
{
    const size_t n = line.size();
    size_t i = 0;

    // Constructs continued from the previous line.
    auto closeSpan = [&](const std::string& end, Token token) -> bool {
        const size_t e = line.find(end);
        if (e == std::string::npos) {
            fill(out, 0, n, token);
            return false;
        }
        fill(out, 0, e + end.size(), token);
        i = e + end.size();
        state = Normal;
        return true;
    };
    if (state == InBlockComment && !closeSpan(spec.blockCommentEnd, Token::Comment))
        return;
    if (state == InTripleDouble && !closeSpan("\"\"\"", Token::String))
        return;
    if (state == InTripleSingle && !closeSpan("'''", Token::String))
        return;
    if (state == InBacktick) {
        const size_t e = stringEnd(line, size_t(-1), '`'); // from the first character
        if (e == std::string::npos) {
            fill(out, 0, n, Token::String);
            return;
        }
        fill(out, 0, e, Token::String);
        i = e;
        state = Normal;
    }

    size_t ws = i;
    while (ws < n && isBlank(line[ws]))
        ++ws;

    // Line-level constructs.
    if (spec.preprocessor && i == 0 && ws < n && line[ws] == '#') {
        if (ws + 1 < n && (line[ws + 1] == '[' || line[ws + 1] == '!')) { // Rust #[attr]
            fill(out, ws, n, Token::Preprocessor);
            return;
        }
        size_t j = ws + 1;
        while (j < n && isBlank(line[j]))
            ++j;
        while (j < n && isIdent(line[j]))
            ++j;
        fill(out, ws, j, Token::Preprocessor);
        const size_t lt = line.find('<', j);
        const size_t gt = lt == std::string::npos ? lt : line.find('>', lt);
        if (gt != std::string::npos && line.compare(ws, 8, "#include") == 0)
            fill(out, lt, gt + 1, Token::String);
        i = j;
    }
    if (spec.fold == FoldStyle::Sections && ws < n && line[ws] == '[') { // [section]
        const size_t e = line.find(']', ws);
        fill(out, ws, e == std::string::npos ? n : e + 1, Token::Heading);
        i = e == std::string::npos ? n : e + 1;
    }
    if (spec.id == Language::Yaml) {
        if (line.compare(0, 3, "---") == 0 || line.compare(0, 3, "...") == 0) {
            fill(out, 0, 3, Token::Preprocessor);
            i = 3;
        } else if (ws + 1 < n && line[ws] == '-' && isBlank(line[ws + 1])) {
            out[ws] = Token::Keyword; // list item
            ws += 2;
            while (ws < n && isBlank(line[ws]))
                ++ws;
            i = std::max(i, ws);
        }
    }
    // A key at the start of the line: "key: value" (YAML), "key = value" (TOML / INI).
    if ((spec.id == Language::Yaml || spec.id == Language::Toml) && i <= ws && ws < n && line[ws] != '#' &&
        line[ws] != '[' && line[ws] != '"' && line[ws] != '\'') {
        size_t k = ws;
        while (k < n && line[k] != spec.keyValue && line[k] != '#' && line[k] != '"')
            ++k;
        const bool yamlKey = spec.keyValue == ':' && k < n && line[k] == ':' && (k + 1 == n || isBlank(line[k + 1]));
        const bool tomlKey = spec.keyValue == '=' && k < n && line[k] == '=';
        if (yamlKey || tomlKey) {
            size_t e = k;
            while (e > ws && isBlank(line[e - 1]))
                --e;
            fill(out, ws, e, Token::Key);
            i = k + 1;
        }
    }
    const bool cssSelectorLine = spec.id == Language::Css && line.find('{') != std::string::npos;

    while (i < n) {
        const char c = line[i];
        // Comments (block first: "--[[" starts with "--").
        if (at(line, i, spec.blockCommentStart)) {
            const size_t e = line.find(spec.blockCommentEnd, i + spec.blockCommentStart.size());
            if (e == std::string::npos) {
                fill(out, i, n, Token::Comment);
                state = InBlockComment;
                return;
            }
            fill(out, i, e + spec.blockCommentEnd.size(), Token::Comment);
            i = e + spec.blockCommentEnd.size();
            continue;
        }
        if (at(line, i, spec.lineComment) && (spec.lineComment != "#" || i == 0 || isBlank(line[i - 1]) || line[i - 1] == ';')) {
            fill(out, i, n, Token::Comment);
            return;
        }
        // Strings.
        if (spec.tripleQuotes && (at(line, i, "\"\"\"") || at(line, i, "'''"))) {
            const std::string q = line.substr(i, 3);
            const size_t e = line.find(q, i + 3);
            if (e == std::string::npos) {
                fill(out, i, n, Token::String);
                state = q[0] == '"' ? InTripleDouble : InTripleSingle;
                return;
            }
            fill(out, i, e + 3, Token::String);
            i = e + 3;
            continue;
        }
        if (spec.quotes.find(c) != std::string::npos) {
            size_t e = stringEnd(line, i, c, !(c == '`' && spec.id == Language::Go));
            if (e == std::string::npos) {
                fill(out, i, n, Token::String);
                if (c == '`' && spec.multilineBacktick)
                    state = InBacktick;
                return;
            }
            Token token = Token::String;
            if (spec.keyValue == ':' && spec.id != Language::Css) { // JSON keys: "key":
                size_t after = e;
                while (after < n && isBlank(line[after]))
                    ++after;
                if (after < n && line[after] == ':')
                    token = Token::Key;
            }
            fill(out, i, e, token);
            i = e;
            continue;
        }
        // Variables: $name, ${name}, $ENV{name}, $(...)
        if (spec.dollarVariables && c == '$' && i + 1 < n) {
            size_t j = i + 1;
            if (line[j] == '<') { // CMake generator expression head
                const size_t e = line.find_first_of(":>", j);
                fill(out, i, e == std::string::npos ? n : e + 1, Token::Preprocessor);
                i = e == std::string::npos ? n : e + 1;
                continue;
            }
            while (j < n && isIdent(line[j]))
                ++j;
            if (j < n && line[j] == '{') {
                const size_t e = line.find('}', j);
                j = e == std::string::npos ? n : e + 1;
            } else if (j == i + 1 && j < n && std::string("?!#@*_-0123456789").find(line[j]) != std::string::npos) {
                ++j; // $? $1 $@
            }
            if (j > i + 1) {
                fill(out, i, j, Token::Variable);
                i = j;
                continue;
            }
        }
        // Decorators / annotations / CSS at-rules.
        if (spec.atKeywords && c == '@' && i + 1 < n && isIdentStart(line[i + 1])) {
            size_t j = i + 1;
            while (j < n && (isIdent(line[j]) || line[j] == '.' || line[j] == '-'))
                ++j;
            fill(out, i, j, Token::Preprocessor);
            i = j;
            continue;
        }
        if (spec.id == Language::Css && c == '#' && i + 1 < n && std::isxdigit(static_cast<unsigned char>(line[i + 1])) &&
            !cssSelectorLine) {
            size_t j = i + 1;
            while (j < n && std::isxdigit(static_cast<unsigned char>(line[j])))
                ++j;
            fill(out, i, j, Token::Number);
            i = j;
            continue;
        }
        // Numbers (not the digits inside words: ubuntu-24.04, v1.2).
        const bool inWord = i > 0 && (isIdent(line[i - 1]) || ((line[i - 1] == '-' || line[i - 1] == '.') && i > 1 && isIdent(line[i - 2])));
        if (!inWord && (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < n && std::isdigit(static_cast<unsigned char>(line[i + 1]))))) {
            size_t j = i + 1;
            while (j < n && (isIdent(line[j]) || line[j] == '.' || line[j] == '%' ||
                             ((line[j] == '-' || line[j] == '+') && (line[j - 1] == 'e' || line[j - 1] == 'E'))))
                ++j;
            fill(out, i, j, Token::Number);
            i = j;
            continue;
        }
        // Words.
        const bool dashWord = spec.dashInIdentifiers && c == '-' && i + 1 < n && std::isalpha(static_cast<unsigned char>(line[i + 1])) &&
                              (i == 0 || isBlank(line[i - 1]));
        if (isIdentStart(c) || dashWord) {
            size_t j = i + 1;
            while (j < n && (isIdent(line[j]) || (spec.dashInIdentifiers && line[j] == '-' && j + 1 < n && isIdent(line[j + 1]))))
                ++j;
            const std::string word = line.substr(i, j - i);
            const std::string lookup = spec.caseInsensitive ? lower(word) : word;
            size_t next = j;
            while (spec.callsAreKeywords && next < n && isBlank(line[next]))
                ++next;
            const bool call = next < n && line[next] == '(';
            const bool scope = j + 1 < n && line[j] == ':' && line[j + 1] == ':';
            Token token = Token::Text;
            if (spec.keywords.count(lookup))
                token = Token::Keyword;
            else if (spec.types.count(lookup))
                token = Token::Type;
            else if (spec.id == Language::Css && !cssSelectorLine && next < n && line[next] == ':')
                token = Token::Key; // property
            else if (spec.callsAreKeywords && call)
                token = Token::Function;
            else if (spec.upperCaseConstants && word.size() > 1 &&
                     std::all_of(word.begin(), word.end(), [](char ch) { return !std::islower(static_cast<unsigned char>(ch)); }))
                token = Token::Type;
            else if (spec.capitalizedTypes && (std::isupper(static_cast<unsigned char>(word[0])) || scope))
                token = Token::Type;
            else if (call)
                token = Token::Function;
            if (token != Token::Text)
                fill(out, i, j, token);
            i = j;
            continue;
        }
        ++i;
    }
}

// ---- Markdown -----------------------------------------------------------------------------------

void lexMarkdown(const std::string& line, int& state, std::vector<Token>& out)
{
    const size_t n = line.size();
    size_t ws = 0;
    while (ws < n && isBlank(line[ws]))
        ++ws;
    if (line.compare(ws, 3, "```") == 0 || line.compare(ws, 3, "~~~") == 0) {
        fill(out, 0, n, Token::Code);
        state = state == InFence ? Normal : InFence;
        return;
    }
    if (state == InFence) {
        fill(out, 0, n, Token::Code);
        return;
    }
    if (line.rfind("    ", 0) == 0 || line.rfind("\t", 0) == 0) {
        fill(out, 0, n, Token::Code);
        return;
    }
    if (ws < n && line[ws] == '#') {
        fill(out, 0, n, Token::Heading);
        return;
    }
    if (ws < n && line[ws] == '>') {
        fill(out, 0, n, Token::Comment);
        return;
    }
    if (n >= 3 && line.find_first_not_of("-=_* ") == std::string::npos) { // rule, or setext underline
        fill(out, 0, n, Token::Comment);
        return;
    }
    // List bullets and numbers.
    if (ws + 1 < n && (line[ws] == '-' || line[ws] == '*' || line[ws] == '+') && line[ws + 1] == ' ') {
        out[ws] = Token::Keyword;
    } else {
        size_t d = ws;
        while (d < n && std::isdigit(static_cast<unsigned char>(line[d])))
            ++d;
        if (d > ws && d + 1 < n && (line[d] == '.' || line[d] == ')') && line[d + 1] == ' ')
            fill(out, ws, d + 1, Token::Keyword);
    }
    for (size_t i = ws; i < n;) {
        const char c = line[i];
        if (c == '`') {
            size_t e = line.find('`', i + 1);
            e = e == std::string::npos ? n : e + 1;
            fill(out, i, e, Token::Code);
            i = e;
        } else if ((c == '*' || c == '_') && i + 1 < n && !isBlank(line[i + 1])) {
            // **strong**, __strong__, *em*, _em_ (an underscore inside a word is not emphasis).
            if (c == '_' && i > 0 && isIdent(line[i - 1])) {
                ++i;
                continue;
            }
            const std::string mark = (i + 1 < n && line[i + 1] == c) ? std::string(2, c) : std::string(1, c);
            const size_t e = line.find(mark, i + mark.size());
            if (e == std::string::npos || e == i + mark.size()) {
                ++i;
                continue;
            }
            fill(out, i, e + mark.size(), Token::Emphasis);
            i = e + mark.size();
        } else if (c == '[' || (c == '!' && i + 1 < n && line[i + 1] == '[')) {
            // [text](url), ![alt](src), [text][ref]
            const size_t close = line.find(']', i);
            if (close != std::string::npos && close + 1 < n && (line[close + 1] == '(' || line[close + 1] == '[')) {
                const size_t e = line.find(line[close + 1] == '(' ? ')' : ']', close + 2);
                const size_t end = e == std::string::npos ? n : e + 1;
                fill(out, i, end, Token::Link);
                i = end;
            } else {
                ++i;
            }
        } else if (c == '<' && (line.compare(i + 1, 4, "http") == 0 || line.compare(i + 1, 7, "mailto:") == 0)) {
            const size_t e = line.find('>', i);
            const size_t end = e == std::string::npos ? n : e + 1;
            fill(out, i, end, Token::Link);
            i = end;
        } else {
            ++i;
        }
    }
}

// ---- Markup (HTML / XML) ------------------------------------------------------------------------

// Attributes and the end of a tag, from `i`; returns the position after the tag or n.
size_t lexTagBody(const std::string& line, size_t i, int& state, std::vector<Token>& out)
{
    const size_t n = line.size();
    while (i < n) {
        const char c = line[i];
        if (c == '>' || (c == '/' && i + 1 < n && line[i + 1] == '>') || (c == '?' && i + 1 < n && line[i + 1] == '>')) {
            const size_t e = c == '>' ? i + 1 : i + 2;
            fill(out, i, e, Token::Tag);
            state = Normal;
            return e;
        }
        if (c == '"' || c == '\'') {
            const size_t e = stringEnd(line, i, c, false);
            fill(out, i, e == std::string::npos ? n : e, Token::String);
            i = e == std::string::npos ? n : e;
            continue;
        }
        if (isIdentStart(c)) {
            size_t j = i;
            while (j < n && (isIdent(line[j]) || line[j] == '-' || line[j] == ':' || line[j] == '.'))
                ++j;
            fill(out, i, j, Token::Attribute);
            i = j;
            continue;
        }
        ++i;
    }
    state = InTag;
    return n;
}

void lexMarkup(const std::string& line, const LanguageSpec& spec, int& state, std::vector<Token>& out)
{
    const size_t n = line.size();
    size_t i = 0;
    if (state == InBlockComment) {
        const size_t e = line.find(spec.blockCommentEnd);
        if (e == std::string::npos) {
            fill(out, 0, n, Token::Comment);
            return;
        }
        fill(out, 0, e + spec.blockCommentEnd.size(), Token::Comment);
        i = e + spec.blockCommentEnd.size();
        state = Normal;
    } else if (state == InTag) {
        i = lexTagBody(line, 0, state, out);
    }
    while (i < n) {
        const char c = line[i];
        if (at(line, i, spec.blockCommentStart)) {
            const size_t e = line.find(spec.blockCommentEnd, i + spec.blockCommentStart.size());
            if (e == std::string::npos) {
                fill(out, i, n, Token::Comment);
                state = InBlockComment;
                return;
            }
            fill(out, i, e + spec.blockCommentEnd.size(), Token::Comment);
            i = e + spec.blockCommentEnd.size();
            continue;
        }
        if (c == '<' && i + 1 < n && (line[i + 1] == '!' || line[i + 1] == '?')) { // <!DOCTYPE>, <?xml ?>, CDATA
            const size_t e = line.find('>', i);
            fill(out, i, e == std::string::npos ? n : e + 1, Token::Preprocessor);
            i = e == std::string::npos ? n : e + 1;
            continue;
        }
        if (c == '<' && i + 1 < n && (isIdentStart(line[i + 1]) || line[i + 1] == '/')) {
            size_t j = i + 1;
            if (line[j] == '/')
                ++j;
            while (j < n && (isIdent(line[j]) || line[j] == '-' || line[j] == ':' || line[j] == '.'))
                ++j;
            fill(out, i, j, Token::Tag);
            i = lexTagBody(line, j, state, out);
            continue;
        }
        if (c == '&') {
            const size_t e = line.find(';', i);
            if (e != std::string::npos && e - i < 12) {
                fill(out, i, e + 1, Token::Variable);
                i = e + 1;
                continue;
            }
        }
        ++i;
    }
}

// ---- Delimited (CSV / TSV) ----------------------------------------------------------------------

void lexDelimited(const std::string& line, const LanguageSpec& spec, std::vector<Token>& out)
{
    char delimiter = spec.delimiter;
    if (!delimiter)
        delimiter = line.find('\t') != std::string::npos ? '\t'
                    : line.find(',') == std::string::npos && line.find(';') != std::string::npos ? ';'
                                                                                               : ',';
    const size_t n = line.size();
    int column = 0;
    size_t start = 0;
    bool quoted = false;
    for (size_t i = 0; i <= n; ++i) {
        if (i < n && line[i] == '"')
            quoted = !quoted;
        if (i == n || (line[i] == delimiter && !quoted)) {
            fill(out, start, i, Token(int(Token::Column1) + column % 6));
            ++column;
            start = i + 1;
        }
    }
}

} // namespace

void highlightLine(const std::string& line, const LanguageSpec& spec, int& state, std::vector<Token>& out)
{
    out.assign(line.size(), Token::Text);
    if (spec.id == Language::PlainText)
        return;
    switch (spec.lexer) {
    case Lexer::Markdown: lexMarkdown(line, state, out); return;
    case Lexer::Markup: lexMarkup(line, spec, state, out); return;
    case Lexer::Delimited: lexDelimited(line, spec, out); return;
    case Lexer::Code: lexCode(line, spec, state, out); return;
    }
}

int HighlightCache::stateAt(const TextBuffer& buffer, const LanguageSpec& spec, int line)
{
    if (spec_ != &spec) {
        spec_ = &spec;
        states_.clear();
    } else if (version_ != buffer.version()) {
        // The state at the start of the first changed line is still right; below it is not.
        const int first = version_ == UINT64_MAX ? 0 : buffer.firstLineChangedSince(version_);
        if (size_t(first) + 1 < states_.size())
            states_.resize(size_t(first) + 1);
    }
    version_ = buffer.version();
    if (states_.empty())
        states_.push_back(0);
    line = std::max(0, std::min(line, buffer.lineCount()));
    while (int(states_.size()) <= line) {
        const int l = int(states_.size()) - 1;
        int state = states_.back();
        highlightLine(buffer.line(l), spec, state, scratch_);
        states_.push_back(state);
    }
    return states_[size_t(line)];
}

} // namespace enp::model
