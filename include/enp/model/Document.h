// An open document: text, caret/selection and the file attributes shown in the status bar.
#pragma once

#include "enp/model/Highlighter.h"
#include "enp/model/Indent.h"
#include "enp/model/Language.h"
#include "enp/model/TextBuffer.h"

#include <cstdint>
#include <set>
#include <string>
#include <string_view>

namespace enp::model {

enum class EolMode { CrLf, Lf, Cr };
enum class Encoding { Utf8, Utf8Bom, Utf16Le, Utf16Be, Ansi };

const char* eolName(EolMode eol);       // "Windows (CR LF)"
const char* eolShortName(EolMode eol);  // "CRLF"
const char* encodingName(Encoding encoding);

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
    std::set<int> bookmarks;
    std::set<int> folded;   // start lines of folded outline regions
    int detailLevel = -1;   // detail level last applied to `folded` (-1: never)
    uint64_t revision = 0;  // bumped on every text change

    bool hasSelection() const { return anchor != caret; }
    TextPos selectionStart() const { return anchor < caret ? anchor : caret; }
    TextPos selectionEnd() const { return anchor < caret ? caret : anchor; }
};

} // namespace enp::model
