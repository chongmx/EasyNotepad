// An open document: text, caret/selection and the file attributes shown in the status bar.
#pragma once

#include "enp/model/TextBuffer.h"

#include <set>
#include <string>
#include <string_view>

namespace enp::model {

enum class Language { PlainText, Cpp, Python, JavaScript, Markdown, Json, CMake };
enum class EolMode { CrLf, Lf, Cr };
enum class Encoding { Utf8, Utf8Bom, Utf16Le, Utf16Be, Ansi };

const char* languageName(Language language);
const char* eolName(EolMode eol);       // "Windows (CR LF)"
const char* eolShortName(EolMode eol);  // "CRLF"
const char* encodingName(Encoding encoding);
Language languageFromPath(std::string_view path);

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

    TextPos caret;
    TextPos anchor; // selection is [min(anchor, caret), max(anchor, caret))
    std::set<int> bookmarks;

    bool hasSelection() const { return anchor != caret; }
    TextPos selectionStart() const { return anchor < caret ? anchor : caret; }
    TextPos selectionEnd() const { return anchor < caret ? caret : anchor; }
};

} // namespace enp::model
