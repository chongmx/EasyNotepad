#include "enp/model/Csv.h"

#include <algorithm>
#include <map>

namespace enp::model::csv {

std::string_view Table::cell(size_t row, int column) const
{
    if (row >= rowCount() || column < 0 || column >= cellCount(row))
        return {};
    const size_t i = rows[row] + size_t(column);
    return std::string_view(text).substr(cells[i], cells[i + 1] - cells[i]);
}

char detectDelimiter(std::string_view text, std::string_view path)
{
    if (path.size() >= 4 && (path.substr(path.size() - 4) == ".tsv" || path.substr(path.size() - 4) == ".TSV"))
        return '\t';
    // Count each candidate per line (outside quotes) over the first lines: the best one appears
    // the same number of times, more than zero, on most of them.
    const char candidates[] = {',', ';', '\t', '|'};
    char best = ',';
    double bestScore = 0.0;
    for (char d : candidates) {
        std::map<int, int> perLine; // count -> lines
        int lines = 0, count = 0;
        bool quoted = false;
        for (size_t i = 0; i < text.size() && lines < 50; ++i) {
            const char c = text[i];
            if (c == '"')
                quoted = !quoted;
            else if (c == d && !quoted)
                ++count;
            else if (c == '\n' && !quoted) {
                ++perLine[count];
                ++lines;
                count = 0;
            }
        }
        if (count > 0)
            ++perLine[count], ++lines;
        int modeCount = 0, modeLines = 0;
        for (const auto& [n, l] : perLine)
            if (n > 0 && l > modeLines)
                modeCount = n, modeLines = l;
        const double score = lines ? double(modeLines) / double(lines) * (1.0 + 0.01 * modeCount) : 0.0;
        if (modeCount > 0 && score > bestScore)
            best = d, bestScore = score;
    }
    return best;
}

Table parse(std::string_view text, char delimiter)
{
    Table t;
    t.delimiter = delimiter;
    t.text.reserve(text.size());
    t.cells.reserve(text.size() / 8 + 2);
    t.rows.push_back(0);
    size_t i = 0;
    const size_t n = text.size();
    bool rowHasContent = false;
    auto endRow = [&]() {
        const std::uint32_t cellsInRow = std::uint32_t(t.cells.size()) - t.rows.back();
        t.columns = std::max(t.columns, int(cellsInRow));
        t.rows.push_back(std::uint32_t(t.cells.size()));
        rowHasContent = false;
    };
    while (i < n) {
        // One field.
        t.cells.push_back(std::uint32_t(t.text.size()));
        rowHasContent = true;
        if (text[i] == '"') {
            ++i;
            while (i < n) {
                if (text[i] == '"') {
                    if (i + 1 < n && text[i + 1] == '"') {
                        t.text += '"';
                        i += 2;
                        continue;
                    }
                    ++i; // closing quote
                    break;
                }
                t.text += text[i++];
            }
            while (i < n && text[i] != delimiter && text[i] != '\n' && text[i] != '\r') // after the quote: kept as written
                t.text += text[i++];
        } else {
            while (i < n && text[i] != delimiter && text[i] != '\n' && text[i] != '\r')
                t.text += text[i++];
        }
        if (i >= n)
            break;
        if (text[i] == delimiter) {
            ++i;
            if (i >= n) { // a trailing delimiter: one more, empty cell
                t.cells.push_back(std::uint32_t(t.text.size()));
            }
            continue;
        }
        // End of the row (\n, \r\n or \r).
        if (text[i] == '\r' && i + 1 < n && text[i + 1] == '\n')
            ++i;
        ++i;
        endRow();
    }
    if (rowHasContent || t.cells.size() > t.rows.back())
        endRow();
    t.cells.push_back(std::uint32_t(t.text.size())); // the end of the last cell
    t.text.shrink_to_fit();
    t.cells.shrink_to_fit();
    t.rows.shrink_to_fit();
    return t;
}

} // namespace enp::model::csv
