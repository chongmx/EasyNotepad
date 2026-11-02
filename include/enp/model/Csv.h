// CSV and TSV read into a table (RFC 4180: quoted fields, "" inside quotes, line breaks inside
// quotes), with the delimiter detected from the text. Every cell lives in one string with offsets,
// so a table of a million cells is a handful of allocations, not a million.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::csv {

struct Table {
    std::string text;                // every cell's text, back to back, quotes removed
    std::vector<std::uint32_t> cells; // start of each cell in `text`; one extra entry at the end
    std::vector<std::uint32_t> rows;  // index of each row's first cell; one extra entry at the end
    int columns = 0;                  // cells in the widest row
    char delimiter = ',';

    size_t rowCount() const { return rows.empty() ? 0 : rows.size() - 1; }
    int cellCount(size_t row) const { return int(rows[row + 1] - rows[row]); }
    // Empty beyond the end of a short row.
    std::string_view cell(size_t row, int column) const;
};

// ',' ';' '\t' or '|': the one that splits the first lines most consistently ('\t' for .tsv).
char detectDelimiter(std::string_view text, std::string_view path = {});
Table parse(std::string_view text, char delimiter);

} // namespace enp::model::csv
