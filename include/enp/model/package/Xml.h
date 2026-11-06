// A small pull parser for the XML inside documents (OOXML parts, relationships, content types).
// Not validating; safe by construction: no DTD is read (a DOCTYPE is skipped), no entity but the
// five predefined and character references is expanded, nothing is fetched, and nesting is
// limited. Names are kept with their prefix ("w:p"); readers match the local name.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::package {

class XmlReader {
public:
    enum class Event { Start, End, Text, Eof, Error };
    static constexpr int kMaxDepth = 256;

    explicit XmlReader(std::string_view xml);

    // The next start tag, end tag or text. A self-closing tag gives Start, then End.
    Event next();
    // Start / End: the element's name with its prefix, and without it.
    std::string_view name() const { return name_; }
    std::string_view localName() const;
    // Start: an attribute's value (entities decoded), by name with its prefix ("r:id") or, when
    // `name` has no prefix and no attribute is called exactly that, by local name. Empty if absent.
    std::string attribute(std::string_view name) const;
    bool hasAttribute(std::string_view name) const;
    // Text: the characters (entities decoded; CDATA as is).
    const std::string& text() const { return text_; }
    // Elements open around the current position (a Start counts its own element).
    int depth() const { return depth_; }
    // After a Start: skips to the element's End (so next() continues after it).
    void skip();
    // After a Start: the text of the element and everything inside it, concatenated (then at its End).
    std::string elementText();
    const std::string& error() const { return error_; }

    static std::string decode(std::string_view raw);
    static std::string_view localPart(std::string_view qualified);

private:
    struct Attribute {
        std::string_view name;
        std::string_view raw;
    };
    const Attribute* findAttribute(std::string_view name) const;
    Event fail(const char* why);

    std::string_view xml_;
    std::size_t at_ = 0;
    std::string_view name_;
    std::vector<Attribute> attributes_;
    std::string text_;
    std::string error_;
    int depth_ = 0;
    bool pendingEnd_ = false; // a self-closing tag: its End comes next
};

} // namespace enp::model::package
