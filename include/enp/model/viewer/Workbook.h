// A spreadsheet once read (.xlsx): sheets of sparse rows of cells, each cell with the text it
// shows (its saved value, formatted) and its formula. Values are as saved; nothing is recalculated.
#pragma once

#include "enp/model/viewer/Page.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace enp::model::viewer {

struct Cell {
    enum Flags : std::uint8_t { Number = 1, Bold = 2, Italic = 4, Error = 8, Underline = 16 };
    std::uint32_t column = 0; // 0-based
    std::string text;         // as shown
    std::string formula;      // without the '='; empty for a constant
    std::uint8_t flags = 0;
    std::int8_t align = -1;   // -1 by type (numbers right), 0 left, 1 centre, 2 right
    std::uint32_t fill = 0;   // background (0: none), packed like Page colours
    std::uint32_t color = 0;  // text colour (0: default)
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
};

} // namespace enp::model::viewer
