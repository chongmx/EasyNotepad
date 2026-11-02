#include "enp/model/Markdown.h"

#include "enp/model/Emoji.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace enp::model::markdown {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t'; }
bool isPunct(char c) { return std::ispunct(static_cast<unsigned char>(c)) != 0; }
bool isAlnum(char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || static_cast<unsigned char>(c) >= 0x80; }

std::string_view trim(std::string_view s)
{
    while (!s.empty() && isSpace(s.front()))
        s.remove_prefix(1);
    while (!s.empty() && (isSpace(s.back()) || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

int indentOf(std::string_view line)
{
    int n = 0;
    for (char c : line) {
        if (c == ' ')
            ++n;
        else if (c == '\t')
            n += 4 - n % 4;
        else
            break;
    }
    return n;
}

// ---- Entities -----------------------------------------------------------------------------------

struct Entity {
    const char* name;
    const char* utf8;
};
// The character references found in READMEs and notes: HTML 4's Latin-1 set and the common symbols.
constexpr Entity kEntities[] = {
    {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", "\xC2\xA0"}, {"ensp", "\xE2\x80\x82"},
    {"emsp", "\xE2\x80\x83"}, {"thinsp", "\xE2\x80\x89"}, {"copy", "©"}, {"reg", "®"}, {"trade", "™"}, {"deg", "°"},
    {"plusmn", "±"}, {"times", "×"}, {"divide", "÷"}, {"micro", "µ"}, {"para", "¶"}, {"sect", "§"}, {"middot", "·"},
    {"bull", "•"}, {"hellip", "…"}, {"ndash", "–"}, {"mdash", "—"}, {"lsquo", "‘"}, {"rsquo", "’"}, {"ldquo", "“"},
    {"rdquo", "”"}, {"sbquo", "‚"}, {"bdquo", "„"}, {"laquo", "«"}, {"raquo", "»"}, {"prime", "′"}, {"Prime", "″"},
    {"larr", "←"}, {"uarr", "↑"}, {"rarr", "→"}, {"darr", "↓"}, {"harr", "↔"}, {"lArr", "⇐"}, {"rArr", "⇒"}, {"hArr", "⇔"},
    {"crarr", "↵"}, {"forall", "∀"}, {"part", "∂"}, {"exist", "∃"}, {"empty", "∅"}, {"nabla", "∇"}, {"isin", "∈"},
    {"notin", "∉"}, {"ni", "∋"}, {"prod", "∏"}, {"sum", "∑"}, {"minus", "−"}, {"lowast", "∗"}, {"radic", "√"}, {"prop", "∝"},
    {"infin", "∞"}, {"ang", "∠"}, {"and", "∧"}, {"or", "∨"}, {"cap", "∩"}, {"cup", "∪"}, {"int", "∫"}, {"there4", "∴"},
    {"sim", "∼"}, {"cong", "≅"}, {"asymp", "≈"}, {"ne", "≠"}, {"equiv", "≡"}, {"le", "≤"}, {"ge", "≥"}, {"sub", "⊂"},
    {"sup", "⊃"}, {"nsub", "⊄"}, {"sube", "⊆"}, {"supe", "⊇"}, {"oplus", "⊕"}, {"otimes", "⊗"}, {"perp", "⊥"},
    {"sdot", "⋅"}, {"lceil", "⌈"}, {"rceil", "⌉"}, {"lfloor", "⌊"}, {"rfloor", "⌋"}, {"lang", "⟨"}, {"rang", "⟩"},
    {"loz", "◊"}, {"spades", "♠"}, {"clubs", "♣"}, {"hearts", "♥"}, {"diams", "♦"}, {"check", "✓"}, {"cross", "✗"},
    {"alpha", "α"}, {"beta", "β"}, {"gamma", "γ"}, {"delta", "δ"}, {"epsilon", "ε"}, {"zeta", "ζ"}, {"eta", "η"},
    {"theta", "θ"}, {"iota", "ι"}, {"kappa", "κ"}, {"lambda", "λ"}, {"mu", "μ"}, {"nu", "ν"}, {"xi", "ξ"}, {"omicron", "ο"},
    {"pi", "π"}, {"rho", "ρ"}, {"sigma", "σ"}, {"sigmaf", "ς"}, {"tau", "τ"}, {"upsilon", "υ"}, {"phi", "φ"}, {"chi", "χ"},
    {"psi", "ψ"}, {"omega", "ω"}, {"Alpha", "Α"}, {"Beta", "Β"}, {"Gamma", "Γ"}, {"Delta", "Δ"}, {"Theta", "Θ"},
    {"Lambda", "Λ"}, {"Xi", "Ξ"}, {"Pi", "Π"}, {"Sigma", "Σ"}, {"Phi", "Φ"}, {"Psi", "Ψ"}, {"Omega", "Ω"},
    {"euro", "€"}, {"pound", "£"}, {"yen", "¥"}, {"cent", "¢"}, {"curren", "¤"}, {"iexcl", "¡"}, {"iquest", "¿"},
    {"frac12", "½"}, {"frac14", "¼"}, {"frac34", "¾"}, {"sup1", "¹"}, {"sup2", "²"}, {"sup3", "³"}, {"ordf", "ª"},
    {"ordm", "º"}, {"shy", ""}, {"zwj", "\xE2\x80\x8D"}, {"zwnj", "\xE2\x80\x8C"},
    {"Agrave", "À"}, {"Aacute", "Á"}, {"Acirc", "Â"}, {"Atilde", "Ã"}, {"Auml", "Ä"}, {"Aring", "Å"}, {"AElig", "Æ"},
    {"Ccedil", "Ç"}, {"Egrave", "È"}, {"Eacute", "É"}, {"Ecirc", "Ê"}, {"Euml", "Ë"}, {"Igrave", "Ì"}, {"Iacute", "Í"},
    {"Icirc", "Î"}, {"Iuml", "Ï"}, {"ETH", "Ð"}, {"Ntilde", "Ñ"}, {"Ograve", "Ò"}, {"Oacute", "Ó"}, {"Ocirc", "Ô"},
    {"Otilde", "Õ"}, {"Ouml", "Ö"}, {"Oslash", "Ø"}, {"Ugrave", "Ù"}, {"Uacute", "Ú"}, {"Ucirc", "Û"}, {"Uuml", "Ü"},
    {"Yacute", "Ý"}, {"THORN", "Þ"}, {"szlig", "ß"}, {"agrave", "à"}, {"aacute", "á"}, {"acirc", "â"}, {"atilde", "ã"},
    {"auml", "ä"}, {"aring", "å"}, {"aelig", "æ"}, {"ccedil", "ç"}, {"egrave", "è"}, {"eacute", "é"}, {"ecirc", "ê"},
    {"euml", "ë"}, {"igrave", "ì"}, {"iacute", "í"}, {"icirc", "î"}, {"iuml", "ï"}, {"eth", "ð"}, {"ntilde", "ñ"},
    {"ograve", "ò"}, {"oacute", "ó"}, {"ocirc", "ô"}, {"otilde", "õ"}, {"ouml", "ö"}, {"oslash", "ø"}, {"ugrave", "ù"},
    {"uacute", "ú"}, {"ucirc", "û"}, {"uuml", "ü"}, {"yacute", "ý"}, {"thorn", "þ"}, {"yuml", "ÿ"}, {"OElig", "Œ"},
    {"oelig", "œ"}, {"Scaron", "Š"}, {"scaron", "š"}, {"Yuml", "Ÿ"}, {"fnof", "ƒ"}, {"circ", "ˆ"}, {"tilde", "˜"},
    {"dagger", "†"}, {"Dagger", "‡"}, {"permil", "‰"}, {"lsaquo", "‹"}, {"rsaquo", "›"}, {"oline", "‾"}, {"frasl", "⁄"},
    {"weierp", "℘"}, {"image", "ℑ"}, {"real", "ℜ"}, {"alefsym", "ℵ"}, {"not", "¬"}, {"macr", "¯"}, {"acute", "´"},
    {"cedil", "¸"}, {"uml", "¨"}, {"brvbar", "¦"},
};

void appendUtf8(std::string& out, unsigned cp)
{
    if (cp == 0 || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
        cp = 0xFFFD;
    if (cp < 0x80) {
        out += char(cp);
    } else if (cp < 0x800) {
        out += char(0xC0 | (cp >> 6));
        out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += char(0xE0 | (cp >> 12));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    } else {
        out += char(0xF0 | (cp >> 18));
        out += char(0x80 | ((cp >> 12) & 0x3F));
        out += char(0x80 | ((cp >> 6) & 0x3F));
        out += char(0x80 | (cp & 0x3F));
    }
}

// The reference starting at text[i] ('&'): appends its character and returns the index after it,
// or `i` when this is not one.
size_t entity(std::string_view text, size_t i, std::string& out)
{
    const size_t semi = text.find(';', i + 1);
    if (semi == std::string_view::npos || semi - i > 12 || semi == i + 1)
        return i;
    const std::string_view name = text.substr(i + 1, semi - i - 1);
    if (name[0] == '#') {
        unsigned cp = 0;
        const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
        const std::string_view digits = name.substr(hex ? 2 : 1);
        if (digits.empty())
            return i;
        for (char c : digits) {
            const int d = hex ? (std::isxdigit(static_cast<unsigned char>(c)) ? (std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : (std::tolower(c) - 'a' + 10)) : -1)
                              : (std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : -1);
            if (d < 0)
                return i;
            cp = cp * (hex ? 16u : 10u) + unsigned(d);
            if (cp > 0x10FFFF)
                return i;
        }
        appendUtf8(out, cp);
        return semi + 1;
    }
    for (const Entity& e : kEntities)
        if (name == e.name) {
            out += e.utf8;
            return semi + 1;
        }
    return i;
}

// An inline math span starts at text[i] ('$'): $...$ with no space just inside the dollars and
// not a price ("$5 and $6"). Returns the index after it and the source, or `i`.
size_t inlineMath(std::string_view text, size_t i, std::string& source)
{
    if (i + 2 >= text.size() || text[i + 1] == '$' || isSpace(text[i + 1]))
        return i;
    for (size_t j = i + 1; j < text.size(); ++j) {
        if (text[j] == '\\') {
            ++j;
            continue;
        }
        if (text[j] == '$') {
            if (isSpace(text[j - 1]) || (j + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[j + 1]))))
                return i;
            source = std::string(text.substr(i + 1, j - i - 1));
            return j + 1;
        }
    }
    return i;
}

// ---- Inline -------------------------------------------------------------------------------------

class InlineParser {
public:
    explicit InlineParser(std::string_view text) : text_(text) {}

    Spans run()
    {
        for (size_t i = 0; i < text_.size();) {
            const char c = text_[i];
            if (c == '\\' && i + 1 < text_.size() && text_[i + 1] == '(' && text_.find("\\)", i + 2) != std::string_view::npos) {
                const size_t end = text_.find("\\)", i + 2);
                flush();
                spans_.push_back({std::string(text_.substr(i + 2, end - i - 2)), style_ | Math, {}, {}});
                i = end + 2;
            } else if (c == '\\' && i + 1 < text_.size() && isPunct(text_[i + 1])) {
                buffer_ += text_[i + 1];
                i += 2;
            } else if (c == '`') {
                i = codeSpan(i);
            } else if (c == '!' && i + 1 < text_.size() && text_[i + 1] == '[') {
                const size_t next = link(i + 1, true);
                i = next == i + 1 ? (buffer_ += c, i + 1) : next;
            } else if (c == '[') {
                const size_t next = link(i, false);
                i = next == i ? (buffer_ += c, i + 1) : next;
            } else if (c == '<') {
                i = tag(i);
            } else if (c == '&') {
                const size_t next = entity(text_, i, buffer_);
                i = next == i ? (buffer_ += c, i + 1) : next;
            } else if (c == '$') {
                std::string source;
                const size_t next = inlineMath(text_, i, source);
                if (next == i) {
                    buffer_ += c;
                    ++i;
                } else {
                    flush();
                    spans_.push_back({source, style_ | Math, {}, {}});
                    i = next;
                }

            } else if ((c == 'h' || c == 'w') && (i == 0 || !isAlnum(text_[i - 1]))) {
                i = bareUrl(i);
            } else if (c == '*' || c == '_' || (c == '~' && i + 1 < text_.size() && text_[i + 1] == '~')) {
                i = emphasis(i);
            } else {
                buffer_ += c;
                ++i;
            }
        }
        flush();
        return std::move(spans_);
    }

private:
    std::string_view text_;
    Spans spans_;
    std::string buffer_;
    unsigned style_ = Plain;

    void flush()
    {
        if (buffer_.empty())
            return;
        if (!spans_.empty() && spans_.back().style == style_ && spans_.back().url.empty())
            spans_.back().text += buffer_;
        else
            spans_.push_back({buffer_, style_, {}, {}});
        buffer_.clear();
    }

    size_t codeSpan(size_t i)
    {
        size_t n = 0;
        while (i + n < text_.size() && text_[i + n] == '`')
            ++n;
        for (size_t j = i + n; j < text_.size();) {
            if (text_[j] != '`') {
                ++j;
                continue;
            }
            size_t m = 0;
            while (j + m < text_.size() && text_[j + m] == '`')
                ++m;
            if (m == n) {
                std::string code(text_.substr(i + n, j - i - n));
                if (code.size() >= 2 && code.front() == ' ' && code.back() == ' ')
                    code = code.substr(1, code.size() - 2);
                flush();
                spans_.push_back({code, style_ | Code, {}, {}});
                return j + m;
            }
            j += m;
        }
        buffer_.append(text_.substr(i, n)); // no closing run: literal backticks
        return i + n;
    }

    // [text](url "title"), or ![alt](src) for an image; returns `i` when it is not one.
    size_t link(size_t i, bool image)
    {
        int depth = 0;
        size_t close = std::string_view::npos;
        for (size_t j = i; j < text_.size(); ++j) {
            if (text_[j] == '\\') {
                ++j;
                continue;
            }
            if (text_[j] == '[')
                ++depth;
            else if (text_[j] == ']' && --depth == 0) {
                close = j;
                break;
            }
        }
        if (close == std::string_view::npos || close + 1 >= text_.size() || text_[close + 1] != '(')
            return i;
        const size_t end = text_.find(')', close + 2);
        if (end == std::string_view::npos)
            return i;
        std::string_view target = trim(text_.substr(close + 2, end - close - 2));
        if (const size_t space = target.find(' '); space != std::string_view::npos)
            target = target.substr(0, space); // drop a "title"
        if (target.size() >= 2 && target.front() == '<' && target.back() == '>')
            target = target.substr(1, target.size() - 2);
        flush();
        if (image) {
            spans_.push_back({plainText(InlineParser(text_.substr(i + 1, close - i - 1)).run()), style_ | Image, std::string(target), {}});
            return end + 1;
        }
        Spans inner = InlineParser(text_.substr(i + 1, close - i - 1)).run();
        if (inner.empty())
            inner.push_back({std::string(target), Plain, {}, {}});
        for (Span& span : inner) {
            span.style |= style_ | Link;
            span.url = std::string(target);
            spans_.push_back(std::move(span));
        }
        return end + 1;
    }

    // <https://...> autolinks, <img src="..." alt="..."> and <br>; other tags stay text.
    size_t tag(size_t i)
    {
        const size_t end = text_.find('>', i);
        if (end == std::string_view::npos)
            return autolink(i);
        const std::string_view inside = text_.substr(i + 1, end - i - 1);
        auto lowerStarts = [&](const char* word) {
            const size_t n = std::char_traits<char>::length(word);
            if (inside.size() < n)
                return false;
            for (size_t k = 0; k < n; ++k)
                if (std::tolower(static_cast<unsigned char>(inside[k])) != word[k])
                    return false;
            return inside.size() == n || isSpace(inside[n]) || inside[n] == '/';
        };
        if (lowerStarts("br")) {
            buffer_ += '\n';
            return end + 1;
        }
        if (lowerStarts("img")) {
            auto attribute = [&](const char* name) -> std::string {
                const std::string key = std::string(name) + "=";
                for (size_t p = inside.find(key); p != std::string_view::npos; p = inside.find(key, p + 1)) {
                    if (p > 0 && isAlnum(inside[p - 1]))
                        continue;
                    size_t v = p + key.size();
                    if (v >= inside.size())
                        return {};
                    const char quote = inside[v] == '"' || inside[v] == '\'' ? inside[v] : 0;
                    if (quote) {
                        const size_t close = inside.find(quote, v + 1);
                        return std::string(inside.substr(v + 1, close == std::string_view::npos ? std::string_view::npos : close - v - 1));
                    }
                    size_t e = v;
                    while (e < inside.size() && !isSpace(inside[e]) && inside[e] != '/' && inside[e] != '>')
                        ++e;
                    return std::string(inside.substr(v, e - v));
                }
                return {};
            };
            const std::string src = attribute("src");
            if (!src.empty()) {
                flush();
                spans_.push_back({attribute("alt"), style_ | Image, src, {}});
                return end + 1;
            }
        }
        return autolink(i);
    }

    size_t autolink(size_t i)
    {
        const size_t end = text_.find('>', i);
        if (end != std::string_view::npos) {
            const std::string_view inside = text_.substr(i + 1, end - i - 1);
            if (inside.find(' ') == std::string_view::npos &&
                (inside.rfind("http://", 0) == 0 || inside.rfind("https://", 0) == 0 || inside.rfind("mailto:", 0) == 0)) {
                flush();
                spans_.push_back({std::string(inside), style_ | Link, std::string(inside), {}});
                return end + 1;
            }
        }
        buffer_ += '<';
        return i + 1;
    }

    size_t bareUrl(size_t i)
    {
        const std::string_view rest = text_.substr(i);
        if (rest.rfind("https://", 0) != 0 && rest.rfind("http://", 0) != 0 && rest.rfind("www.", 0) != 0) {
            buffer_ += text_[i];
            return i + 1;
        }
        size_t end = i;
        while (end < text_.size() && !isSpace(text_[end]) && text_[end] != '<' && text_[end] != '`')
            ++end;
        while (end > i && std::string_view(".,;:!?)'\"*_").find(text_[end - 1]) != std::string_view::npos)
            --end; // sentence punctuation is not part of it
        const std::string url(text_.substr(i, end - i));
        flush();
        spans_.push_back({url, style_ | Link, url.rfind("www.", 0) == 0 ? "https://" + url : url, {}});
        return end;
    }

    // Does a closing run `run` follow `from` (preceded by non-space)?
    bool closes(std::string_view run, size_t from) const
    {
        for (size_t j = text_.find(run, from); j != std::string_view::npos; j = text_.find(run, j + 1)) {
            if (j > from && !isSpace(text_[j - 1]) && (run[0] != '_' || j + run.size() >= text_.size() || !isAlnum(text_[j + run.size()])))
                return true;
        }
        return false;
    }

    size_t emphasis(size_t i)
    {
        const char c = text_[i];
        size_t n = 0;
        while (i + n < text_.size() && text_[i + n] == c)
            ++n;
        if (c == '~' && n != 2) {
            buffer_.append(text_.substr(i, n));
            return i + n;
        }
        const size_t length = c == '~' ? 2 : std::min<size_t>(n, 3);
        const std::string_view run = text_.substr(i, length);
        const unsigned flag = c == '~' ? Strike : length == 3 ? (Bold | Italic) : length == 2 ? Bold : Italic;
        const bool before = i > 0 && !isSpace(text_[i - 1]);
        const bool after = i + length < text_.size() && !isSpace(text_[i + length]);
        // Underscores inside words (snake_case) are text.
        if (c == '_' && ((i > 0 && isAlnum(text_[i - 1])) && (i + length < text_.size() && isAlnum(text_[i + length])))) {
            buffer_.append(run);
            return i + length;
        }
        if ((style_ & flag) == flag && before) { // closing
            flush();
            style_ &= ~flag;
            return i + length;
        }
        if ((style_ & flag) == 0 && after && closes(run, i + length)) { // opening
            flush();
            style_ |= flag;
            return i + length;
        }
        buffer_.append(run);
        return i + length;
    }
};

// ---- Blocks -------------------------------------------------------------------------------------

struct ListMarker {
    bool ordered = false;
    std::string marker;
    int indent = 0;   // of the marker
    size_t content = 0; // where the item's text starts in the line
};

bool listMarker(std::string_view line, ListMarker* out)
{
    const int indent = indentOf(line);
    size_t i = 0;
    while (i < line.size() && isSpace(line[i]))
        ++i;
    if (i >= line.size())
        return false;
    ListMarker m;
    m.indent = indent;
    if (line[i] == '-' || line[i] == '*' || line[i] == '+') {
        m.marker = std::string(1, line[i]);
        ++i;
    } else if (std::isdigit(static_cast<unsigned char>(line[i]))) {
        size_t j = i;
        while (j < line.size() && j - i < 9 && std::isdigit(static_cast<unsigned char>(line[j])))
            ++j;
        if (j >= line.size() || (line[j] != '.' && line[j] != ')'))
            return false;
        m.ordered = true;
        m.marker = std::string(line.substr(i, j - i)) + ".";
        i = j + 1;
    } else {
        return false;
    }
    if (i < line.size() && !isSpace(line[i]))
        return false;
    while (i < line.size() && isSpace(line[i]))
        ++i;
    m.content = i;
    if (out)
        *out = m;
    return true;
}

bool isRule(std::string_view t)
{
    if (t.size() < 3)
        return false;
    const char c = t[0];
    if (c != '-' && c != '*' && c != '_')
        return false;
    int count = 0;
    for (char ch : t) {
        if (ch == c)
            ++count;
        else if (!isSpace(ch))
            return false;
    }
    return count >= 3;
}

int headingLevel(std::string_view t)
{
    int n = 0;
    while (n < int(t.size()) && t[size_t(n)] == '#')
        ++n;
    if (n < 1 || n > 6 || (n < int(t.size()) && !isSpace(t[size_t(n)])))
        return 0;
    return n;
}

bool isFence(std::string_view t, char* fenceChar, size_t* fenceLength)
{
    if (t.size() < 3 || (t[0] != '`' && t[0] != '~'))
        return false;
    size_t n = 0;
    while (n < t.size() && t[n] == t[0])
        ++n;
    if (n < 3)
        return false;
    if (t[0] == '`' && t.substr(n).find('`') != std::string_view::npos)
        return false;
    *fenceChar = t[0];
    *fenceLength = n;
    return true;
}

// The cells of a table row: split at '|' outside code spans.
std::vector<std::string> cells(std::string_view line)
{
    std::string_view t = trim(line);
    if (!t.empty() && t.front() == '|')
        t.remove_prefix(1);
    if (!t.empty() && t.back() == '|' && (t.size() < 2 || t[t.size() - 2] != '\\'))
        t.remove_suffix(1);
    std::vector<std::string> out;
    std::string cell;
    bool code = false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '\\' && i + 1 < t.size() && t[i + 1] == '|') {
            cell += '|';
            ++i;
        } else if (t[i] == '`') {
            code = !code;
            cell += '`';
        } else if (t[i] == '|' && !code) {
            out.emplace_back(trim(cell));
            cell.clear();
        } else {
            cell += t[i];
        }
    }
    out.emplace_back(trim(cell));
    return out;
}

bool isTableSeparator(std::string_view line, std::vector<int>* align)
{
    const std::string_view t = trim(line);
    if (t.find('-') == std::string_view::npos || t.find('|') == std::string_view::npos)
        return false;
    std::vector<int> result;
    for (const std::string& cell : cells(t)) {
        if (cell.empty())
            return false;
        const bool left = cell.front() == ':';
        const bool right = cell.back() == ':';
        for (char c : cell)
            if (c != '-' && c != ':')
                return false;
        result.push_back(left && right ? 0 : right ? 1 : -1);
    }
    if (align)
        *align = result;
    return true;
}

class BlockParser {
public:
    explicit BlockParser(std::vector<std::string_view> lines, int firstLine, int quote)
        : lines_(std::move(lines)), firstLine_(firstLine), quote_(quote)
    {
    }

    std::vector<Block> run()
    {
        while (i_ < lines_.size()) {
            const std::string_view line = lines_[i_];
            const std::string_view t = trim(line);
            if (t.empty()) {
                ++i_;
                listIndents_.clear();
                continue;
            }
            char fenceChar = 0;
            size_t fenceLength = 0;
            if (indentOf(line) < 4 && isFence(t, &fenceChar, &fenceLength))
                code(fenceChar, fenceLength, indentOf(line));
            else if (isDisplayMathOpener(t))
                displayMath();
            else if (t[0] == '>')
                quote();
            else if (const int level = headingLevel(t); level > 0 && indentOf(line) < 4)
                heading(level, t);
            else if (isRule(t) && indentOf(line) < 4)
                add(BlockKind::Rule, i_++);
            else if (ListMarker m; listMarker(line, &m))
                item(m);
            else if (i_ + 1 < lines_.size() && line.find('|') != std::string_view::npos && isTableSeparator(lines_[i_ + 1], nullptr))
                table();
            else
                paragraph();
            if (!out_.empty() && out_.back().kind != BlockKind::Bullet && out_.back().kind != BlockKind::Numbered &&
                out_.back().kind != BlockKind::Task)
                listIndents_.clear();
        }
        return std::move(out_);
    }

private:
    std::vector<std::string_view> lines_;
    int firstLine_;
    int quote_;
    size_t i_ = 0;
    std::vector<Block> out_;
    std::vector<int> listIndents_; // marker indents of the open list levels

    Block& add(BlockKind kind, size_t line)
    {
        Block b;
        b.kind = kind;
        b.quote = quote_;
        b.line = firstLine_ + int(line);
        out_.push_back(std::move(b));
        return out_.back();
    }

    // Does this line start a block other than a paragraph?
    bool startsBlock(size_t index) const
    {
        const std::string_view line = lines_[index];
        const std::string_view t = trim(line);
        char c = 0;
        size_t n = 0;
        return t.empty() || t[0] == '>' || headingLevel(t) > 0 || isRule(t) || isFence(t, &c, &n) || listMarker(line, nullptr) ||
               isDisplayMathOpener(t) ||
               (index + 1 < lines_.size() && line.find('|') != std::string_view::npos && isTableSeparator(lines_[index + 1], nullptr));
    }

    // "$$" or "\\[" at the start of a line opens display math ($$ x $$ on one line included).
    static bool isDisplayMathOpener(std::string_view t) { return t.substr(0, 2) == "$$" || t.substr(0, 2) == "\\["; }

    void displayMath()
    {
        Block& b = add(BlockKind::Math, i_);
        std::string_view t = trim(lines_[i_]);
        const bool dollars = t[0] == '$';
        const std::string_view closer = dollars ? "$$" : "\\]";
        t.remove_prefix(2);
        ++i_;
        if (const size_t close = t.find(closer); close != std::string_view::npos) { // one line
            b.code = std::string(trim(t.substr(0, close)));
            return;
        }
        b.code = std::string(trim(t));
        while (i_ < lines_.size()) {
            const std::string_view line = trim(lines_[i_++]);
            const size_t close = line.find(closer);
            const std::string_view part = close == std::string_view::npos ? line : trim(line.substr(0, close));
            if (!part.empty()) {
                if (!b.code.empty())
                    b.code += '\n';
                b.code.append(part);
            }
            if (close != std::string_view::npos)
                break;
        }
    }

    // Joins lines into inline text: a line ending in two spaces or a backslash breaks the line.
    static void join(std::string& text, std::string_view line)
    {
        if (!text.empty()) {
            if (text.size() >= 2 && text.compare(text.size() - 2, 2, "  ") == 0) {
                while (!text.empty() && text.back() == ' ')
                    text.pop_back();
                text += '\n';
            } else if (text.back() == '\\') {
                text.back() = '\n';
            } else {
                while (!text.empty() && text.back() == ' ')
                    text.pop_back();
                text += ' ';
            }
        }
        std::string_view l = line;
        while (!l.empty() && isSpace(l.front()))
            l.remove_prefix(1);
        while (!l.empty() && l.back() == '\r')
            l.remove_suffix(1);
        text.append(l);
    }

    void code(char fenceChar, size_t fenceLength, int indent)
    {
        Block& b = add(BlockKind::Code, i_);
        std::string_view info = trim(trim(lines_[i_]).substr(fenceLength));
        b.language = std::string(info.substr(0, info.find_first_of(" \t{")));
        ++i_;
        bool first = true;
        while (i_ < lines_.size()) { // an unclosed fence runs to the end (an answer still streaming)
            const std::string_view line = lines_[i_];
            const std::string_view t = trim(line);
            if (t.size() >= fenceLength && t.find_first_not_of(fenceChar) == std::string_view::npos) {
                ++i_;
                break;
            }
            std::string_view content = line;
            for (int k = 0; k < indent && !content.empty() && content.front() == ' '; ++k)
                content.remove_prefix(1);
            while (!content.empty() && content.back() == '\r')
                content.remove_suffix(1);
            if (!first)
                b.code += '\n';
            b.code.append(content);
            first = false;
            ++i_;
        }
    }

    void quote()
    {
        const size_t start = i_;
        std::vector<std::string_view> inner;
        while (i_ < lines_.size()) {
            std::string_view t = trim(lines_[i_]);
            if (t.empty() || (t[0] != '>' && (startsBlock(i_) || inner.empty())))
                break;
            if (t[0] == '>') {
                t.remove_prefix(1);
                if (!t.empty() && t.front() == ' ')
                    t.remove_prefix(1);
            }
            inner.push_back(t);
            ++i_;
        }
        for (Block& b : BlockParser(std::move(inner), firstLine_ + int(start), quote_ + 1).run())
            out_.push_back(std::move(b));
    }

    void heading(int level, std::string_view t)
    {
        std::string_view text = trim(t.substr(size_t(level)));
        // Closing #s ("## Title ##") are not part of it.
        const size_t hashes = text.find_last_not_of('#');
        if (hashes != std::string_view::npos && hashes + 1 < text.size() && isSpace(text[hashes]))
            text = trim(text.substr(0, hashes));
        else if (hashes == std::string_view::npos)
            text = {};
        Block& b = add(BlockKind::Heading, i_++);
        b.level = level;
        b.spans = parseInline(text);
    }

    void item(const ListMarker& m)
    {
        while (!listIndents_.empty() && listIndents_.back() > m.indent)
            listIndents_.pop_back();
        if (listIndents_.empty() || listIndents_.back() < m.indent)
            listIndents_.push_back(m.indent);
        std::string text(lines_[i_].substr(m.content));
        BlockKind kind = m.ordered ? BlockKind::Numbered : BlockKind::Bullet;
        bool checked = false;
        if (!m.ordered && text.size() >= 3 && text[0] == '[' && text[2] == ']' && (text.size() == 3 || text[3] == ' ') &&
            (text[1] == ' ' || text[1] == 'x' || text[1] == 'X')) {
            kind = BlockKind::Task;
            checked = text[1] != ' ';
            text = text.size() > 4 ? text.substr(4) : std::string();
        }
        const size_t start = i_++;
        // Continuation lines: indented or lazy text that does not start a block.
        while (i_ < lines_.size() && !startsBlock(i_))
            join(text, lines_[i_++]);
        Block& b = add(kind, start);
        b.level = int(listIndents_.size()) - 1;
        b.marker = m.ordered ? m.marker : std::string();
        b.checked = checked;
        b.spans = parseInline(text);
    }

    void table()
    {
        Block& b = add(BlockKind::Table, i_);
        isTableSeparator(lines_[i_ + 1], &b.align);
        std::vector<std::string> header = cells(lines_[i_]);
        auto addRow = [&b](const std::vector<std::string>& texts) {
            std::vector<Spans> row;
            for (size_t c = 0; c < std::max(texts.size(), b.align.size()); ++c)
                row.push_back(c < texts.size() ? parseInline(texts[c]) : Spans());
            b.rows.push_back(std::move(row));
        };
        addRow(header);
        i_ += 2;
        while (i_ < lines_.size() && !trim(lines_[i_]).empty() && lines_[i_].find('|') != std::string_view::npos) {
            addRow(cells(lines_[i_]));
            ++i_;
        }
        const size_t columns = b.align.size();
        for (std::vector<Spans>& row : b.rows)
            row.resize(std::max(columns, row.size()));
        b.align.resize(b.rows.empty() ? columns : b.rows[0].size(), -1);
    }

    void paragraph()
    {
        const size_t start = i_;
        std::string text;
        join(text, lines_[i_++]);
        while (i_ < lines_.size()) {
            const std::string_view t = trim(lines_[i_]);
            // Setext heading: the paragraph underlined with === or ---.
            if (!t.empty() && t.find_first_not_of('=') == std::string_view::npos) {
                Block& b = add(BlockKind::Heading, start);
                b.level = 1;
                b.spans = parseInline(text);
                ++i_;
                return;
            }
            if (!t.empty() && t.size() >= 2 && t.find_first_not_of('-') == std::string_view::npos) {
                Block& b = add(BlockKind::Heading, start);
                b.level = 2;
                b.spans = parseInline(text);
                ++i_;
                return;
            }
            if (startsBlock(i_))
                break;
            join(text, lines_[i_++]);
        }
        add(BlockKind::Paragraph, start).spans = parseInline(text);
    }
};

} // namespace

