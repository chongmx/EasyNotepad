// Documents read for viewing: Word (.docx), Excel (.xlsx), PowerPoint (.pptx) and PDF. Each
// reader is a pure function from the file's bytes to one of three shapes, shared by every
// format that has it:
//   Flow   text that reflows to the reader's width: the Markdown renderer's blocks (headings,
//          paragraphs, lists, tables, images), so a document reads like a rendered .md file
//   Sheet  a workbook of cell grids
//   Pages  fixed-layout pages drawn as display lists (slides, PDF pages)
// Nothing here draws, reads files or knows about workspaces: the views decide where they appear.
#pragma once

#include "enp/model/Markdown.h"
#include "enp/model/viewer/Page.h"
#include "enp/model/viewer/Workbook.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::viewer {

enum class Shape { None, Flow, Sheet, Pages };

struct FlowDocument {
    std::vector<markdown::Block> blocks;
    // Encoded images by the source their Image spans (and the drawings' images) name.
    std::map<std::string, std::string> media;
    // Fixed drawings (charts, shapes) between the blocks, sized in points: a Drawing block names one.
    std::vector<Page> drawings;
    // The font families the runs are in ("Calibri", "Times New Roman"), as Span::font numbers them.
    std::vector<std::string> fonts;

    // As printed (the page view): the page's size and margins in points, the header and footer
    // every page repeats, the blocks the pages flow through in order (the body, then endnotes and
    // comments: not the header, footer and footnotes `blocks` holds for reading), and each
    // footnote with the block that refers to it (it goes at the foot of that block's page).
    struct PageSetup {
        float width = 612.0f, height = 792.0f;
        float top = 72.0f, right = 72.0f, bottom = 72.0f, left = 72.0f;
        float header = 36.0f, footer = 36.0f; // from the page's edge to the header's top / the footer's bottom
    };
    PageSetup page;
    std::vector<markdown::Block> header, footer;
    std::vector<std::size_t> printed;
    std::vector<std::pair<std::size_t, std::size_t>> footnotes; // (footnote block, referring block), into `blocks`
    // A floating picture (wp:anchor): where it sits on the page of the paragraph that anchors it.
    // Offsets in points from its reference: horizontally 0 the page, 1 the margin, 2 the column /
    // character (the text's left edge); vertically 0 the page, 1 the margin, 2 the paragraph / line.
    // An alignment (0 start, 1 centre, 2 end) replaces the offset; -1 none.
    struct Float {
        std::size_t block = 0; // into `blocks`
        std::string media;
        float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
        std::uint8_t fromH = 2, fromV = 2;
        std::int8_t alignH = -1, alignV = -1;
        bool behind = false; // behind the text (else over it)
        // How the text goes round it: 0 not at all (in front of it or behind it), 1 beside it
        // (square, tight, through), 2 above and below it; keeping `dist` (points: left, top,
        // right, bottom) from it.
        std::uint8_t wrap = 0;
        float dist[4] = {9.0f, 0.0f, 9.0f, 0.0f};
        Page drawing; // a group of drawn shapes (wpg), in points from its box's top left: drawn instead of a picture
    };
    std::vector<Float> floats;
    // The header's floating pictures and drawings: on every page, behind the text, placed from
    // the header's first paragraph.
    std::vector<Float> headerFloats;
};

// Scripts whose letters the bundled fonts lack: the views merge a system font in while a document
// that uses them is shown. Han: Chinese characters, kana, CJK punctuation and full-width forms.
enum Script : std::uint8_t { ScriptHan = 1, ScriptHangul = 2 };

// The Script bits of the letters in `utf8`.
std::uint8_t scriptsIn(std::string_view utf8);

struct ViewerDocument {
    Shape shape = Shape::None;
    std::string format; // "Word document", "Excel workbook", "PowerPoint presentation", "PDF document"
    std::string error;  // why nothing could be read (shape None)
    // What was in the file but is not shown ("2 charts", "comments"): said, never dropped silently.
    std::vector<std::string> notShown;
    FlowDocument flow;
    Workbook book;
    PageDocument pages;
    std::uint8_t scripts = 0; // Script bits of its text (pages not read yet: their own, when read)
};

// The format a file is by its content (and extension for the ZIP-based ones), or "" when it is not
// one this reads. Cheap: `bytes` may be the first kilobytes.
std::string viewerFormat(std::string_view path, std::string_view bytes);

// Reads the whole file. Never throws; a file that cannot be read gives shape None and `error`.
ViewerDocument readViewerDocument(std::string_view path, std::string bytes);

// The readers, for tests and tools.
ViewerDocument readDocx(std::string bytes);
ViewerDocument readXlsx(std::string bytes);
ViewerDocument readPptx(std::string bytes);
ViewerDocument readPdf(std::string bytes);

// The document's text in reading order (copying, agents, tests).
std::string plainText(const ViewerDocument& doc);

} // namespace enp::model::viewer
