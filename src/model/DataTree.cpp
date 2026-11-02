#include "enp/model/DataTree.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>

namespace enp::model::data {

namespace {

// ---- Building --------------------------------------------------------------------------------------

class Builder {
public:
    explicit Builder(Tree& t) : t_(t) {}
    std::uint32_t add(std::uint32_t parent, Kind kind, std::string_view key, std::string_view value, int line)
    {
        Node n;
        n.kind = kind;
        n.parent = parent;
        n.line = line;
        n.key = std::uint32_t(t_.strings.size());
        n.keyLength = std::uint32_t(key.size());
        t_.strings.append(key);
        n.value = std::uint32_t(t_.strings.size());
        n.valueLength = std::uint32_t(value.size());
        t_.strings.append(value);
        const std::uint32_t index = std::uint32_t(t_.nodes.size());
        t_.nodes.push_back(n);
        if (parent != kNone) {
            Node& p = t_.nodes[parent];
            if (p.lastChild == kNone)
                p.firstChild = index;
            else
                t_.nodes[p.lastChild].nextSibling = index;
            p.lastChild = index;
            ++p.childCount;
        }
        return index;
    }
    void setValue(std::uint32_t index, Kind kind, std::string_view value)
    {
        Node& n = t_.nodes[index];
        n.kind = kind;
        n.value = std::uint32_t(t_.strings.size());
        n.valueLength = std::uint32_t(value.size());
        t_.strings.append(value);
    }
    Tree& tree() { return t_; }
    const Tree& tree() const { return t_; }

private:
    Tree& t_;
};

void appendUtf8(std::string& out, std::uint32_t cp)
{
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

void finish(Tree& t)
{
    if (!t.error.empty()) {
        t.nodes.clear();
        t.strings.clear();
    }
    t.nodes.shrink_to_fit();
    t.strings.shrink_to_fit();
}

// ---- JSON --------------------------------------------------------------------------------------

class JsonParser {
public:
    JsonParser(std::string_view s, Tree& t) : s_(s), b_(t) {}

    void run()
    {
        skip();
        if (i_ >= s_.size()) {
            fail("the file is empty");
            return;
        }
        value(kNone, {});
        skip();
        if (ok() && i_ < s_.size())
            fail("unexpected text after the value");
    }

private:
    std::string_view s_;
    Builder b_;
    size_t i_ = 0;
    int line_ = 0;
    int depth_ = 0;
    std::string scratch_;

    bool ok() const { return b_.tree().error.empty(); }
    void fail(const char* what)
    {
        if (ok()) {
            b_.tree().error = what;
            b_.tree().errorLine = line_;
        }
    }
    void skip()
    {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\n')
                ++line_;
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++i_;
            else
                break;
        }
    }
    bool string(std::string& out)
    {
        out.clear();
        ++i_; // the opening quote
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"')
                return true;
            if (c == '\n') {
                fail("a string runs to the end of the line");
                return false;
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i_ >= s_.size())
                break;
            const char e = s_[i_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                auto hex4 = [&](std::uint32_t& v) {
                    if (i_ + 4 > s_.size())
                        return false;
                    v = 0;
                    for (int k = 0; k < 4; ++k) {
                        const char h = s_[i_ + size_t(k)];
                        v = v * 16 + std::uint32_t(std::isdigit(static_cast<unsigned char>(h)) ? h - '0'
                                                   : std::isxdigit(static_cast<unsigned char>(h)) ? (std::tolower(h) - 'a' + 10) : 0);
                    }
                    i_ += 4;
                    return true;
                };
                std::uint32_t cp = 0;
                if (!hex4(cp)) {
                    fail("a \\u escape is cut short");
                    return false;
                }
                if (cp >= 0xD800 && cp <= 0xDBFF && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                    i_ += 2;
                    std::uint32_t low = 0;
                    if (hex4(low) && low >= 0xDC00 && low <= 0xDFFF)
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, cp);
                break;
            }
            default: out += e; break; // \" \\ \/
            }
        }
        fail("a string is not closed");
        return false;
    }
    void value(std::uint32_t parent, std::string_view key)
    {
        if (!ok())
            return;
        if (++depth_ > 512) {
            fail("nested too deeply");
            return;
        }
        skip();
        if (i_ >= s_.size()) {
            fail("a value is missing at the end");
            --depth_;
            return;
        }
        const int line = line_;
        const char c = s_[i_];
        if (c == '{' || c == '[') {
            const bool object = c == '{';
            const std::uint32_t self = b_.add(parent, object ? Kind::Object : Kind::Array, key, {}, line);
            ++i_;
            skip();
            if (i_ < s_.size() && s_[i_] == (object ? '}' : ']')) {
                ++i_;
                --depth_;
                return;
            }
            std::string childKey;
            while (ok()) {
                skip();
                if (object) {
                    if (i_ >= s_.size() || s_[i_] != '"') {
                        fail("expected a \"key\"");
                        break;
                    }
                    if (!string(childKey))
                        break;
                    skip();
                    if (i_ >= s_.size() || s_[i_] != ':') {
                        fail("expected ':' after the key");
                        break;
                    }
                    ++i_;
                    value(self, childKey);
                } else {
                    value(self, {});
                }
                skip();
                if (i_ < s_.size() && s_[i_] == ',') {
                    ++i_;
                    continue;
                }
                if (i_ < s_.size() && s_[i_] == (object ? '}' : ']')) {
                    ++i_;
                    break;
                }
                fail(object ? "expected ',' or '}'" : "expected ',' or ']'");
            }
        } else if (c == '"') {
            if (string(scratch_))
                b_.add(parent, Kind::String, key, scratch_, line);
        } else {
            size_t end = i_;
            while (end < s_.size() && std::strchr(",]} \t\r\n", s_[end]) == nullptr)
                ++end;
            const std::string_view word = s_.substr(i_, end - i_);
            Kind kind = Kind::Number;
            if (word == "true" || word == "false")
                kind = Kind::Bool;
            else if (word == "null")
                kind = Kind::Null;
            else if (word.empty() || !(std::isdigit(static_cast<unsigned char>(word[0])) || word[0] == '-')) {
                fail("expected a value");
                --depth_;
                return;
            }
            b_.add(parent, kind, key, word, line);
            i_ = end;
        }
        --depth_;
    }
};

