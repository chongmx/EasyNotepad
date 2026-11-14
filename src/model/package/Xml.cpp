#include "enp/model/package/Xml.h"

#include <cstdint>
#include <cstdlib>

namespace enp::model::package {
namespace {

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isNameChar(char c)
{
    return !isSpace(c) && c != '>' && c != '/' && c != '=' && c != '<' && c != '"' && c != '\'';
}

void appendUtf8(std::string& out, std::uint32_t cp)
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

} // namespace

XmlReader::XmlReader(std::string_view xml) : xml_(xml)
{
    if (xml_.substr(0, 3) == "\xEF\xBB\xBF")
        at_ = 3;
}

std::string_view XmlReader::localPart(std::string_view qualified)
{
    const std::size_t colon = qualified.find(':');
    return colon == std::string_view::npos ? qualified : qualified.substr(colon + 1);
}

std::string_view XmlReader::localName() const
{
    return localPart(name_);
}

std::string XmlReader::decode(std::string_view raw)
{
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const char c = raw[i];
        if (c != '&') {
            out += c;
            continue;
        }
        const std::size_t semi = raw.find(';', i + 1);
        if (semi == std::string_view::npos || semi - i > 12) {
            out += c;
            continue;
        }
        const std::string_view ref = raw.substr(i + 1, semi - i - 1);
        if (ref == "lt")
            out += '<';
        else if (ref == "gt")
            out += '>';
        else if (ref == "amp")
            out += '&';
        else if (ref == "quot")
            out += '"';
        else if (ref == "apos")
            out += '\'';
        else if (ref.size() > 1 && ref[0] == '#') {
            const bool hex = ref[1] == 'x' || ref[1] == 'X';
            const std::string digits(ref.substr(hex ? 2 : 1));
            char* end = nullptr;
            const unsigned long cp = std::strtoul(digits.c_str(), &end, hex ? 16 : 10);
            if (digits.empty() || *end != 0) {
                out += c;
                continue;
            }
            appendUtf8(out, std::uint32_t(cp > 0x10FFFF ? 0xFFFD : cp));
        } else {
            out.append(raw.substr(i, semi - i + 1)); // an entity no DTD of ours defines: kept as written
        }
        i = semi;
    }
    return out;
}

XmlReader::Event XmlReader::fail(const char* why)
{
    error_ = why;
    at_ = xml_.size();
    return Event::Error;
}

