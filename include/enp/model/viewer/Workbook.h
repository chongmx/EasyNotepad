// A spreadsheet once read (.xlsx): sheets of sparse rows of cells, each cell with the text it
// shows (its saved value, formatted) and its formula. Values are as saved; nothing is recalculated.
#pragma once

#include "enp/model/viewer/Page.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace enp::model::viewer {

// A cell's four edges (left, right, top, bottom) and its diagonals, shared by the cells that use them.
struct Border {
    enum Style : std::uint8_t { None, Thin, Medium, Thick, Dashed, Dotted, Double, Hair };
    std::uint8_t style[5] = {None, None, None, None, None}; // [4]: the diagonal
    std::uint32_t color[5] = {0, 0, 0, 0, 0};               // 0: automatic (the text colour)
    bool diagonalUp = false, diagonalDown = false;          // which diagonals the [4] line draws
};

// A cell background other than one colour: a hatch pattern over `Cell::fill`, or a gradient.
struct FillStyle {
    enum Kind : std::uint8_t { Hatch, Linear, Path };
    // Hatches: lines across (horizontal), down (vertical), falling left to right (down), rising
    // (up), both straight ones (grid), both slanted ones (trellis); dark ones thicker and closer.
    enum Lines : std::uint8_t { Horizontal, Vertical, Down, Up, Grid, Trellis };
    Kind kind = Hatch;
    Lines lines = Horizontal;
    bool dark = false;
    std::uint32_t color = 0; // the hatch's lines
    float degree = 0.0f;     // Linear: the direction (0 left to right, 90 top to bottom)
    // Linear / Path: the colours at positions 0..1 (Path: from its centre out to the edges).
    std::uint8_t stopCount = 0;
    float position[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    std::uint32_t stop[4] = {0, 0, 0, 0};
};

// A part of a cell's text in its own font (rich text): bytes [begin, end) of the text.
struct CellRun {
    std::uint32_t begin = 0, end = 0;
    bool own = false;          // false: the cell's font; true: the font below
    std::uint8_t flags = 0;    // Cell::Bold / Italic / Underline / Strike
    std::uint32_t color = 0;   // 0: the default
    std::uint8_t halfPoints = 0; // 0: the cell's size
    std::uint8_t font = 0;       // into Workbook::fontNames, from 1 (0: the cell's)
};

struct Cell {
    enum Flags : std::uint8_t { Number = 1, Bold = 2, Italic = 4, Error = 8, Underline = 16, Wrap = 32, Strike = 64, Shrink = 128 };
    std::uint32_t column = 0; // 0-based
    std::string text;         // as shown
    std::string formula;      // without the '='; empty for a constant
    std::uint8_t flags = 0;
    std::int8_t align = -1;   // -1 by type (numbers right), 0 left, 1 centre, 2 right
    std::uint8_t indent = 0;  // levels in from the side it is aligned to
    std::uint8_t font = 0;    // into Workbook::fontNames, from 1 (0: the workbook's default font)
    std::uint16_t fillStyle = 0; // into Workbook::fills (0: `fill` alone)
    std::uint32_t fill = 0;   // background (0: none), packed like Page colours
    std::uint32_t color = 0;  // text colour (0: default)
    std::uint8_t halfPoints = 0; // font size in half points (0: the workbook's default)
    std::int8_t valign = -1;     // -1 bottom (Excel's default), 0 top, 1 centre
    std::int16_t rotation = 0;   // degrees anticlockwise (-90..90); 255: letters stacked downwards
    std::uint16_t border = 0;    // into Workbook::borders (0: none)
    std::uint32_t rich = 0;      // into Workbook::richTexts (0: one font)
};

struct Row {
    std::uint32_t index = 0; // 0-based
    float height = 0.0f;     // points; 0: the sheet's default
    std::vector<Cell> cells; // by column, ascending
};

struct Merge {
    std::uint32_t row0 = 0, col0 = 0, row1 = 0, col1 = 0; // inclusive
};

// A picture or chart over the cells: drawn as a page from cell (row0, col0) plus an offset to cell
// (row1, col1) plus an offset (points); the page is sized for the sheet's own column widths.
struct SheetDrawing {
    std::uint32_t row0 = 0, col0 = 0, row1 = 0, col1 = 0;
    float dx0 = 0.0f, dy0 = 0.0f, dx1 = 0.0f, dy1 = 0.0f;
    Page page;
};

struct Sheet {
    std::string name;
    std::vector<Row> rows; // ascending, only rows with cells or a height
    std::map<std::uint32_t, float> columnWidths; // in characters of the default font
    float defaultColumnWidth = 8.43f;
    float defaultRowHeight = 15.0f; // points
    std::uint32_t rowCount = 0, columnCount = 0; // the used range's extent
    std::vector<Merge> merges;
    std::uint32_t frozenRows = 0, frozenColumns = 0;
    bool hidden = false;
    bool showGrid = true;
    std::vector<SheetDrawing> drawings;

    const Row* row(std::uint32_t index) const;
    const Cell* cell(std::uint32_t row, std::uint32_t column) const;
    float columnWidth(std::uint32_t column) const;
};

struct Workbook {
    std::vector<Sheet> sheets;
    // Encoded pictures of the drawings, by the key their image items name.
    std::map<std::string, std::string> media;
    bool date1904 = false;
    float defaultFontSize = 11.0f;              // points: the size of the Normal style's font
    std::string defaultFont = "Calibri";        // the Normal style's font
    std::vector<std::string> fontNames;         // the other fonts cells and runs name
    std::vector<Border> borders;                // [0] is no border
    std::vector<FillStyle> fills;               // [0] is unused
    std::vector<std::vector<CellRun>> richTexts; // [0] is unused
};

} // namespace enp::model::viewer