// ---- YAML --------------------------------------------------------------------------------------

struct YLine {
    int indent;
    std::string_view text; // without the indent and the comment
    int number;
};

class YamlParser {
public:
    YamlParser(std::string_view s, Tree& t) : b_(t) { split(s); }

    void run()
    {
        if (docs_.size() <= 1) {
            lines_ = docs_.empty() ? std::vector<YLine>() : docs_.front();
            const std::uint32_t root = b_.add(kNone, Kind::Null, {}, {}, lines_.empty() ? 0 : lines_.front().number);
            if (!lines_.empty())
                node(root, lines_.front().indent);
        } else {
            const std::uint32_t root = b_.add(kNone, Kind::Array, {}, {}, 0);
            for (std::vector<YLine>& doc : docs_) {
                lines_ = std::move(doc);
                i_ = 0;
                const std::uint32_t item = b_.add(root, Kind::Null, {}, {}, lines_.empty() ? 0 : lines_.front().number);
                if (!lines_.empty())
                    node(item, lines_.front().indent);
                if (!b_.tree().error.empty())
                    break;
            }
        }
        if (b_.tree().error.empty() && i_ < lines_.size())
            fail("unexpected indentation", lines_[i_].number);
    }

private:
    Builder b_;
    std::vector<std::vector<YLine>> docs_;
    std::vector<YLine> lines_;
    std::vector<std::string_view> raw_; // every source line, for block scalars (which keep comments' '#')
    size_t i_ = 0;
    std::string scratch_;

    void fail(const char* what, int line)
    {
        if (b_.tree().error.empty()) {
            b_.tree().error = what;
            b_.tree().errorLine = line;
        }
    }

