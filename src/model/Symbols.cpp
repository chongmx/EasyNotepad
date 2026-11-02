#include "enp/model/Symbols.h"

#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace enp::model {

namespace {

// How a language declares things; the extraction rules differ per family.
enum class Family { None, CLike, Script, Rust, Go, Python, Lua, Shell, PowerShell, CMake };

Family familyOf(Language language)
{
    switch (language) {
    case Language::Cpp:
    case Language::CSharp:
    case Language::Java: return Family::CLike;
    case Language::JavaScript:
    case Language::TypeScript: return Family::Script;
    case Language::Rust: return Family::Rust;
    case Language::Go: return Family::Go;
    case Language::Python: return Family::Python;
    case Language::Lua: return Family::Lua;
    case Language::Shell: return Family::Shell;
    case Language::PowerShell: return Family::PowerShell;
    case Language::CMake: return Family::CMake;
    default: return Family::None;
    }
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

bool isUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool isLower(char c) { return c >= 'a' && c <= 'z'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }
char lower(char c) { return isUpper(c) ? char(c + ('a' - 'A')) : c; }

bool isOneOf(std::string_view word, std::initializer_list<const char*> words)
{
    for (const char* w : words)
        if (word == w)
            return true;
    return false;
}

// SCREAMING_CASE (two characters or more), or kConstant.
bool isConstantName(std::string_view name)
{
    if (name.size() < 2)
        return false;
    if (name[0] == 'k' && isUpper(name[1]))
        return true;
    bool letter = false;
    for (char c : name) {
        if (isLower(c))
            return false;
        letter = letter || isUpper(c);
    }
    return letter;
}

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && isSpace(s.back()))
        s.remove_suffix(1);
    return s;
}

// The declaring line as shown in a tooltip: trimmed, tabs as spaces, cut on a character boundary.
std::string detailOf(std::string_view line)
{
    line = trim(line);
    if (line.size() > kMaxSymbolDetail) {
        size_t cut = kMaxSymbolDetail;
        while (cut > 0 && (static_cast<unsigned char>(line[cut]) & 0xC0) == 0x80)
            --cut;
        line = line.substr(0, cut);
    }
    std::string detail(line);
    std::replace(detail.begin(), detail.end(), '\t', ' ');
    return detail;
}

// An identifier of the cleaned line: [begin, end).
struct Tok {
    size_t begin = 0;
    size_t end = 0;
};

// Comments and strings blanked out (replaced by spaces), so that brackets, semicolons and words
// inside them are not mistaken for code. Offsets stay those of the original line.
class Cleaner {
public:
    Cleaner(const LanguageSpec& spec, Family family) : spec_(spec), family_(family) {}

    void clean(std::string_view line, std::string& out)
    {
        out.assign(line.data(), line.size());
        const size_t n = line.size();
        size_t i = 0;
        auto blank = [&](size_t from, size_t to) {
            for (size_t k = from; k < to && k < n; ++k)
                out[k] = ' ';
        };
        auto at = [&](size_t pos, const std::string& token) { return !token.empty() && line.compare(pos, token.size(), token) == 0; };
        while (i < n) {
            if (state_ != State::Code) {
                const std::string& end = state_ == State::BlockComment ? spec_.blockCommentEnd : delimiter_;
                size_t close = line.find(end, i);
                while (state_ == State::String && close != std::string::npos && close > i && line[close - 1] == '\\')
                    close = line.find(end, close + 1);
                if (close == std::string::npos) {
                    blank(i, n);
                    return;
                }
                blank(i, close + end.size());
                i = close + end.size();
                state_ = State::Code;
                continue;
            }
            const char c = line[i];
            if (at(i, spec_.lineComment)) {
                // Shell: "$#" and "${#name}" are not comments.
                if (!(family_ == Family::Shell && i > 0 && (line[i - 1] == '$' || line[i - 1] == '{'))) {
                    blank(i, n);
                    return;
                }
            }
            if (at(i, spec_.blockCommentStart)) {
                blank(i, i + spec_.blockCommentStart.size());
                i += spec_.blockCommentStart.size();
                state_ = State::BlockComment;
                continue;
            }
            if (spec_.tripleQuotes && i + 2 < n && (c == '"' || c == '\'') && line[i + 1] == c && line[i + 2] == c) {
                delimiter_.assign(3, c);
                blank(i, i + 3);
                i += 3;
                state_ = State::String;
                continue;
            }
            if (spec_.multilineBacktick && c == '`') {
                delimiter_.assign(1, '`');
                blank(i, i + 1);
                ++i;
                state_ = State::String;
                continue;
            }
            if (spec_.quotes.find(c) != std::string::npos) {
                // Rust: 'a is a lifetime unless it is a character ('a', '\n').
                if (family_ == Family::Rust && c == '\'' && !(i + 2 < n && line[i + 2] == '\'') && !(i + 1 < n && line[i + 1] == '\\')) {
                    out[i] = ' ';
                    ++i;
                    continue;
                }
                size_t j = i + 1;
                while (j < n && line[j] != c)
                    j += line[j] == '\\' ? 2 : 1;
                blank(i, j + 1);
                i = j + 1;
                continue;
            }
            ++i;
        }
    }

private:
    enum class State { Code, BlockComment, String };
    const LanguageSpec& spec_;
    Family family_;
    State state_ = State::Code;
    std::string delimiter_; // closes the string that spans lines
};

