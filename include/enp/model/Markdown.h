// Markdown read into blocks (headings, paragraphs, lists, quotes, code, tables, rules, display
// math) with styled inline spans (emphasis, code, links, images, inline math, HTML entities), for
// drawing it formatted: the agent's answers and .md files shown rendered. CommonMark's common
// cases, not every corner of it; text it cannot place stays plain.
#pragma once

#include <cstdint>
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
    Image = 1u << 5,    // ![alt](src) or <img src alt>: text is the alt text, url the source
    Math = 1u << 6,     // $...$ or \(...\): text is the LaTeX source
    Emoji = 1u << 7,    // one emoji sequence (model/Emoji.h): text is its characters, emoji its picture key
    Display = 1u << 8,  // with Math: $$...$$ within a paragraph, set apart on lines of its own
    Underline = 1u << 9,
    PageNumber = 1u << 10, // documents: the number of the page it is drawn on (a PAGE field); text: the saved one
    PageCount = 1u << 11,  // documents: the number of pages (NUMPAGES)
    Floating = 1u << 12,   // with Image: placed on the page by the document (FlowDocument::floats), not in the line
};

struct Span {
    std::string text;
    unsigned style = Plain;
    std::string url;   // for Link and Image
    std::string emoji; // for Emoji: the key of its picture in the emoji pack
    // Documents read for viewing (Word) give a run its own look; Markdown leaves these at 0.
    std::uint32_t color = 0;     // text colour (packed like ImU32); 0: the block's
    std::uint32_t highlight = 0; // background behind the text; 0: none
    float size = 0.0f;           // points; 0: the block's size
    std::uint8_t font = 0;       // 1-based into the document's font names (FlowDocument::fonts); 0: the view's
    float width = 0.0f, height = 0.0f; // Image: its size in points as the document sets it (0: its pixels')
};
using Spans = std::vector<Span>;

// Drawing: a fixed drawing of a document read for viewing (a chart, a shape): `level` is its index in
// the document's drawings. The Markdown parser never makes one.
enum class BlockKind { Paragraph, Heading, Bullet, Numbered, Task, Code, Rule, Table, Math, Drawing };

// A document paragraph's tab stop (Word): where a tab character moves the text to.
struct TabStop {
    float pos = 0.0f;        // points from the text column's left edge
    std::uint8_t kind = 0;   // 0 left, 1 centre, 2 right, 3 decimal (the text's '.' at the stop)
    char leader = 0;         // what fills the gap: '.', '-', '_', or 0 nothing
};

// A document table's cell (Word): its look, as the table's style, its parts (header row, banding)
// and the cell's own properties give it.
struct CellEdge {
    float width = 0.0f;      // points; 0: no line
    std::uint32_t color = 0; // 0: the text's colour
};
struct CellLook {
    std::uint32_t fill = 0;    // background; 0: none
    std::uint32_t color = 0;   // text colour where a run sets none; 0: the block's
    bool bold = false;         // its text bold (a header row's)
    std::int8_t align = -1;    // its first paragraph's alignment: -1 left, 1 centre, 2 right
    std::vector<std::int8_t> paragraphAligns; // each paragraph's (the '\n'-separated parts of its text)
    std::int8_t valign = 0;    // 0 top, 1 centre, 2 bottom
    std::uint8_t span = 1;     // columns it covers (gridSpan)
    bool covered = false;      // inside the span of a cell before it on its row: not drawn
    bool merged = false;       // continues the cell above (vMerge): no text, no line between them
    CellEdge edges[4];         // left, top, right, bottom
};

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
    // Documents read for viewing (Word): the paragraph's own layout, in points; Markdown leaves 0.
    int textAlign = 0;            // 0 left (or justified), 1 centre, 2 right
    float size = 0.0f;            // the paragraph's text size; 0: the renderer's (11 points)
    float indentLeft = 0.0f, indentRight = 0.0f, firstLine = 0.0f; // firstLine < 0: a hanging indent
    float spaceBefore = 0.0f;     // above the paragraph, beyond the renderer's own gap
    std::uint32_t shading = 0;    // the paragraph's background (0: none)
    std::vector<TabStop> tabs;    // its tab stops, by position (none: every half inch)
    // Spacing as the author set it (points), used instead of the renderer's gaps when
    // `documentSpacing` is set; and where pages break (the page view).
    bool documentSpacing = false;
    float spaceAfter = 0.0f;
    float lineMultiple = 0.0f;    // lines this many times the font's single line; 0: single
    float lineExact = 0.0f;       // > 0: lines exactly (with lineAtLeast: at least) this tall
    bool lineAtLeast = false;
    bool contextual = false;      // no spacing to a neighbour of the same paragraph style (lists)
    std::uint16_t style = 0;      // its paragraph style, numbered (contextual spacing compares them)
    bool pageBreakBefore = false; // starts a new page
    bool keepNext = false;        // kept on the page of the block after it
    std::vector<float> columns;   // Table: Word's grid, the columns' widths in points (empty: by their text)
    std::vector<std::vector<CellLook>> looks; // Table: each cell's look, as `rows` (empty: the renderer's own)
};

std::vector<Block> parse(std::string_view text);
Spans parseInline(std::string_view text);
// The spans' text without markup.
std::string plainText(const Spans& spans);
// HTML character references (&amp; &copy; &rarr; &#169; &#xA9; and the common named ones) as text.
std::string decodeEntities(std::string_view text);

} // namespace enp::model::markdown