    static std::string_view stripComment(std::string_view t)
    {
        char quote = 0;
        for (size_t k = 0; k < t.size(); ++k) {
            const char c = t[k];
            if (quote) {
                if (c == quote)
                    quote = 0;
                else if (c == '\\' && quote == '"')
                    ++k;
            } else if (c == '"' || c == '\'') {
                if (k == 0 || t[k - 1] == ' ' || t[k - 1] == '[' || t[k - 1] == '{' || t[k - 1] == ',' || t[k - 1] == ':' || t[k - 1] == '-')
                    quote = c;
            } else if (c == '#' && (k == 0 || t[k - 1] == ' ' || t[k - 1] == '\t')) {
                t = t.substr(0, k);
                break;
            }
        }
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r'))
            t.remove_suffix(1);
        return t;
    }

    void split(std::string_view s)
    {
        docs_.emplace_back();
        size_t start = 0;
        int number = 0;
        bool sawContent = false;
        while (start <= s.size()) {
            size_t end = s.find('\n', start);
            if (end == std::string_view::npos)
                end = s.size();
            std::string_view line = s.substr(start, end - start);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            raw_.push_back(line);
            int indent = 0;
            while (size_t(indent) < line.size() && line[size_t(indent)] == ' ')
                ++indent;
            const std::string_view text = stripComment(line.substr(size_t(indent)));
            if (line.rfind("---", 0) == 0 && (line.size() == 3 || line[3] == ' ')) {
                if (sawContent || !docs_.back().empty())
                    docs_.emplace_back();
                const std::string_view rest = stripComment(line.substr(3));
                std::string_view trimmed = rest;
                while (!trimmed.empty() && trimmed.front() == ' ')
                    trimmed.remove_prefix(1);
                if (!trimmed.empty())
                    docs_.back().push_back({4, trimmed, number});
                sawContent = true;
            } else if (line == "...") {
                // end of a document
            } else if (!text.empty() && text[0] != '%') {
                docs_.back().push_back({indent, text, number});
                sawContent = true;
            }
            ++number;
            if (end == s.size())
                break;
            start = end + 1;
        }
        docs_.erase(std::remove_if(docs_.begin(), docs_.end(), [](const std::vector<YLine>& d) { return d.empty(); }), docs_.end());
    }

    // Where the ':' of "key: value" is (outside quotes and flow brackets), or npos.
    static size_t colon(std::string_view t)
    {
        char quote = 0;
        int depth = 0;
        for (size_t k = 0; k < t.size(); ++k) {
            const char c = t[k];
            if (quote) {
                if (c == quote)
                    quote = 0;
                continue;
            }
            if ((c == '"' || c == '\'') && (k == 0))
                quote = c;
            else if (c == '[' || c == '{')
                ++depth;
            else if (c == ']' || c == '}')
                --depth;
            else if (c == ':' && depth == 0 && (k + 1 == t.size() || t[k + 1] == ' ' || t[k + 1] == '\t'))
                return k;
        }
        return std::string_view::npos;
    }
    static bool isItem(std::string_view t) { return t == "-" || t.rfind("- ", 0) == 0; }

    static std::string_view trim(std::string_view t)
    {
        while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
            t.remove_prefix(1);
        while (!t.empty() && (t.back() == ' ' || t.back() == '\t'))
            t.remove_suffix(1);
        return t;
    }

    // Drops a tag ("!!str x") and an anchor ("&a x") in front of a value.
    static std::string_view properties(std::string_view t)
    {
        for (;;) {
            t = trim(t);
            if (!t.empty() && (t[0] == '!' || t[0] == '&')) {
                const size_t sp = t.find(' ');
                t = sp == std::string_view::npos ? std::string_view() : t.substr(sp + 1);
            } else {
                return t;
            }
        }
    }