// Words that may come before a declaring keyword or a declaration's type.
bool isModifier(std::string_view w)
{
    return isOneOf(w, {"export", "default", "pub", "public", "private", "protected", "internal", "static", "async",
                       "abstract", "sealed", "final", "partial", "unsafe", "inline", "virtual", "declare", "extern",
                       "readonly", "override", "new", "typedef", "friend", "const", "constexpr", "mutable", "volatile",
                       "local", "global", "open", "data", "file", "required", "explicit", "consteval", "constinit",
                       "fileprivate", "synchronized", "native", "transient", "strictfp", "get", "set", "accessor",
                       "crate", "super"});
}

// Statements, not declarations: a line starting with one of these declares nothing.
bool isStatementWord(std::string_view w)
{
    return isOneOf(w, {"return", "else", "case", "goto", "throw", "new", "delete", "co_return", "co_yield", "co_await",
                       "using", "break", "continue", "if", "for", "while", "do", "switch", "await", "yield", "typeof",
                       "sizeof", "assert", "static_assert", "import", "package", "from", "with", "try", "catch"});
}

class Parser {
public:
    Parser(Language language, int file, std::vector<Symbol>& out)
        : spec_(languageSpec(language)), family_(familyOf(language)), file_(file), out_(out), cleaner_(spec_, family_)
    {
    }

    void run(std::string_view text)
    {
        size_t start = 0;
        while (start <= text.size()) {
            size_t end = text.find_first_of("\r\n", start);
            if (end == std::string_view::npos)
                end = text.size();
            raw_ = text.substr(start, end - start);
            cleaner_.clean(raw_, code_);
            if (family_ == Family::Python)
                pythonLine();
            else
                bracedLine();
            ++line_;
            if (end >= text.size())
                break;
            start = end + ((text[end] == '\r' && end + 1 < text.size() && text[end + 1] == '\n') ? 2 : 1);
        }
    }

private:
    // A block opened by '{': a type or namespace (its declarations are indexed), or a body.
    struct Block {
        std::string name; // the type's or namespace's name ("" for a body or an anonymous namespace)
        bool container = false;
        bool type = false;
        bool isEnum = false;
        bool typedefBody = false; // typedef struct { ... } Name;
    };
    struct PyBlock {
        int indent = 0;
        std::string name;
        bool isClass = false;
    };

    const LanguageSpec& spec_;
    Family family_;
    int file_;
    std::vector<Symbol>& out_;
    Cleaner cleaner_;
    std::string_view raw_;
    std::string code_;
    std::string word_; // scratch for keyword lookups
    std::vector<Tok> toks_;
    int line_ = 0;
    int parenDepth_ = 0;
    std::vector<Block> blocks_;
    bool hasPending_ = false; // a type or namespace header waiting for its '{'
    Block pending_;
    bool closedTypedef_ = false; // the last '}' closed a typedef's body: the name follows
    std::vector<PyBlock> py_;

    std::string_view word(const Tok& t) const { return std::string_view(code_).substr(t.begin, t.end - t.begin); }

    bool isKeyword(std::string_view w)
    {
        word_.assign(w.data(), w.size());
        if (spec_.caseInsensitive)
            for (char& c : word_)
                c = lower(c);
        return spec_.keywords.count(word_) > 0 || spec_.types.count(word_) > 0;
    }

    bool atContainerLevel() const
    {
        for (const Block& b : blocks_)
            if (!b.container)
                return false;
        return true;
    }
    const Block* innermost() const { return blocks_.empty() ? nullptr : &blocks_.back(); }
    std::string_view containerName() const
    {
        for (auto it = blocks_.rbegin(); it != blocks_.rend(); ++it)
            if (it->container && !it->name.empty())
                return it->name;
        return {};
    }

    void emit(std::string_view name, size_t column, SymbolKind kind, bool definition, std::string_view container)
    {
        if (name.empty() || isDigit(name[0]))
            return;
        Symbol& s = out_.emplace_back();
        s.name.assign(name.data(), name.size());
        s.container.assign(container.data(), container.size());
        s.detail = detailOf(raw_);
        s.file = file_;
        s.line = line_;
        s.column = int(column);
        s.kind = kind;
        s.definition = definition;
    }

    // Identifiers of code_[from, to).
    void tokenize(size_t from, size_t to)
    {
        toks_.clear();
        size_t i = from;
        while (i < to) {
            if (isIdentifierStart(code_[i]) && !(i > from && isDigit(code_[i - 1]))) {
                const size_t b = i;
                while (i < to && (isIdentifierChar(code_[i]) || (spec_.dashInIdentifiers && code_[i] == '-')))
                    ++i;
                toks_.push_back({b, i});
            } else if (isDigit(code_[i])) {
                while (i < to && isIdentifierChar(code_[i]))
                    ++i;
            } else {
                ++i;
            }
        }
    }

    // First non-space character at or after `pos` (before `to`), or 0.
    char nextChar(size_t pos, size_t to) const
    {
        while (pos < to && isSpace(code_[pos]))
            ++pos;
        return pos < to ? code_[pos] : '\0';
    }
    // Last non-space character before `pos` (at or after `from`), or 0.
    char prevChar(size_t pos, size_t from) const
    {
        while (pos > from && isSpace(code_[pos - 1]))
            --pos;
        return pos > from ? code_[pos - 1] : '\0';
    }
    // Skips a <...> group starting at `pos` (which holds '<'); returns the position after it.
    size_t skipAngles(size_t pos, size_t to) const
    {
        int depth = 0;
        for (size_t i = pos; i < to; ++i) {
            if (code_[i] == '<')
                ++depth;
            else if (code_[i] == '>' && --depth == 0)
                return i + 1;
        }
        return to;
    }

