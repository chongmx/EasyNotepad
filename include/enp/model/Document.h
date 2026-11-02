// An open document: text, caret/selection and the file attributes shown in the status bar.
#pragma once

#include "enp/model/FileKind.h"
#include "enp/model/Highlighter.h"
#include "enp/model/Indent.h"
#include "enp/model/Language.h"
#include "enp/model/TextBuffer.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model {

enum class EolMode { CrLf, Lf, Cr };
enum class Encoding { Utf8, Utf8Bom, Utf16Le, Utf16Be, Ansi };

const char* eolName(EolMode eol);       // "Windows (CR LF)"
const char* eolShortName(EolMode eol);  // "CRLF"
const char* encodingName(Encoding encoding);

// JSON and YAML: shown as text (to edit), as a tree with the chosen value beside it, or as nested
// sections that open and close. (Markdown and CSV use Document::rendered.)
enum class DataView { Text, Tree, Accordion };

// One caret with its selection: [min(anchor, caret), max(anchor, caret)).
struct Selection {
    TextPos anchor;
    TextPos caret;

    bool empty() const { return anchor == caret; }
    TextPos start() const { return anchor < caret ? anchor : caret; }
    TextPos end() const { return anchor < caret ? caret : anchor; }
    bool operator==(const Selection& o) const { return anchor == o.anchor && caret == o.caret; }
    bool operator!=(const Selection& o) const { return !(*this == o); }
};

struct Document {
    int id = 0;
    std::string title; // tab caption
    std::string path;  // empty for untitled documents
    TextBuffer buffer;
    std::string savedText; // text at the last load/save, for "modified" and compare-with-saved
    Language language = Language::PlainText;
    EolMode eol = EolMode::CrLf;
    Encoding encoding = Encoding::Utf8;
    bool modified = false;
    bool onDisk = false; // loaded from (and saved to) a real file under the workspace
    bool rendered = false; // Markdown shown formatted, CSV as a table (read-only); off: the text, to edit
    DataView dataView = DataView::Text; // JSON and YAML
    // What the file is: Text is loaded into `buffer`; an Image is shown, a Binary file is not
    // loaded (the buffer stays empty) until the user asks to open it as text anyway.
    FileKind kind = FileKind::Text;
    std::string format; // the detected format for Image and Binary ("PNG", "ELF executable")
    IndentStyle indent;          // spaces or tabs, and the width (also the tab stop)
    bool indentDetected = false; // `indent` came from the file's content
    mutable HighlightCache highlightCache; // lexer state per line, for drawing

    // The file as it was when loaded or saved (size, time), and whether it changed on disk since.
    enum class DiskState { InSync, Changed, Deleted };
    DiskState diskState = DiskState::InSync;
    bool hasDiskStamp = false;
    unsigned long long diskSize = 0;
    long long diskModified = 0;

    TextPos caret;
    TextPos anchor; // selection is [min(anchor, caret), max(anchor, caret))
    // Multi-cursor editing: the other carets (each with its selection) besides the primary one
    // above. Empty with a single caret. Kept sorted and without overlaps by normalizeCursors().
    std::vector<Selection> cursors;
    std::set<int> bookmarks;
    std::set<int> folded;   // start lines of folded outline regions
    int detailLevel = -1;   // detail level last applied to `folded` (-1: never)
    uint64_t revision = 0;  // bumped on every text change

    bool hasSelection() const { return anchor != caret; }
    bool multiCursor() const { return !cursors.empty(); }
    Selection primary() const { return {anchor, caret}; }
    // Every caret, the primary first, then the others in document order.
    std::vector<Selection> selections() const;
    TextPos selectionStart() const { return anchor < caret ? anchor : caret; }
    TextPos selectionEnd() const { return anchor < caret ? caret : anchor; }
};

// Clamps every caret into the buffer, merges carets that overlap or touch the same place (the
// primary absorbs the others it meets) and sorts the other carets by position.
void normalizeCursors(Document& doc);

} // namespace enp::model
