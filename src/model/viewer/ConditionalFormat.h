// Excel's conditional formatting, applied when the workbook is read: each rule is evaluated
// against the saved values (nothing is recalculated) and what it sets goes into the cells (fill,
// font colour and style, number format) or into the sheet's marks (data bars, icons).
#pragma once

#include "enp/model/viewer/Workbook.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::viewer::cf {

// "B12" or "$B$12" -> row 11, column 1. False when it is not a cell reference.
bool parseReference(std::string_view ref, std::uint32_t& row, std::uint32_t& column);

// "A1:B5 D2" -> its rectangles (a lone cell: a rectangle of one).
void parseRanges(std::string_view sqref, std::vector<Merge>& out);

// A differential format (styles.xml dxfs): only what it sets.
struct Dxf {
    std::uint8_t setFlags = 0, clearFlags = 0; // Cell::Bold / Italic / Underline / Strike
    bool hasColor = false, hasFill = false;
    std::uint32_t color = 0, fill = 0;
    std::string format; // a number format code ("": keeps the cell's)
};

// A number cell's saved value (the sheet's, in reading order: by row, then column).
struct Value {
    std::uint32_t row = 0, column = 0;
    double value = 0.0;
};

// A threshold of a colour scale, data bar or icon set: min, max, num, percent, percentile, formula.
struct Cfvo {
    std::string type, val;
    bool gte = true; // an icon's threshold: at least (else above)
};

struct Rule {
    std::vector<Merge> ranges;
    std::string type, op, text, iconSet;
    std::vector<std::string> formulas;
    std::vector<Cfvo> cfvos;
    std::vector<std::uint32_t> colors; // the colour scale's, or the data bar's one
    int dxf = -1, priority = 0, rank = 10, stdDev = 0;
    bool stop = false, percent = false, bottom = false, above = true, equal = false, reverse = false, showValue = true;
};

// Applies `rules` (in their priority order: a property set by one is kept from the rules after it,
// stopIfTrue ends a cell's rules) to `sheet`. Returns the number of rules it cannot evaluate
// (formulas other than constants and cell references, time periods).
int apply(std::vector<Rule>& rules, const std::vector<Dxf>& dxfs, const std::vector<Value>& values, Sheet& sheet, bool date1904);

} // namespace enp::model::viewer::cf
