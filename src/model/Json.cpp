#include "enp/model/Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace enp::model {

namespace {

void appendUtf8(std::string& out, unsigned c)
{
    if (c < 0x80) {
        out += char(c);
    } else if (c < 0x800) {
        out += char(0xC0 | (c >> 6));
        out += char(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
        out += char(0xE0 | (c >> 12));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    } else {
        out += char(0xF0 | (c >> 18));
        out += char(0x80 | ((c >> 12) & 0x3F));
        out += char(0x80 | ((c >> 6) & 0x3F));
        out += char(0x80 | (c & 0x3F));
    }
}

void dumpString(std::string& out, const std::string& s)
{
    out += '"';
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out += ch;
            }
        }
    }
    out += '"';
}

void dumpValue(std::string& out, const Json& j)
{
    switch (j.type()) {
    case Json::Type::Null: out += "null"; break;
    case Json::Type::Bool: out += j.asBool() ? "true" : "false"; break;
    case Json::Type::Number: {
        const double n = j.asNumber();
        char buffer[32];
        if (std::isfinite(n) && n == std::floor(n) && std::fabs(n) < 1e15)
            std::snprintf(buffer, sizeof(buffer), "%.0f", n);
        else if (std::isfinite(n))
            std::snprintf(buffer, sizeof(buffer), "%.17g", n);
        else
            std::snprintf(buffer, sizeof(buffer), "null");
        out += buffer;
        break;
    }
    case Json::Type::String: dumpString(out, j.asString()); break;
    case Json::Type::Array:
        out += '[';
        for (size_t i = 0; i < j.items().size(); ++i) {
            if (i)
                out += ',';
            dumpValue(out, j.items()[i]);
        }
        out += ']';
        break;
    case Json::Type::Object:
        out += '{';
        for (size_t i = 0; i < j.members().size(); ++i) {
            if (i)
                out += ',';
            dumpString(out, j.members()[i].first);
            out += ':';
            dumpValue(out, j.members()[i].second);
        }
        out += '}';
        break;
    }
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    std::optional<Json> parseDocument(std::string* error)
    {
        std::optional<Json> value = parseValue(0);
        skipSpace();
        if (value && pos_ != s_.size())
            fail("unexpected trailing characters");
        if (!error_.empty()) {
            if (error)
                *error = error_ + " at offset " + std::to_string(pos_);
            return std::nullopt;
        }
        return value;
    }

private:
    void fail(const char* message)
    {
        if (error_.empty())
            error_ = message;
    }
    void skipSpace()
    {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r'))
            ++pos_;
    }
    bool consume(std::string_view word)
    {
        if (s_.substr(pos_, word.size()) != word)
            return false;
        pos_ += word.size();
        return true;
    }

    std::optional<Json> parseValue(int depth)
    {
        if (depth > 200) {
            fail("nesting too deep");
            return std::nullopt;
        }
        skipSpace();
        if (pos_ >= s_.size()) {
            fail("unexpected end of input");
            return std::nullopt;
        }
        const char c = s_[pos_];
        if (c == '{')
            return parseObject(depth);
        if (c == '[')
            return parseArray(depth);
        if (c == '"') {
            std::string str;
            if (!parseString(str))
                return std::nullopt;
            return Json(std::move(str));
        }
        if (consume("true"))
            return Json(true);
        if (consume("false"))
            return Json(false);
        if (consume("null"))
            return Json();
        if (c == '-' || (c >= '0' && c <= '9'))
            return parseNumber();
        fail("unexpected character");
        return std::nullopt;
    }

    std::optional<Json> parseNumber()
    {
        const size_t start = pos_;
        if (s_[pos_] == '-')
            ++pos_;
        while (pos_ < s_.size() && ((s_[pos_] >= '0' && s_[pos_] <= '9') || s_[pos_] == '.' || s_[pos_] == 'e' ||
                                    s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-'))
            ++pos_;
        const std::string text(s_.substr(start, pos_ - start));
        char* end = nullptr;
        const double value = std::strtod(text.c_str(), &end);
        if (end != text.c_str() + text.size()) {
            fail("malformed number");
            return std::nullopt;
        }
        return Json(value);
    }

    bool parseHex4(unsigned& out)
    {
        if (pos_ + 4 > s_.size()) {
            fail("truncated \\u escape");
            return false;
        }
        out = 0;
        for (int i = 0; i < 4; ++i) {
            const char h = s_[pos_++];
            out <<= 4;
            if (h >= '0' && h <= '9')
                out |= unsigned(h - '0');
            else if (h >= 'a' && h <= 'f')
                out |= unsigned(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F')
                out |= unsigned(h - 'A' + 10);
            else {
                fail("bad \\u escape");
                return false;
            }
        }
        return true;
    }

    bool parseString(std::string& out)
    {
        ++pos_; // opening quote
        while (pos_ < s_.size()) {
            const char c = s_[pos_++];
            if (c == '"')
                return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos_ >= s_.size())
                break;
            const char e = s_[pos_++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned code = 0;
                if (!parseHex4(code))
                    return false;
                if (code >= 0xD800 && code <= 0xDBFF && s_.substr(pos_, 2) == "\\u") { // surrogate pair
                    pos_ += 2;
                    unsigned low = 0;
                    if (!parseHex4(low))
                        return false;
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                appendUtf8(out, code);
                break;
            }
            default: fail("bad escape"); return false;
            }
        }
        fail("unterminated string");
        return false;
    }

    std::optional<Json> parseArray(int depth)
    {
        ++pos_;
        Json array = Json::array();
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == ']') {
            ++pos_;
            return array;
        }
        for (;;) {
            std::optional<Json> value = parseValue(depth + 1);
            if (!value)
                return std::nullopt;
            array.push(std::move(*value));
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == ']') {
                ++pos_;
                return array;
            }
            fail("expected ',' or ']'");
            return std::nullopt;
        }
    }

    std::optional<Json> parseObject(int depth)
    {
        ++pos_;
        Json object = Json::object();
        skipSpace();
        if (pos_ < s_.size() && s_[pos_] == '}') {
            ++pos_;
            return object;
        }
        for (;;) {
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != '"') {
                fail("expected a key");
                return std::nullopt;
            }
            std::string key;
            if (!parseString(key))
                return std::nullopt;
            skipSpace();
            if (pos_ >= s_.size() || s_[pos_] != ':') {
                fail("expected ':'");
                return std::nullopt;
            }
            ++pos_;
            std::optional<Json> value = parseValue(depth + 1);
            if (!value)
                return std::nullopt;
            object.set(key, std::move(*value));
            skipSpace();
            if (pos_ < s_.size() && s_[pos_] == ',') {
                ++pos_;
                continue;
            }
            if (pos_ < s_.size() && s_[pos_] == '}') {
                ++pos_;
                return object;
            }
            fail("expected ',' or '}'");
            return std::nullopt;
        }
    }

    std::string_view s_;
    size_t pos_ = 0;
    std::string error_;
};

} // namespace

