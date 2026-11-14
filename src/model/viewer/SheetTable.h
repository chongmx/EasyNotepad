// Excel tables ("Format as Table", xl/tables/*.xml): the table's style, built in (Light, Medium,
// Dark 1-28, made from the theme's accents as Excel makes them) or the workbook's own, laid onto
// its cells when the workbook is read. A cell's own fill, colour and borders win over the style's.
#pragma once

#include "enp/model/viewer/Workbook.h"

#include "Ooxml.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace enp::model::viewer::table {

// What one part of a style sets; edges: left, right, top, bottom (the part's outline), then the
// lines inside it (between rows, between columns).
struct Part {
    bool hasFill = false, hasColor = false, bold = false;
    std::uint32_t fill = 0, color = 0;
    std::uint8_t edge[6] = {Border::None, Border::None, Border::None, Border::None, Border::None, Border::None};
    std::uint32_t edgeColor[6] = {0, 0, 0, 0, 0, 0};
};

enum Element { Whole, ColumnStripe1, ColumnStripe2, RowStripe1, RowStripe2, FirstColumn, LastColumn, Header, Total, ElementCount };

struct Style {
    Part parts[ElementCount];
};

// The element named in a tableStyleElement's type ("wholeTable", "headerRow"...); ElementCount when not one of these.
Element elementOf(std::string_view type);

// A built-in style by its name ("TableStyleMedium2"). False when the name is not one.
bool builtin(std::string_view name, const ooxml::Theme& theme, Style& out);

struct Table {
    Merge ref;
    std::uint32_t headerRows = 1, totalRows = 0;
    bool firstColumn = false, lastColumn = false, rowStripes = true, columnStripes = false;
};

// Lays `style` onto the table's cells (adding the empty ones it colours).
void apply(const Table& table, const Style& style, Workbook& book, Sheet& sheet);

} // namespace enp::model::viewer::table