    // A scalar as written: quotes removed (escapes in "..."), its kind guessed as YAML does.
    void scalar(std::uint32_t self, std::string_view t)
    {
        t = properties(t);
        if (t.size() >= 2 && t.front() == '"' && t.back() == '"') {
            scratch_.clear();
            for (size_t k = 1; k + 1 < t.size(); ++k) {
                if (t[k] == '\\' && k + 2 < t.size()) {
                    const char e = t[++k];
                    scratch_ += e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e;
                } else {
                    scratch_ += t[k];
                }
            }
            b_.setValue(self, Kind::String, scratch_);
            return;
        }
        if (t.size() >= 2 && t.front() == '\'' && t.back() == '\'') {
            scratch_.clear();
            for (size_t k = 1; k + 1 < t.size(); ++k) {
                scratch_ += t[k];
                if (t[k] == '\'' && k + 2 < t.size() && t[k + 1] == '\'')
                    ++k;
            }
            b_.setValue(self, Kind::String, scratch_);
            return;
        }
        if (t.empty() || t == "~" || t == "null" || t == "Null" || t == "NULL") {
            b_.setValue(self, Kind::Null, t.empty() ? std::string_view("null") : t);
            return;
        }
        if (t == "true" || t == "false" || t == "True" || t == "False" || t == "TRUE" || t == "FALSE") {
            b_.setValue(self, Kind::Bool, t);
            return;
        }
        bool number = !t.empty();
        size_t k = (t[0] == '-' || t[0] == '+') ? 1 : 0;
        if (k >= t.size())
            number = false;
        bool digits = false;
        for (; k < t.size() && number; ++k) {
            const char c = t[k];
            if (std::isdigit(static_cast<unsigned char>(c)))
                digits = true;
            else if (!(c == '.' || c == 'e' || c == 'E' || c == '_' || c == '+' || c == '-' || c == 'x' || c == 'o' ||
                       std::isxdigit(static_cast<unsigned char>(c))))
                number = false;
        }
        b_.setValue(self, number && digits ? Kind::Number : Kind::String, t);
    }

    // A flow collection [..] or {..} starting at t[k]; returns the index after it.
    size_t flow(std::uint32_t self, std::string_view t, size_t k, int line, int depth)
    {
        if (depth > 256) {
            fail("nested too deeply", line);
            return t.size();
        }
        const bool object = t[k] == '{';
        b_.setValue(self, object ? Kind::Object : Kind::Array, {});
        const char close = object ? '}' : ']';
        ++k;
        for (;;) {
            while (k < t.size() && (t[k] == ' ' || t[k] == ','))
                ++k;
            if (k >= t.size()) {
                fail(object ? "a '{' is not closed" : "a '[' is not closed", line);
                return k;
            }
            if (t[k] == close)
                return k + 1;
            std::string_view key;
            if (object) { // key: value (or a key alone)
                size_t e = k;
                char quote = (t[k] == '"' || t[k] == '\'') ? t[k] : 0;
                if (quote) {
                    e = t.find(quote, k + 1);
                    if (e == std::string_view::npos) {
                        fail("a quote is not closed", line);
                        return t.size();
                    }
                    key = t.substr(k + 1, e - k - 1);
                    e += 1;
                } else {
                    while (e < t.size() && t[e] != ':' && t[e] != ',' && t[e] != close)
                        ++e;
                    key = trim(t.substr(k, e - k));
                }
                k = e;
                while (k < t.size() && t[k] == ' ')
                    ++k;
                if (k < t.size() && t[k] == ':')
                    ++k;
                while (k < t.size() && t[k] == ' ')
                    ++k;
            }
            const std::uint32_t child = b_.add(self, Kind::Null, key, {}, line);
            if (k < t.size() && (t[k] == '[' || t[k] == '{')) {
                k = flow(child, t, k, line, depth + 1);
                continue;
            }
            size_t e = k;
            const char quote = (k < t.size() && (t[k] == '"' || t[k] == '\'')) ? t[k] : 0;
            if (quote) {
                e = t.find(quote, k + 1);
                e = e == std::string_view::npos ? t.size() : e + 1;
            } else {
                while (e < t.size() && t[e] != ',' && t[e] != close)
                    ++e;
            }
            scalar(child, trim(t.substr(k, e - k)));
            k = e;
        }
    }

