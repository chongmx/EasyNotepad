// Code on a grid of cells, as a terminal shows it: every character one cell wide (two for wide ones
// and emoji), so ASCII art and box drawing line up. Used by rendered Markdown's code blocks.
//
// Box drawing (U+2500-U+257F) and block elements (U+2580-U+259F) are drawn as lines and
// rectangles that fill their cell, the way terminals draw them. From a font they come out wrong on
// such a grid: the bundled monospace font has none, the fallback is proportional (narrower or
// wider than a cell), and a glyph only spans the font's height, so lines break between rows that
// are taller. Drawn here they meet their neighbours on every side.
#pragma once

#include "enp/model/Emoji.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <string>

namespace enp::view {

// Whether `c` is drawn by drawBoxCharacter() instead of from a font.
bool isBoxCharacter(uint32_t c);

// Draws `c` filling the cell [min, max). `lineWidth` is a light line's width in pixels (heavy
// lines are wider). Returns false (nothing drawn) when `c` is not a box character.
bool drawBoxCharacter(ImDrawList* dl, uint32_t c, ImVec2 min, ImVec2 max, ImU32 color, float lineWidth);

// Columns `c` takes on the grid: 2 for East Asian wide characters, else 1.
int cellColumns(uint32_t c);

// The code point at text[i] and its length in bytes (malformed: U+FFFD, one byte). All of Unicode:
// ImGui's own decoder gives U+FFFD above U+FFFF with its 16-bit ImWchar.
uint32_t decodeUtf8(const std::string& text, size_t i, int* length);

// One character of a line of code, placed on the grid.
struct CodeCell {
    size_t byte;              // where it starts in the line
    int length;               // its UTF-8 bytes (an emoji: the whole sequence)
    uint32_t c;               // the character (an emoji: its first)
    int column, row;          // its first cell; the row within the line (a wrapped line takes several)
    int cells;                // 1, 2 for a wide character or an emoji, a tab's to the next stop, 0 for '\r'
    const std::string* emoji; // an emoji's picture key (model::emoji::match), else null
};

// Lays a line of code out `columns` cells wide: tabs to the next multiple of 4, and a line too
// long wraps at a whole character. Calls `visit` for each character; returns the rows the line
// takes (at least 1). `key` holds an emoji's key while it is visited (the caller's: no allocation
// per frame).
template <typename Visit>
int layoutCodeLine(const std::string& line, int columns, std::string& key, Visit&& visit)
{
    int column = 0, row = 0;
    for (size_t i = 0; i < line.size();) {
        int length = 1;
        const uint32_t c = decodeUtf8(line, i, &length);
        // An emoji sequence (a keycap starts with an ASCII digit, # or *, then non-ASCII bytes).
        const bool maybeEmoji = c >= 0x80 || (i + 1 < line.size() && static_cast<unsigned char>(line[i + 1]) >= 0x80);
        const size_t emojiLength = maybeEmoji ? model::emoji::match(line, i, &key) : 0;
        const size_t invisible = maybeEmoji && emojiLength == 0 ? model::emoji::invisible(line, i) : 0;
        if (emojiLength > 0 || invisible > 0)
            length = int(emojiLength > 0 ? emojiLength : invisible);
        int cells = emojiLength > 0 ? 2 : invisible > 0 || c == '\r' ? 0 : c == '\t' ? 4 - column % 4 : cellColumns(c);
        if (column + cells > columns && column > 0) {
            ++row;
            column = 0;
            if (c == '\t')
                cells = 4;
        }
        cells = std::min(cells, columns);
        visit(CodeCell{i, length, c, column, row, cells, emojiLength > 0 ? &key : nullptr});
        column += cells;
        i += size_t(length);
    }
    return row + 1;
}

} // namespace enp::view