namespace {

// Emoji sequences become spans of their own (whatever wrote them: the characters, &#x2B07;&#xFE0F;,
// ...), and code points that never show alone (variation selectors, stray joiners) are dropped so
// they cannot be drawn as a box. Math and image spans are left as they are.
Spans splitEmoji(Spans in)
{
    Spans out;
    out.reserve(in.size());
    for (Span& span : in) {
        if (span.style & (Math | Image)) {
            out.push_back(std::move(span));
            continue;
        }
        const std::string& t = span.text;
        std::string plain;
        bool split = false;
        auto flushPlain = [&]() {
            if (!plain.empty()) {
                out.push_back({std::move(plain), span.style, span.url, {}});
                plain.clear();
            }
        };
        for (size_t i = 0; i < t.size();) {
            const unsigned char c = static_cast<unsigned char>(t[i]);
            // Only bytes that can start an emoji are looked at: non-ASCII, and [0-9#*] for keycaps.
            if (c >= 0x80 || (c >= '0' && c <= '9') || c == '#' || c == '*') {
                std::string key;
                if (const size_t n = emoji::match(t, i, &key)) {
                    flushPlain();
                    out.push_back({t.substr(i, n), span.style | Emoji, span.url, std::move(key)});
                    i += n;
                    split = true;
                    continue;
                }
                if (const size_t n = emoji::invisible(t, i)) {
                    i += n;
                    split = true;
                    continue;
                }
            }
            plain += t[i++];
        }
        if (!split) {
            out.push_back(std::move(span));
            continue;
        }
        flushPlain();
    }
    return out;
}

} // namespace

Spans parseInline(std::string_view text)
{
    return splitEmoji(InlineParser(text).run());
}

std::vector<Block> parse(std::string_view text)
{
    std::vector<std::string_view> lines;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t end = text.find('\n', start);
        std::string_view line = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        lines.push_back(line);
        if (end == std::string_view::npos)
            break;
        start = end + 1;
    }
    return BlockParser(std::move(lines), 0, 0).run();
}

std::string plainText(const Spans& spans)
{
    std::string text;
    for (const Span& span : spans)
        text += span.text;
    return text;
}

std::string decodeEntities(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text[i] == '&') {
            const size_t next = entity(text, i, out);
            if (next != i) {
                i = next;
                continue;
            }
        }
        out += text[i++];
    }
    return out;
}

} // namespace enp::model::markdown
