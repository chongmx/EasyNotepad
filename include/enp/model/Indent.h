// Indentation: a document's style (spaces or tabs, and the width), detecting it from the text,
// and smart indentation driven by the language table: what Enter inserts, and the indent a line
// should take once it starts with a closing token (}, </tag>, endif, end, fi, else:).
#pragma once

#include "enp/model/Language.h"
#include "enp/model/TextBuffer.h"

#include <optional>
#include <string>
#include <string_view>

namespace enp::model {

struct IndentStyle {
    bool tabs = false;
    int width = 4; // spaces per level; also the tab stop
    bool operator==(const IndentStyle& o) const { return tabs == o.tabs && width == o.width; }
};

std::string indentUnit(IndentStyle style);                 // "\t" or `width` spaces
std::string leadingWhitespace(std::string_view line);
int indentColumns(std::string_view whitespace, int tabWidth); // visual width
std::string makeIndent(int columns, IndentStyle style);     // tabs then spaces, or spaces
// Removes one level from an indent string.
std::string outdented(const std::string& indent, IndentStyle style);

// The file's own style: tabs if most indented lines start with one, else the most common step
// between consecutive lines' indents (2, 3, 4 or 8). nullopt when nothing is indented.
std::optional<IndentStyle> detectIndent(const TextBuffer& buffer);
IndentStyle defaultIndent(const LanguageSpec& spec);

// What pressing Enter at `caret` does: delete `eraseBefore` blank characters before the caret
// and `eraseAfter` after it, insert `text`, then put the caret `caretOffset` bytes into `text`.
struct NewlineEdit {
    int eraseBefore = 0;
    int eraseAfter = 0;
    std::string text;
    int caretOffset = 0;
};
NewlineEdit smartNewline(const TextBuffer& buffer, TextPos caret, const LanguageSpec& spec, IndentStyle style);

// The indent `line` should have now that its text starts with a closing token, or nullopt when
// it does not start with one (or no matching opener is found).
std::optional<std::string> closingIndent(const TextBuffer& buffer, int line, const LanguageSpec& spec, IndentStyle style);

// True when typing `c` can complete a closing token for the language (worth re-indenting for).
bool closesIndent(char c, const LanguageSpec& spec);

} // namespace enp::model
