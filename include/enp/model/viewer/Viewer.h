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