    // ---- Languages with braces (and those without blocks at all) ------------------------------

    void bracedLine()
    {
        const size_t n = code_.size();
        size_t first = 0;
        while (first < n && isSpace(code_[first]))
            ++first;
        if (first < n && spec_.preprocessor && code_[first] == '#' && family_ == Family::CLike) {
            directive(first);
            return;
        }
        size_t segment = 0;
        int segmentDepth = parenDepth_;
        for (size_t i = 0; i < n; ++i) {
            const char c = code_[i];
            if (c == '(' || c == '[') {
                ++parenDepth_;
            } else if (c == ')' || c == ']') {
                if (parenDepth_ > 0)
                    --parenDepth_;
            } else if (c == '{' || c == '}' || c == ';') {
                if (segmentDepth == 0)
                    analyze(segment, i, c);
                if (c == '{') {
                    if (hasPending_ && parenDepth_ == 0) {
                        blocks_.push_back(std::move(pending_));
                        pending_ = Block{};
                        hasPending_ = false;
                    } else {
                        blocks_.push_back(Block{});
                    }
                } else if (c == '}') {
                    closedTypedef_ = !blocks_.empty() && blocks_.back().typedefBody;
                    if (!blocks_.empty())
                        blocks_.pop_back();
                } else if (parenDepth_ == 0) {
                    hasPending_ = false; // a forward declaration: no body follows
                }
                segment = i + 1;
                segmentDepth = parenDepth_;
            }
        }
        if (segmentDepth == 0)
            analyze(segment, n, '\0');
    }

    void directive(size_t hash)
    {
        tokenize(hash + 1, code_.size());
        if (toks_.size() >= 2 && word(toks_[0]) == "define")
            emit(word(toks_[1]), toks_[1].begin, SymbolKind::Macro, true, {});
    }

    // One statement piece of the line: code_[from, to), ended by `term` ('{', '}', ';' or 0 for
    // the end of the line). Only pieces that start outside parentheses get here.
    void analyze(size_t from, size_t to, char term)
    {
        const bool afterTypedef = closedTypedef_;
        closedTypedef_ = false;
        // Access labels ("public:", "signals:") are not part of the declaration.
        tokenize(from, to);
        size_t t = 0;
        while (t < toks_.size() && isOneOf(word(toks_[t]), {"public", "private", "protected", "signals", "slots"}) &&
               nextChar(toks_[t].end, to) == ':' && !(toks_[t].end + 1 < to && code_.compare(toks_[t].end, 2, "::") == 0)) {
            from = code_.find(':', toks_[t].end) + 1;
            ++t;
        }
        if (t > 0)
            tokenize(from, to);
        if (toks_.empty())
            return;
        if (afterTypedef && family_ == Family::CLike && term == ';') { // typedef struct { ... } Name;
            emit(word(toks_.back()), toks_.back().begin, SymbolKind::Type, true, containerName());
            return;
        }
        // C++ templates: the parameter list is not part of the declaration.
        if (word(toks_[0]) == "template" && nextChar(toks_[0].end, to) == '<') {
            size_t at = toks_[0].end;
            while (code_[at] != '<')
                ++at;
            from = skipAngles(at, to);
            tokenize(from, to);
            if (toks_.empty())
                return;
        }
        const Block* inner = innermost();
        if (inner && inner->isEnum && family_ != Family::Go) {
            enumerators(from, to, inner->name);
            return;
        }
        if (keywordDeclaration(from, to, term))
            return;
        switch (family_) {
        case Family::CLike:
            if (atContainerLevel() && !cFunction(from, to, term))
                cVariables(from, to, term);
            break;
        case Family::Script:
            if (inner && inner->type)
                scriptMember(from, to, term);
            break;
        case Family::Shell:
            shellLine(from, to);
            break;
        default:
            break;
        }
    }

    // enum { A, B = 2, C }: the first word of every comma-separated item.
    void enumerators(size_t from, size_t to, std::string_view container)
    {
        bool expectName = true;
        bool annotation = false; // @Name before an item (Java)
        int depth = 0;
        for (size_t i = from; i < to; ++i) {
            const char c = code_[i];
            if (c == '(' || c == '[' || c == '{')
                ++depth;
            else if ((c == ')' || c == ']' || c == '}') && depth > 0)
                --depth;
            else if (c == ',' && depth == 0)
                expectName = true;
            else if (c == '#')
                continue; // #[attribute]: its words are inside the brackets
            else if (c == '@')
                annotation = true;
            else if (annotation && isIdentifierStart(c)) {
                while (i + 1 < to && isIdentifierChar(code_[i + 1]))
                    ++i;
                annotation = false;
            } else if (expectName && isIdentifierStart(c) && depth == 0) {
                size_t e = i;
                while (e < to && isIdentifierChar(code_[e]))
                    ++e;
                const std::string_view name(code_.data() + i, e - i);
                if (!isKeyword(name))
                    emit(name, i, SymbolKind::Constant, true, container);
                expectName = false;
                i = e - 1;
            } else if (!isSpace(c) && depth == 0) {
                expectName = false;
            }
        }
    }

