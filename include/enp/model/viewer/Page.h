// Fixed-layout pages: what a slide (.pptx) or a PDF page is once read. A display list in page
// points (1/72 inch), origin at the top left, drawn in order; one painter draws both formats.
// Colours are packed like Dear ImGui's ImU32 (0xAABBGGRR) without depending on it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace enp::model::viewer {

constexpr std::uint32_t rgba(unsigned r, unsigned g, unsigned b, unsigned a = 255)
{
    return std::uint32_t(r) | (std::uint32_t(g) << 8) | (std::uint32_t(b) << 16) | (std::uint32_t(a) << 24);
}
constexpr std::uint32_t kBlack = rgba(0, 0, 0);
constexpr std::uint32_t kWhite = rgba(255, 255, 255);
constexpr std::uint32_t kNone = 0; // no fill or no stroke

struct Point {
    float x = 0.0f, y = 0.0f;
};

struct Rect {
    float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
};

// A colour that changes across a shape: along a direction (linear) or out from a centre (radial).
struct GradientStop {
    float position = 0.0f; // 0..1
    std::uint32_t color = kBlack;
};
struct Gradient {
    std::vector<GradientStop> stops; // empty: no gradient
    float angle = 0.0f;              // linear: the direction the colour changes in, degrees clockwise from left-to-right
    bool radial = false;             // radial: from the centre of the shape's bounds out to its corners
    // PDF shadings give the axis itself (page points): linear from `from` to `to`, radial from a
    // circle at `from` of radius r0 to one at `to` of radius r1. Otherwise the shape's bounds do.
    bool absolute = false;
    Point from, to;
    float r0 = 0.0f, r1 = 0.0f;
    std::uint32_t at(float t) const; // the colour at 0..1
};

// The end of a line: none, or a filled triangle, an open arrow, a stealth arrow, a diamond or a dot.
enum class LineEnd : std::uint8_t { None, Triangle, Arrow, Stealth, Diamond, Oval };

// Filled and / or stroked outlines, already flattened to polygons.
struct PathItem {
    std::vector<std::vector<Point>> subpaths;
    std::vector<bool> closed; // per subpath
    std::uint32_t fill = kNone; // with a gradient: its first colour (a painter without gradients uses it)
    std::uint32_t stroke = kNone;
    float lineWidth = 1.0f;
    Gradient gradient;          // over the fill
    std::vector<float> dash;    // on, off, on, ... lengths in points; empty: solid
    LineEnd head = LineEnd::None, tail = LineEnd::None; // at the first and the last point of an open subpath
};

enum TextFlags : std::uint8_t { Bold = 1, Italic = 2, Underline = 4, Mono = 8, Strike = 16 };

// Text placed glyph by glyph (PDF): each code point at its own x, all on one baseline.
struct GlyphRun {
    float x = 0.0f, baseline = 0.0f;
    float angle = 0.0f; // degrees clockwise: the run goes along this direction from (x, baseline)
    float size = 12.0f;
    std::uint32_t color = kBlack;
    std::uint8_t flags = 0;
    std::string text;           // UTF-8
    std::vector<float> offsets; // x of each code point, relative to `x`
    float width = 0.0f;         // the run's advance
};

// Text laid out by the painter inside a box (slides): paragraphs of styled runs, wrapped.
struct TextRun {
    std::string text;
    float size = 18.0f;
    std::uint32_t color = kBlack;
    std::uint8_t flags = 0;
};

struct TextParagraph {
    std::vector<TextRun> runs;
    int align = 0;      // -1 left... 0 left, 1 centre, 2 right, 3 justified (drawn left)
    int level = 0;      // list level: indents
    std::string bullet; // "•", "1.", or empty
    float spaceBefore = 0.0f; // points
    float lineSpacing = 1.0f; // multiple of the line height
};

struct TextBox {
    Rect box;
    float rotation = 0.0f; // degrees clockwise about the box's centre (a turned shape, vertical text)
    std::vector<TextParagraph> paragraphs;
    int anchor = 0; // 0 top, 1 middle, 2 bottom
    float insetL = 7.2f, insetT = 3.6f, insetR = 7.2f, insetB = 3.6f;
    bool wrap = true;
};

struct ImageItem {
    Rect box;
    std::string media; // key into PageDocument::media
    Rect crop{0.0f, 0.0f, 1.0f, 1.0f}; // the part of the picture shown, in 0..1 of its width and height
    float rotation = 0.0f;              // degrees clockwise about the box's centre
    bool flipH = false, flipV = false;
};

struct PageItem {
    enum class Kind : std::uint8_t { Path, Glyphs, Box, Image };
    Kind kind;
    std::uint32_t index; // into the page's list of that kind
    std::int32_t clip = -1; // into Page::clips: drawn only inside it; -1: the whole page
};

struct Page {
    float width = 612.0f, height = 792.0f; // points
    std::uint32_t background = kWhite;
    Gradient backgroundGradient; // over the whole page, when the background is one
    // False while the page's content is not read yet (PDF reads pages as they are shown): only
    // its size and title are known. PageDocument::load fills it.
    bool loaded = true;
    std::vector<PageItem> items; // drawing order
    std::vector<PathItem> paths;
    std::vector<GlyphRun> glyphs;
    std::vector<TextBox> boxes;
    std::vector<ImageItem> images;
    std::vector<Rect> clips; // clip rectangles items refer to (a PDF's clip paths, by their bounds)
    std::int32_t clip = -1;  // while reading: the clip the items added now get
    std::string title; // for the page list: a slide's title, or "Page 3"
    std::string notes; // a slide's speaker notes
    std::uint8_t scripts = 0; // Script bits of its text (Viewer.h): fonts the views add while it is shown

    void add(PathItem p);
    void add(GlyphRun g);
    void add(TextBox b);
    void add(ImageItem i);
    // The page's text in reading order, for copying and for agents.
    std::string plainText() const;
};

// Reads one page's content on request (PDF). Keeps what it needs of the file.
class PageSource {
public:
    virtual ~PageSource() = default;
    // Fills `page` (its items) and adds the images it uses to `media`. False when it cannot.
    virtual bool load(std::size_t index, Page& page, std::map<std::string, std::string>& media) = 0;
};

struct PageDocument {
    std::vector<Page> pages;
    // Encoded images (PNG, JPEG, QOI, ...) by key; the views decode them when first shown and may
    // drop the bytes then (a page read again puts them back).
    std::map<std::string, std::string> media;
    // Pages not loaded yet are read through it, on the reader service's thread while the document
    // is shown here: shared across threads, so a shared_ptr (the last holder frees the file).
    std::shared_ptr<PageSource> source;

    // Reads page `index` if it is not loaded; true when it is loaded now.
    bool load(std::size_t index);
    // Drops a page's content (its size and title stay): memory for pages scrolled far away.
    void unload(std::size_t index);
};

} // namespace enp::model::viewer