XmlReader::Event XmlReader::next()
{
    if (!error_.empty())
        return Event::Error;
    if (pendingEnd_) {
        pendingEnd_ = false;
        attributes_.clear();
        --depth_;
        return Event::End;
    }
    attributes_.clear();
    while (at_ < xml_.size()) {
        if (xml_[at_] != '<') {
            // Text up to the next tag.
            const std::size_t lt = xml_.find('<', at_);
            const std::size_t end = lt == std::string_view::npos ? xml_.size() : lt;
            text_ = decode(xml_.substr(at_, end - at_));
            at_ = end;
            if (depth_ > 0)
                return Event::Text;
            continue; // outside the root element: whitespace, ignored
        }
        const std::string_view rest = xml_.substr(at_);
        if (rest.substr(0, 4) == "<!--") {
            const std::size_t end = xml_.find("-->", at_ + 4);
            at_ = end == std::string_view::npos ? xml_.size() : end + 3;
            continue;
        }
        if (rest.substr(0, 9) == "<![CDATA[") {
            const std::size_t end = xml_.find("]]>", at_ + 9);
            if (end == std::string_view::npos)
                return fail("unterminated CDATA section");
            text_.assign(xml_.substr(at_ + 9, end - at_ - 9));
            at_ = end + 3;
            if (depth_ > 0)
                return Event::Text;
            continue;
        }
        if (rest.substr(0, 2) == "<?") {
            const std::size_t end = xml_.find("?>", at_ + 2);
            at_ = end == std::string_view::npos ? xml_.size() : end + 2;
            continue;
        }
        if (rest.substr(0, 2) == "<!") { // DOCTYPE (with an internal subset in brackets): skipped, never read
            std::size_t i = at_ + 2;
            int brackets = 0;
            for (; i < xml_.size(); ++i) {
                if (xml_[i] == '[')
                    ++brackets;
                else if (xml_[i] == ']')
                    --brackets;
                else if (xml_[i] == '>' && brackets <= 0)
                    break;
            }
            at_ = i + 1;
            continue;
        }
        if (rest.substr(0, 2) == "</") {
            std::size_t i = at_ + 2;
            const std::size_t nameStart = i;
            while (i < xml_.size() && isNameChar(xml_[i]))
                ++i;
            name_ = xml_.substr(nameStart, i - nameStart);
            const std::size_t gt = xml_.find('>', i);
            if (gt == std::string_view::npos)
                return fail("unterminated end tag");
            at_ = gt + 1;
            if (depth_ == 0)
                return fail("an end tag without its start");
            --depth_;
            return Event::End;
        }
        // A start tag.
        std::size_t i = at_ + 1;
        const std::size_t nameStart = i;
        while (i < xml_.size() && isNameChar(xml_[i]))
            ++i;
        if (i == nameStart)
            return fail("a tag without a name");
        name_ = xml_.substr(nameStart, i - nameStart);
        for (;;) {
            while (i < xml_.size() && isSpace(xml_[i]))
                ++i;
            if (i >= xml_.size())
                return fail("unterminated start tag");
            if (xml_[i] == '>') {
                ++i;
                break;
            }
            if (xml_[i] == '/') {
                if (i + 1 >= xml_.size() || xml_[i + 1] != '>')
                    return fail("a stray '/' in a tag");
                i += 2;
                pendingEnd_ = true;
                break;
            }
            const std::size_t attrStart = i;
            while (i < xml_.size() && isNameChar(xml_[i]))
                ++i;
            const std::string_view attrName = xml_.substr(attrStart, i - attrStart);
            while (i < xml_.size() && isSpace(xml_[i]))
                ++i;
            if (attrName.empty() || i >= xml_.size() || xml_[i] != '=')
                return fail("an attribute without a value");
            ++i;
            while (i < xml_.size() && isSpace(xml_[i]))
                ++i;
            if (i >= xml_.size() || (xml_[i] != '"' && xml_[i] != '\''))
                return fail("an attribute value without quotes");
            const char quote = xml_[i];
            const std::size_t close = xml_.find(quote, i + 1);
            if (close == std::string_view::npos)
                return fail("an unterminated attribute value");
            if (attributes_.size() < 512)
                attributes_.push_back({attrName, xml_.substr(i + 1, close - i - 1)});
            i = close + 1;
        }
        at_ = i;
        if (++depth_ > kMaxDepth)
            return fail("elements nested too deeply");
        return Event::Start;
    }
    if (depth_ > 0)
        return fail("the document ends inside an element");
    return Event::Eof;
}

const XmlReader::Attribute* XmlReader::findAttribute(std::string_view name) const
{
    for (const Attribute& a : attributes_)
        if (a.name == name)
            return &a;
    if (name.find(':') == std::string_view::npos)
        for (const Attribute& a : attributes_)
            if (localPart(a.name) == name && a.name.substr(0, 6) != "xmlns:")
                return &a;
    return nullptr;
}

std::string XmlReader::attribute(std::string_view name) const
{
    const Attribute* a = findAttribute(name);
    return a ? decode(a->raw) : std::string();
}

bool XmlReader::hasAttribute(std::string_view name) const
{
    return findAttribute(name) != nullptr;
}

void XmlReader::skip()
{
    const int target = depth_ - 1;
    for (;;) {
        const Event e = next();
        if (e == Event::Eof || e == Event::Error || (e == Event::End && depth_ == target))
            return;
    }
}

std::string XmlReader::elementText()
{
    std::string out;
    const int target = depth_ - 1;
    for (;;) {
        const Event e = next();
        if (e == Event::Text)
            out += text_;
        else if (e == Event::Eof || e == Event::Error || (e == Event::End && depth_ == target))
            return out;
    }
}

} // namespace enp::model::package