    // Declarations that begin with a word saying what they are: class, fn, def, function, type...
    bool keywordDeclaration(size_t from, size_t to, char term)
    {
        size_t k = 0;
        // extern "C" { ... }: its declarations are file level.
        if (family_ == Family::CLike && toks_.size() == 1 && word(toks_[0]) == "extern" && term == '{') {
            pending_ = Block{"", true, false, false, false};
            hasPending_ = true;
            return true;
        }
        bool typedefSeen = false;
        while (k < toks_.size() && isModifier(word(toks_[k])) && !isDeclaringKeyword(word(toks_[k]))) {
            typedefSeen = typedefSeen || word(toks_[k]) == "typedef";
            ++k;
        }
        if (k >= toks_.size())
            return false;
        // "const" and "static" are modifiers in C-like languages, declaring keywords elsewhere.
        const std::string_view kw = word(toks_[k]);
        if (!isDeclaringKeyword(kw)) {
            if (typedefSeen && family_ == Family::CLike)
                return typedefName(from, to, term);
            return false;
        }
        const std::string_view container = containerName();
        const bool topLevel = atContainerLevel();

        if (isOneOf(kw, {"class", "struct", "union", "interface", "trait", "record", "enum", "protocol"})) {
            if (family_ == Family::Python)
                return false;
            // enum class / enum struct
            size_t n = k + 1;
            if (kw == "enum" && n < toks_.size() && isOneOf(word(toks_[n]), {"class", "struct"}))
                ++n;
            // The name: the last word before the base list, the body or the template arguments
            // ("class ENP_API Foo final : public Bar").
            const Tok* name = nullptr;
            for (size_t i = n; i < toks_.size(); ++i) {
                const std::string_view w = word(toks_[i]);
                if (isOneOf(w, {"extends", "implements", "where", "with", "of"}))
                    break;
                if (!isOneOf(w, {"final", "sealed", "abstract", "open"}))
                    name = &toks_[i];
                const char next = nextChar(toks_[i].end, to);
                if (next != '\0' && !isIdentifierStart(next))
                    break;
            }
            if (!name) { // anonymous: typedef struct { ... } Name; names it after the body
                if (typedefSeen && term == '{') {
                    pending_ = Block{"", true, true, false, true};
                    hasPending_ = true;
                }
                return true;
            }
            // "struct Foo* make()", "struct Foo foo = {...}": the type is used, not declared.
            const char after = nextChar(name->end, to);
            if (after == '*' || after == '&' || after == ')' || after == ',' || after == '=')
                return family_ == Family::CLike ? cFunction(from, to, term) : true;
            if (term == ';' && family_ != Family::Rust && !typedefSeen)
                return true; // forward declaration
            if (!topLevel && family_ != Family::Lua)
                return true; // a type inside a function body
            emit(word(*name), name->begin, SymbolKind::Type, true, container);
            pending_ = Block{std::string(word(*name)), true, true, kw == "enum", typedefSeen};
            hasPending_ = term != ';';
            return true;
        }
        if (isOneOf(kw, {"namespace", "mod", "module"})) {
            if (!topLevel)
                return true;
            const Tok* name = nullptr;
            for (size_t i = k + 1; i < toks_.size(); ++i)
                name = &toks_[i];
            if (name)
                emit(word(*name), name->begin, SymbolKind::Namespace, term != ';', container);
            pending_ = Block{name ? std::string(word(*name)) : std::string(), true, false, false, false};
            hasPending_ = term != ';';
            return true;
        }
        if (kw == "impl") { // Rust: impl Type / impl Trait for Type
            size_t i = k + 1;
            const Tok* name = nullptr;
            for (size_t j = i; j < toks_.size(); ++j)
                if (word(toks_[j]) == "for" && j + 1 < toks_.size()) {
                    name = &toks_[j + 1];
                    break;
                }
            if (!name) {
                size_t at = toks_[k].end;
                if (nextChar(at, to) == '<') {
                    while (code_[at] != '<')
                        ++at;
                    at = skipAngles(at, to);
                }
                for (size_t j = i; j < toks_.size() && !name; ++j)
                    if (toks_[j].begin >= at)
                        name = &toks_[j];
            }
            pending_ = Block{name ? std::string(word(*name)) : std::string(), true, true, false, false};
            hasPending_ = true;
            return true;
        }
        if (isOneOf(kw, {"fn", "func", "function", "def", "sub", "filter", "macro"})) {
            return functionKeyword(k, to, term, container);
        }
        if (kw == "type") { // Go, TypeScript, Rust
            if (k + 1 >= toks_.size() || !topLevel)
                return true;
            const Tok& name = toks_[k + 1];
            emit(word(name), name.begin, SymbolKind::Type, true, container);
            pending_ = Block{std::string(word(name)), true, true, false, false};
            hasPending_ = term == '{'; // "type Id int" has no body
            return true;
        }
        if (kw == "using") { // C++ / C#: using Name = ...;
            if (k + 1 < toks_.size() && nextChar(toks_[k + 1].end, to) == '=' && topLevel)
                emit(word(toks_[k + 1]), toks_[k + 1].begin, SymbolKind::Type, true, container);
            return true;
        }
        if (isOneOf(kw, {"const", "let", "var", "static", "local", "val"})) {
            if (family_ == Family::Lua && raw_.size() > 0 && isSpace(raw_[0]))
                return true; // only file-level locals
            if (!topLevel && family_ != Family::Lua)
                return true;
            size_t i = k + 1;
            while (i < toks_.size() && isOneOf(word(toks_[i]), {"mut", "ref"}))
                ++i;
            if (i >= toks_.size())
                return true;
            if (isOneOf(word(toks_[i]), {"fn", "function"}))
                return functionKeyword(i, to, term, container);
            if (family_ == Family::Go && nextChar(toks_[k].end, to) == '(')
                return true; // grouped: const ( ... )
            const std::string_view name = word(toks_[i]);
            const std::string_view rest(code_.data() + toks_[i].end, to - toks_[i].end);
            SymbolKind kind = SymbolKind::Variable;
            if (rest.find("=>") != std::string_view::npos || rest.find("function") != std::string_view::npos)
                kind = SymbolKind::Function;
            else if ((kw == "const" || kw == "static") && (family_ != Family::Script || isConstantName(name)))
                kind = SymbolKind::Constant;
            else if (isConstantName(name))
                kind = SymbolKind::Constant;
            emit(name, toks_[i].begin, kind, true, container);
            return true;
        }
        return false;
    }