const std::string& Json::asString() const
{
    static const std::string empty;
    return type_ == Type::String ? string_ : empty;
}

Json& Json::push(Json value)
{
    if (type_ != Type::Array) {
        *this = array();
    }
    items_.push_back(std::move(value));
    return *this;
}

Json& Json::set(const std::string& key, Json value)
{
    if (type_ != Type::Object)
        *this = object();
    for (auto& member : members_)
        if (member.first == key) {
            member.second = std::move(value);
            return *this;
        }
    members_.emplace_back(key, std::move(value));
    return *this;
}

const Json* Json::find(std::string_view key) const
{
    for (const auto& member : members_)
        if (member.first == key)
            return &member.second;
    return nullptr;
}

std::string Json::getString(std::string_view key, const std::string& fallback) const
{
    const Json* j = find(key);
    return j && j->isString() ? j->asString() : fallback;
}

double Json::getNumber(std::string_view key, double fallback) const
{
    const Json* j = find(key);
    return j && j->isNumber() ? j->asNumber() : fallback;
}

bool Json::getBool(std::string_view key, bool fallback) const
{
    const Json* j = find(key);
    return j && j->isBool() ? j->asBool() : fallback;
}

std::string Json::dump() const
{
    std::string out;
    dumpValue(out, *this);
    return out;
}

std::optional<Json> Json::parse(std::string_view text, std::string* error)
{
    return Parser(text).parseDocument(error);
}

} // namespace enp::model