    // A value that starts on this line after "key:" or "- ": flow, block scalar, or plain/quoted
    // scalar (continued on more-indented lines).
    void inlineValue(std::uint32_t self, std::string_view rest, int parentIndent, int line)
    {
        rest = properties(rest);
        if (!rest.empty() && (rest[0] == '[' || rest[0] == '{')) {
            // A flow collection may span lines: join until the brackets balance.
            std::string joined(rest);
            int depth = 0;
            auto balance = [](std::string_view s) {
                int d = 0;
                char q = 0;
                for (char c : s) {
                    if (q) {
                        if (c == q)
                            q = 0;
                    } else if (c == '"' || c == '\'')
                        q = c;
                    else if (c == '[' || c == '{')
                        ++d;
                    else if (c == ']' || c == '}')
                        --d;
                }
                return d;
            };
            depth = balance(joined);
            while (depth > 0 && i_ < lines_.size()) {
                joined += ' ';
                joined += lines_[i_].text;
                depth = balance(joined);
                ++i_;
            }
            flow(self, joined, 0, line, 0);
            return;
        }
        if (!rest.empty() && (rest[0] == '|' || rest[0] == '>')) {
            blockScalar(self, rest[0] == '>', parentIndent, line);
            return;
        }
        // Plain or quoted, possibly continued on following, more indented lines.
        if (i_ < lines_.size() && lines_[i_].indent > parentIndent && !(rest.size() > 1 && (rest[0] == '"' || rest[0] == '\'') &&
                                                                          rest.back() == rest[0])) {
            std::string joined(rest);
            while (i_ < lines_.size() && lines_[i_].indent > parentIndent && colon(lines_[i_].text) == std::string_view::npos &&
                   !isItem(lines_[i_].text)) {
                joined += ' ';
                joined += lines_[i_].text;
                ++i_;
            }
            scalar(self, joined);
            return;
        }
        scalar(self, rest);
    }

    // | or > : the lines indented further than the key, as text (newlines kept, or folded).
    void blockScalar(std::uint32_t self, bool folded, int parentIndent, int line)
    {
        // Work on the raw source lines: comments and blank lines are part of the text.
        int r = line + 1;
        int indent = -1;
        std::string text;
        int lastUsed = line;
        for (; r < int(raw_.size()); ++r) {
            const std::string_view l = raw_[size_t(r)];
            int ind = 0;
            while (size_t(ind) < l.size() && l[size_t(ind)] == ' ')
                ++ind;
            const bool blank = size_t(ind) == l.size();
            if (!blank && ind <= parentIndent)
                break;
            if (!blank && indent < 0)
                indent = ind;
            if (blank) {
                text += '\n'; // literal: an empty line; folded: a paragraph break
                continue;
            }
            if (folded && !text.empty() && text.back() != '\n')
                text += ' '; // folded: lines of a paragraph are joined
            text.append(l.substr(size_t(std::min(ind, indent))));
            if (!folded)
                text += '\n';
            lastUsed = r;
        }
        while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
            text.pop_back();
        b_.setValue(self, Kind::String, text);
        while (i_ < lines_.size() && lines_[i_].number <= lastUsed)
            ++i_; // the block's lines are consumed
    }

    // The node whose lines start at lines_[i_] (indent `indent`).
    void node(std::uint32_t self, int indent)
    {
        if (i_ >= lines_.size())
            return;
        const YLine& l = lines_[i_];
        if (isItem(l.text))
            sequence(self, l.indent);
        else if (colon(l.text) != std::string_view::npos)
            mapping(self, l.indent);
        else {
            ++i_;
            inlineValue(self, l.text, indent - 1, l.number);
        }
    }