    bool isDeclaringKeyword(std::string_view w) const
    {
        switch (family_) {
        case Family::CLike:
            return isOneOf(w, {"class", "struct", "union", "enum", "interface", "record", "namespace", "using"});
        case Family::Script:
            return isOneOf(w, {"class", "interface", "enum", "namespace", "module", "type", "function", "const", "let", "var"});
        case Family::Rust:
            return isOneOf(w, {"struct", "enum", "trait", "union", "mod", "fn", "impl", "type", "const", "static"});
        case Family::Go:
            return isOneOf(w, {"type", "func", "var", "const"});
        case Family::Lua:
            return isOneOf(w, {"function", "local"});
        case Family::Shell:
            return w == "function";
        case Family::PowerShell:
            return isOneOf(w, {"function", "filter", "class", "enum"});
        case Family::CMake:
            return isOneOf(w, {"function", "macro"});
        default:
            return false;
        }
    }

    // fn / func / function / def name(...): Go receivers and Lua's a.b:c qualify the name.
    bool functionKeyword(size_t k, size_t to, char term, std::string_view container)
    {
        size_t after = toks_[k].end;
        std::string_view owner = container;
        if (family_ == Family::CMake) { // function(name ...)
            if (k + 1 < toks_.size())
                emit(word(toks_[k + 1]), toks_[k + 1].begin, SymbolKind::Function, true, {});
            return true;
        }
        size_t i = k + 1;
        if (family_ == Family::Go && nextChar(after, to) == '(') { // func (r *Recv) Name(
            size_t close = code_.find(')', after);
            if (close == std::string::npos || close >= to)
                return true;
            const Tok* receiver = nullptr;
            while (i < toks_.size() && toks_[i].begin < close)
                receiver = &toks_[i++];
            if (receiver)
                owner = word(*receiver);
        }
        if (i >= toks_.size())
            return true; // anonymous: function (...) {
        // Lua: function a.b:c(...)
        while (family_ == Family::Lua && i + 1 < toks_.size() && toks_[i + 1].begin == toks_[i].end + 1 &&
               (code_[toks_[i].end] == '.' || code_[toks_[i].end] == ':')) {
            owner = word(toks_[i]);
            ++i;
        }
        if (family_ != Family::Lua && family_ != Family::Shell && family_ != Family::PowerShell && !atContainerLevel())
            return true; // a function inside a function body
        emit(word(toks_[i]), toks_[i].begin, SymbolKind::Function, term != ';', owner);
        return true;
    }

    // typedef ... Name; and typedef ret (*Name)(...);
    bool typedefName(size_t from, size_t to, char term)
    {
        if (term == '{' || !atContainerLevel())
            return true;
        const size_t fp = code_.find("(*", from);
        if (fp != std::string::npos && fp < to) {
            for (const Tok& t : toks_)
                if (t.begin > fp) {
                    emit(word(t), t.begin, SymbolKind::Type, true, containerName());
                    break;
                }
            return true;
        }
        emit(word(toks_.back()), toks_.back().begin, SymbolKind::Type, true, containerName());
        return true;
    }

    // C, C++, C#, Java: "type name(" at file, namespace or class level.
    bool cFunction(size_t from, size_t to, char term)
    {
        const size_t paren = code_.find('(', from);
        if (paren == std::string::npos || paren >= to)
            return false;
        const size_t equals = code_.find('=', from);
        if (equals < paren)
            return false; // Type name = make(...);
        // The name right before the parenthesis.
        size_t e = paren;
        while (e > from && isSpace(code_[e - 1]))
            --e;
        size_t b = e;
        while (b > from && isIdentifierChar(code_[b - 1]))
            --b;
        if (b == e || !isIdentifierStart(code_[b]))
            return true; // operator(), (*fp)(...): nothing to index, and no variable either
        const std::string_view name(code_.data() + b, e - b);
        if (isKeyword(name) || isStatementWord(name))
            return true;
        // Foo::bar( / Foo<T>::bar(
        size_t q = b;
        while (q > from && isSpace(code_[q - 1]))
            --q;
        std::string_view owner;
        if (q >= from + 2 && code_[q - 1] == ':' && code_[q - 2] == ':') {
            size_t oe = q - 2;
            while (oe > from && isSpace(code_[oe - 1]))
                --oe;
            if (oe > from && code_[oe - 1] == '>') { // skip template arguments backwards
                int depth = 0;
                while (oe > from) {
                    const char c = code_[--oe];
                    if (c == '>')
                        ++depth;
                    else if (c == '<' && --depth == 0)
                        break;
                }
            }
            size_t ob = oe;
            while (ob > from && isIdentifierChar(code_[ob - 1]))
                --ob;
            owner = std::string_view(code_.data() + ob, oe - ob);
            q = ob;
            while (q > from && isSpace(code_[q - 1]))
                --q;
            if (q >= from + 2 && code_[q - 1] == ':' && code_[q - 2] == ':') { // ns::Foo::bar(
                q -= 2;
                while (q > from && (isIdentifierChar(code_[q - 1]) || code_[q - 1] == ':'))
                    --q;
            }
        }
        if (q > from && code_[q - 1] == '~')
            return true; // destructor
        const std::string_view before = trim(std::string_view(code_.data() + from, q - from));
        for (char c : before)
            if (c == '=' || c == '.' || c == '(' || c == ')' || c == '"' || c == '!' || c == '+' || c == '|' || c == '?' || c == '{')
                return true;
        if (before.find("->") != std::string_view::npos || before.find("<<") != std::string_view::npos)
            return true;
        tokenize(from, q);
        for (const Tok& t : toks_)
            if (isStatementWord(word(t)))
                return true;
        const Block* inner = innermost();
        if (before.empty()) {
            // Only constructors have no type: Foo::Foo( or Foo( inside class Foo. A bare NAME(...)
            // at file level is a macro call (TEST(...), Q_OBJECT-like).
            const bool constructor = (!owner.empty() && owner == name) || (inner && inner->type && inner->name == name);
            if (!constructor)
                return true;
        } else {
            const char last = before.back();
            if (!(isIdentifierChar(last) || last == '*' || last == '&' || last == '>' || last == ']'))
                return true;
        }
        if (owner.empty())
            owner = containerName();
        emit(name, b, SymbolKind::Function, term != ';', owner);
        return true;
    }

