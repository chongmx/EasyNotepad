// Spreadsheet number formats: a cell's saved value shown as its format code says ("0.00",
// "#,##0", "0%", "d mmm yyyy", "h:mm AM/PM", sections for negatives and zero, quoted text,
// [$EUR] currency tags). The common codes, not every corner of Excel's grammar.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace enp::model::viewer {

// The code of a built-in format id (0-49), or "" when the id is not one of them.
std::string_view builtinNumberFormat(int id);

// `value` shown with `code` ("General" or empty: up to 11 significant digits). A colour tag of the
// section used ([Red], [Blue], [Color3]) goes to `color` (packed like Page colours) when given.
std::string formatNumber(double value, std::string_view code, bool date1904 = false, std::uint32_t* color = nullptr);

// Whether `code` shows dates or times (so a serial number is a date).
bool isDateFormat(std::string_view code);

} // namespace enp::model::viewer
