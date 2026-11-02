// Markdown read into blocks (headings, paragraphs, lists, quotes, code, tables, rules, display
// math) with styled inline spans (emphasis, code, links, images, inline math, HTML entities), for
// drawing it formatted: the agent's answers and .md files shown rendered. CommonMark's common
// cases, not every corner of it; text it cannot place stays plain.
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace enp::model::markdown {

enum Style : unsigned {
    Plain = 0,
    Bold = 1u << 0,
    Italic = 1u << 1,
    Code = 1u << 2,
    Strike = 1u << 3,
    Link = 1u << 4,
    Image = 1u << 5, // ![alt](src) or <img src alt>: text is the alt text, url the source
    Math = 1u << 6,  // $...$ or \(...\): text is the LaTeX source
    Emoji = 1u << 7, // one emoji sequence (model/Emoji.h): text is its characters, emoji its picture key
};

struct Span {
    std::string text;
    unsigned style = Plain;
    std::string url;   // for Link and Image
    std::string emoji; // for Emoji: the key of its picture in the emoji pack
};
using Spans = std::vector<Span>;

enum class BlockKind { Paragraph, Heading, Bullet, Numbered, Task, Code, Rule, Table, Math };

struct Block {
    BlockKind kind = BlockKind::Paragraph;
    int level = 0;          // heading 1..6; list nesting depth (0 = top)
    int quote = 0;          // inside this many block quotes (> ...)
    std::string marker;     // "3." for a numbered item
    bool checked = false;   // Task: [x]
    Spans spans;            // the inline text (not for Code, Rule or Table)
    std::string code;       // Code: the lines, '\n'-separated; Math: the LaTeX source ($$...$$ or \[...\])
    std::string language;   // Code: the fence's info word
    std::vector<std::vector<Spans>> rows; // Table: cells; the first row is the header
    std::vector<int> align; // Table: per column -1 left, 0 centre, 1 right
    int line = 0;           // first source line (0-based), to go back to the text
};

std::vector<Block> parse(std::string_view text);
Spans parseInline(std::string_view text);
// The spans' text without markup.
std::string plainText(const Spans& spans);
// HTML character references (&amp; &copy; &rarr; &#169; &#xA9; and the common named ones) as text.
std::string decodeEntities(std::string_view text);

} // namespace enp::model::markdown