    // Type name [= value][, other];  at file, namespace or class level.
    void cVariables(size_t from, size_t to, char term)
    {
        const Block* inner = innermost();
        const bool csharpProperty = term == '{' && inner && inner->type && spec_.id == Language::CSharp;
        const std::string_view seg(code_.data() + from, to - from);
        if (term != ';' && !csharpProperty && !(term == '{' && seg.find('=') != std::string_view::npos))
            return;
        if (seg.find('(') != std::string_view::npos && seg.find('(') < seg.find('='))
            return;
        // Split into declarators at commas outside brackets.
        size_t partBegin = from;
        int depth = 0;
        bool firstPart = true;
        bool constant = false;
        for (size_t i = from; i <= to; ++i) {
            const char c = i < to ? code_[i] : ',';
            if (c == '<' || c == '(' || c == '[' || c == '{')
                ++depth;
            else if ((c == '>' || c == ')' || c == ']' || c == '}') && depth > 0)
                --depth;
            if (c != ',' || depth != 0)
                continue;
            // The name: the last word before '=', '[', '{' or a bit-field's ':'.
            size_t end = partBegin;
            while (end < i && code_[end] != '=' && code_[end] != '[' &&
                   !(code_[end] == ':' && !(end + 1 < i && code_[end + 1] == ':') && !(end > partBegin && code_[end - 1] == ':')))
                ++end;
            tokenize(partBegin, end);
            if (toks_.empty())
                return;
            if (firstPart) {
                if (toks_.size() < 2)
                    return; // no type: an expression or a macro
                for (const Tok& t : toks_) {
                    const std::string_view w = word(t);
                    if (isStatementWord(w) || isOneOf(w, {"friend", "operator", "template", "namespace", "typedef", "default"}))
                        return;
                    constant = constant || isOneOf(w, {"const", "constexpr", "final", "readonly", "constinit"});
                }
                const char before = prevChar(toks_.back().begin, partBegin);
                if (!(isIdentifierChar(before) || before == '*' || before == '&' || before == '>' || before == ']'))
                    return;
            }
            const Tok& name = toks_.back();
            const std::string_view w = word(name);
            if (!isKeyword(w))
                emit(w, name.begin, constant || isConstantName(w) ? SymbolKind::Constant : SymbolKind::Variable, true, containerName());
            firstPart = false;
            partBegin = i + 1;
        }
    }

    // JavaScript / TypeScript class and interface bodies: methods and fields.
    void scriptMember(size_t from, size_t to, char term)
    {
        size_t k = 0;
        while (k < toks_.size() && isModifier(word(toks_[k])) && nextChar(toks_[k].end, to) != '(' &&
               nextChar(toks_[k].end, to) != ':' && nextChar(toks_[k].end, to) != '=')
            ++k;
        if (k >= toks_.size())
            return;
        const Tok& name = toks_[k];
        const std::string_view w = word(name);
        if (isKeyword(w) || isStatementWord(w) || w == "constructor" || prevChar(name.begin, from) == '.')
            return;
        size_t after = name.end;
        char next = nextChar(after, to);
        if (next == '?' || next == '!') {
            while (after < to && code_[after] != next)
                ++after;
            next = nextChar(after + 1, to);
        }
        if (next == '(' || next == '<')
            emit(w, name.begin, SymbolKind::Function, term == '{', innermost()->name);
        else if (next == ':' || next == '=' || next == '\0')
            if (term == ';' || next != '\0')
                emit(w, name.begin, SymbolKind::Variable, true, innermost()->name);
    }