    void sequence(std::uint32_t self, int indent)
    {
        b_.setValue(self, Kind::Array, {});
        while (i_ < lines_.size() && lines_[i_].indent == indent && isItem(lines_[i_].text) && b_.tree().error.empty()) {
            YLine& l = lines_[i_];
            const std::uint32_t item = b_.add(self, Kind::Null, {}, {}, l.number);
            const std::string_view rest = trim(l.text.substr(1));
            if (rest.empty()) {
                ++i_;
                if (i_ < lines_.size() && lines_[i_].indent > indent)
                    node(item, lines_[i_].indent);
                else
                    b_.setValue(item, Kind::Null, "null");
                continue;
            }
            // "- key: value" or "- - x": the item's content starts in this line, at its column.
            const int column = indent + int(l.text.size() - rest.size());
            if (isItem(rest) || (colon(rest) != std::string_view::npos && rest[0] != '[' && rest[0] != '{' && rest[0] != '"' && rest[0] != '\'')) {
                l.indent = column;
                l.text = rest;
                node(item, column);
            } else {
                ++i_;
                inlineValue(item, rest, indent, l.number);
            }
        }
    }

    void mapping(std::uint32_t self, int indent)
    {
        b_.setValue(self, Kind::Object, {});
        while (i_ < lines_.size() && lines_[i_].indent == indent && !isItem(lines_[i_].text) && b_.tree().error.empty()) {
            const YLine l = lines_[i_];
            const size_t c = colon(l.text);
            if (c == std::string_view::npos) {
                fail("expected 'key: value'", l.number);
                return;
            }
            std::string_view key = trim(l.text.substr(0, c));
            if (key.size() >= 2 && (key.front() == '"' || key.front() == '\'') && key.back() == key.front())
                key = key.substr(1, key.size() - 2);
            const std::string_view rest = trim(l.text.substr(c + 1));
            const std::uint32_t child = b_.add(self, Kind::Null, key, {}, l.number);
            ++i_;
            if (!rest.empty()) {
                inlineValue(child, rest, indent, l.number);
                continue;
            }
            if (i_ < lines_.size() && lines_[i_].indent > indent)
                node(child, lines_[i_].indent);
            else if (i_ < lines_.size() && lines_[i_].indent == indent && isItem(lines_[i_].text))
                sequence(child, indent); // "key:\n- a" at the same indent
            else
                b_.setValue(child, Kind::Null, "null");
        }
    }
};

} // namespace

const char* kindName(Kind kind)
{
    switch (kind) {
    case Kind::Object: return "object";
    case Kind::Array: return "array";
    case Kind::String: return "string";
    case Kind::Number: return "number";
    case Kind::Bool: return "boolean";
    case Kind::Null: return "null";
    }
    return "";
}

std::uint32_t Tree::indexInParent(std::uint32_t index) const
{
    const Node& n = nodes[index];
    if (n.parent == kNone)
        return 0;
    std::uint32_t i = 0;
    for (std::uint32_t c = nodes[n.parent].firstChild; c != kNone && c != index; c = nodes[c].nextSibling)
        ++i;
    return i;
}

std::string Tree::path(std::uint32_t index) const
{
    std::vector<std::uint32_t> chain;
    for (std::uint32_t i = index; i != kNone && nodes[i].parent != kNone; i = nodes[i].parent)
        chain.push_back(i);
    std::string out = "$";
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const Node& n = nodes[*it];
        if (nodes[n.parent].kind == Kind::Array) {
            out += '[' + std::to_string(indexInParent(*it)) + ']';
        } else {
            const std::string_view k = key(n);
            const bool simple = !k.empty() && std::all_of(k.begin(), k.end(), [](char c) {
                return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
            });
            if (simple)
                out += '.' + std::string(k);
            else
                out += "[\"" + std::string(k) + "\"]";
        }
    }
    return out;
}

Tree parseJson(std::string_view text)
{
    Tree t;
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF")
        text.remove_prefix(3);
    JsonParser(text, t).run();
    finish(t);
    return t;
}

Tree parseYaml(std::string_view text)
{
    Tree t;
    if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF")
        text.remove_prefix(3);
    YamlParser(text, t).run();
    if (t.error.empty() && t.nodes.empty())
        t.error = "the file is empty";
    finish(t);
    return t;
}

} // namespace enp::model::data