    // Shell: name() { ... } and NAME=value at the start of a line.
    void shellLine(size_t from, size_t to)
    {
        if (toks_.empty() || !blocks_.empty() || isKeyword(word(toks_[0])))
            return;
        const Tok& t = toks_[0];
        size_t p = t.end;
        while (p < to && isSpace(code_[p]))
            ++p;
        if (p + 1 < to && code_[p] == '(' && nextChar(p + 1, to) == ')') {
            emit(word(t), t.begin, SymbolKind::Function, true, {});
            return;
        }
        if (t.begin == 0 && t.end < to && code_[t.end] == '=')
            emit(word(t), t.begin, isConstantName(word(t)) ? SymbolKind::Constant : SymbolKind::Variable, true, {});
        (void)from;
    }

    // ---- Python: blocks by indentation ---------------------------------------------------------

    void pythonLine()
    {
        const size_t n = code_.size();
        size_t first = 0;
        int indent = 0;
        while (first < n && isSpace(code_[first])) {
            indent += code_[first] == '\t' ? 8 - indent % 8 : 1;
            ++first;
        }
        if (first == n)
            return; // blank or only a comment: the blocks stay
        const int depthBefore = parenDepth_;
        for (size_t i = first; i < n; ++i) {
            const char c = code_[i];
            if (c == '(' || c == '[' || c == '{')
                ++parenDepth_;
            else if ((c == ')' || c == ']' || c == '}') && parenDepth_ > 0)
                --parenDepth_;
        }
        if (depthBefore > 0)
            return; // a continued line
        while (!py_.empty() && py_.back().indent >= indent)
            py_.pop_back();
        tokenize(first, n);
        if (toks_.empty())
            return;
        size_t k = 0;
        if (word(toks_[0]) == "async")
            ++k;
        if (k >= toks_.size())
            return;
        const std::string_view kw = word(toks_[k]);
        const bool inClass = !py_.empty() && py_.back().isClass;
        const std::string_view owner = inClass ? std::string_view(py_.back().name) : std::string_view();
        if ((kw == "class" || kw == "def") && k + 1 < toks_.size()) {
            const Tok& name = toks_[k + 1];
            if (py_.empty() || inClass)
                emit(word(name), name.begin, kw == "class" ? SymbolKind::Type : SymbolKind::Function, true, owner);
            py_.push_back({indent, std::string(word(name)), kw == "class"});
            return;
        }
        if (code_[first] == '@' || !(py_.empty() || inClass) || isKeyword(kw))
            return;
        // NAME = value, NAME: type = value, NAME: type
        const char next = nextChar(toks_[0].end, n);
        size_t eq = toks_[0].end;
        while (eq < n && isSpace(code_[eq]))
            ++eq;
        const bool assignment = next == '=' && !(eq + 1 < n && code_[eq + 1] == '=');
        if (assignment || next == ':')
            emit(kw, toks_[0].begin, isConstantName(kw) ? SymbolKind::Constant : SymbolKind::Variable, true, owner);
    }
};

} // namespace

const char* symbolKindName(SymbolKind kind)
{
    switch (kind) {
    case SymbolKind::Keyword: return "keyword";
    case SymbolKind::Word: return "word";
    case SymbolKind::Namespace: return "namespace";
    case SymbolKind::Type: return "type";
    case SymbolKind::Function: return "function";
    case SymbolKind::Variable: return "variable";
    case SymbolKind::Constant: return "constant";
    case SymbolKind::Macro: return "macro";
    }
    return "";
}

std::pair<int, int> identifierAround(std::string_view line, int column)
{
    const int n = int(line.size());
    column = std::clamp(column, 0, n);
    int b = column;
    while (b > 0 && isIdentifierChar(line[size_t(b - 1)]))
        --b;
    int e = column;
    while (e < n && isIdentifierChar(line[size_t(e)]))
        ++e;
    while (b < e && isDigit(line[size_t(b)])) // a number is not a name
        ++b;
    if (b == e || !isIdentifierStart(line[size_t(b)]))
        return {column, column};
    return {b, e};
}

bool canExtractSymbols(Language language)
{
    return familyOf(language) != Family::None;
}

void extractSymbols(std::string_view text, Language language, int file, std::vector<Symbol>& out)
{
    if (!canExtractSymbols(language))
        return;
    Parser parser(language, file, out);
    parser.run(text);
}

int matchScore(std::string_view pattern, std::string_view word)
{
    if (pattern.empty())
        return 0;
    if (pattern.size() > word.size())
        return -1;
    const int extra = int(std::min<size_t>(word.size() - pattern.size(), 60));
    if (word.compare(0, pattern.size(), pattern) == 0)
        return 1200 - extra;
    bool prefix = true;
    for (size_t i = 0; i < pattern.size() && prefix; ++i)
        prefix = lower(pattern[i]) == lower(word[i]);
    if (prefix)
        return 1000 - extra;
    // A word part starts here: the start, after '_' or '$', a capital after a small letter, a
    // digit after a letter.
    auto boundary = [&](size_t i) {
        if (i == 0)
            return true;
        const char a = word[i - 1], b = word[i];
        return ((a == '_' || a == '$' || a == '-') && b != '_') || (isLower(a) && isUpper(b)) || (!isDigit(a) && isDigit(b));
    };
    int score = 0;
    size_t i = 0;
    for (size_t j = 0; j < pattern.size(); ++j) {
        const char pc = lower(pattern[j]);
        if (j > 0 && i < word.size() && lower(word[i]) == pc) { // a run
            score += 6 + (word[i] == pattern[j] ? 1 : 0);
            ++i;
            continue;
        }
        size_t found = std::string_view::npos;
        for (size_t k = i; k < word.size(); ++k)
            if (lower(word[k]) == pc && boundary(k)) {
                found = k;
                break;
            }
        if (found == std::string_view::npos && j > 0)
            for (size_t k = i; k < word.size(); ++k)
                if (lower(word[k]) == pc) {
                    found = k;
                    break;
                }
        if (found == std::string_view::npos)
            return -1;
        score += (boundary(found) ? 10 : 1) + (word[found] == pattern[j] ? 1 : 0);
        i = found + 1;
    }
    return std::max(1, 100 + score - extra);
}

// ---- SymbolIndex ------------------------------------------------------------------------------

std::uint32_t SymbolIndex::store(std::string_view text, std::uint16_t& length)
{
    text = text.substr(0, 0xFFFF);
    length = std::uint16_t(text.size());
    const std::uint32_t at = std::uint32_t(text_.size());
    text_.append(text.data(), text.size());
    return at;
}

int SymbolIndex::addFile(std::string_view path)
{
    paths_.append(path.data(), path.size());
    pathEnds_.push_back(std::uint32_t(paths_.size()));
    return int(pathEnds_.size()) - 1;
}

void SymbolIndex::add(std::vector<Symbol>& symbols)
{
    for (const Symbol& s : symbols) {
        Entry e;
        e.name = store(s.name, e.nameLength);
        // The same container as the previous entry (members of one type): stored once.
        if (!entries_.empty() && !s.container.empty() &&
            std::string_view(text_).substr(entries_.back().container, entries_.back().containerLength) == s.container) {
            e.container = entries_.back().container;
            e.containerLength = entries_.back().containerLength;
        } else {
            e.container = store(s.container, e.containerLength);
        }
        e.detail = store(s.detail, e.detailLength);
        e.kind = s.kind;
        e.definition = s.definition;
        e.file = s.file;
        e.line = s.line;
        e.column = s.column;
        entries_.push_back(e);
    }
    symbols.clear(); // the caller reuses the vector for the next file: its capacity stays on purpose
}

void SymbolIndex::replaceFile(std::string_view path, std::vector<Symbol>& symbols)
{
    int file = fileIndex(path);
    if (file < 0)
        file = addFile(path);
    // The pool is written again without the old symbols' text: saving a file a thousand times
    // keeps the index the size of what it holds.
    SymbolIndex kept;
    kept.paths_ = paths_;
    kept.pathEnds_ = pathEnds_;
    kept.text_.reserve(text_.size());
    kept.entries_.reserve(entries_.size() + symbols.size());
    for (const Entry& e : entries_) {
        if (e.file == file)
            continue;
        Entry copy = e;
        copy.name = kept.store(nameOf(e), copy.nameLength);
        copy.container = kept.store(std::string_view(text_).substr(e.container, e.containerLength), copy.containerLength);
        copy.detail = kept.store(std::string_view(text_).substr(e.detail, e.detailLength), copy.detailLength);
        kept.entries_.push_back(copy);
    }
    for (Symbol& s : symbols)
        s.file = file;
    kept.add(symbols);
    kept.finish();
    std::swap(*this, kept); // the old storage goes with `kept`
}

void SymbolIndex::finish()
{
    std::sort(entries_.begin(), entries_.end(), [this](const Entry& a, const Entry& b) {
        if (const int c = nameOf(a).compare(nameOf(b)); c != 0)
            return c < 0;
        if (a.file != b.file)
            return a.file < b.file;
        return a.line < b.line;
    });
    // Exactly what it holds: a large index was grown by doubling.
    text_.shrink_to_fit();
    entries_.shrink_to_fit();
    paths_.shrink_to_fit();
    pathEnds_.shrink_to_fit();
}

void SymbolIndex::release()
{
    std::string().swap(text_);
    std::vector<Entry>().swap(entries_);
    std::string().swap(paths_);
    std::vector<std::uint32_t>().swap(pathEnds_);
}

SymbolView SymbolIndex::at(size_t i) const
{
    const Entry& e = entries_[i];
    const std::string_view text(text_);
    return {text.substr(e.name, e.nameLength), text.substr(e.container, e.containerLength), text.substr(e.detail, e.detailLength),
            e.file, e.line, e.column, e.kind, e.definition};
}

std::string_view SymbolIndex::path(int file) const
{
    if (file < 0 || file >= int(pathEnds_.size()))
        return {};
    const std::uint32_t begin = file == 0 ? 0 : pathEnds_[size_t(file - 1)];
    return std::string_view(paths_).substr(begin, pathEnds_[size_t(file)] - begin);
}

int SymbolIndex::fileIndex(std::string_view path) const
{
    for (size_t i = 0; i < pathEnds_.size(); ++i)
        if (this->path(int(i)) == path)
            return int(i);
    return -1;
}

std::pair<size_t, size_t> SymbolIndex::named(std::string_view name) const
{
    const auto first = std::lower_bound(entries_.begin(), entries_.end(), name,
                                        [this](const Entry& e, std::string_view n) { return nameOf(e) < n; });
    auto last = first;
    while (last != entries_.end() && nameOf(*last) == name)
        ++last;
    return {size_t(first - entries_.begin()), size_t(last - entries_.begin())};
}

size_t SymbolIndex::bytes() const
{
    // A string's built-in room (small-string buffer) is not storage of its own.
    const size_t inlineRoom = std::string().capacity();
    auto heap = [inlineRoom](const std::string& t) { return t.capacity() > inlineRoom ? t.capacity() : size_t(0); };
    return heap(text_) + entries_.capacity() * sizeof(Entry) + heap(paths_) + pathEnds_.capacity() * sizeof(std::uint32_t);
}

} // namespace enp::model
