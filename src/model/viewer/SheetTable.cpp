// Excel's table styles: the built-in ones from the theme's accents, laid onto a table's cells.
#include "SheetTable.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace enp::model::viewer::table {

namespace {

void edges(Part& p, std::initializer_list<int> which, std::uint8_t style, std::uint32_t color)
{
    for (int i : which) {
        p.edge[i] = style;
        p.edgeColor[i] = color;
    }
}

void fill(Part& p, std::uint32_t color)
{
    p.hasFill = true;
    p.fill = color;
}

void text(Part& p, std::uint32_t color, bool bold)
{
    p.hasColor = true;
    p.color = color;
    p.bold = bold;
}

enum { L, R, T, B, H, V }; // a part's edges: its outline, then the lines inside it

Cell& cellAt(Sheet& sheet, std::uint32_t r, std::uint32_t c)
{
    auto row = std::lower_bound(sheet.rows.begin(), sheet.rows.end(), r, [](const Row& x, std::uint32_t i) { return x.index < i; });
    if (row == sheet.rows.end() || row->index != r) {
        Row added;
        added.index = r;
        row = sheet.rows.insert(row, std::move(added));
    }
    auto cell = std::lower_bound(row->cells.begin(), row->cells.end(), c, [](const Cell& x, std::uint32_t i) { return x.column < i; });
    if (cell == row->cells.end() || cell->column != c) {
        Cell added;
        added.column = c;
        cell = row->cells.insert(cell, std::move(added));
    }
    sheet.rowCount = std::max(sheet.rowCount, r + 1);
    sheet.columnCount = std::max(sheet.columnCount, c + 1);
    return *cell;
}

bool same(const Border& a, const Border& b)
{
    return std::memcmp(a.style, b.style, sizeof(a.style)) == 0 && std::memcmp(a.color, b.color, sizeof(a.color)) == 0 &&
           a.diagonalUp == b.diagonalUp && a.diagonalDown == b.diagonalDown;
}

} // namespace

Element elementOf(std::string_view type)
{
    return type == "wholeTable" ? Whole : type == "headerRow" ? Header : type == "totalRow" ? Total : type == "firstColumn" ? FirstColumn
         : type == "lastColumn" ? LastColumn : type == "firstRowStripe" ? RowStripe1 : type == "secondRowStripe" ? RowStripe2
         : type == "firstColumnStripe" ? ColumnStripe1 : type == "secondColumnStripe" ? ColumnStripe2 : ElementCount;
}

bool builtin(std::string_view name, const ooxml::Theme& theme, Style& out)
{
    constexpr std::string_view kPrefix = "TableStyle";
    if (name.substr(0, kPrefix.size()) != kPrefix)
        return false;
    name.remove_prefix(kPrefix.size());
    int family = -1; // light, medium, dark
    for (const auto& [word, f] : {std::pair<std::string_view, int>{"Light", 0}, {"Medium", 1}, {"Dark", 2}})
        if (name.substr(0, word.size()) == word) {
            family = f;
            name.remove_prefix(word.size());
        }
    const int n = std::atoi(std::string(name).c_str());
    if (family < 0 || n < 1 || n > (family == 0 ? 21 : family == 1 ? 28 : 11))
        return false;
    // Seven of a kind, one for black and one for each accent (Dark 8-11: black and the accent pairs).
    int group = (n - 1) / 7, k = (n - 1) % 7;
    if (family == 2 && n >= 8) {
        group = 1;
        k = n == 8 ? 0 : (n - 9) * 2 + 1;
    }
    static const char* const kAccents[] = {"dk1", "accent1", "accent2", "accent3", "accent4", "accent5", "accent6"};
    const std::uint32_t black = theme.color("dk1", rgba(0, 0, 0)), white = rgba(255, 255, 255);
    const std::uint32_t a = theme.color(kAccents[k], k == 0 ? black : rgba(0x44, 0x72, 0xC4));
    auto tint = [&](double t) { return ooxml::applyTint(k == 0 ? rgba(0, 0, 0) : a, k == 0 && t > 0.0 ? std::min(0.95, t + 0.05) : t); };
    out = Style();
    Part& whole = out.parts[Whole];
    Part& header = out.parts[Header];
    Part& total = out.parts[Total];
    Part& band = out.parts[RowStripe1];
    Part& columnBand = out.parts[ColumnStripe1];
    Part& first = out.parts[FirstColumn];
    Part& last = out.parts[LastColumn];
    first.bold = last.bold = true;
    total.bold = true;
    if (family == 0 && group == 0) { // Light 1-7: lines above and below, banded rows, text in the accent
        edges(whole, {T, B}, Border::Thin, a);
        if (k != 0)
            text(whole, ooxml::applyTint(a, -0.25), false);
        header.bold = true;
        edges(header, {B}, Border::Thin, a);
        edges(total, {T}, Border::Thin, a);
        fill(band, tint(0.8));
        fill(columnBand, tint(0.8));
    } else if (family == 0 && group == 1) { // Light 8-14: an outline, a solid header
        edges(whole, {L, R, T, B}, Border::Thin, a);
        fill(header, a);
        text(header, white, true);
        edges(total, {T}, Border::Double, a);
        edges(band, {T, B}, Border::Thin, a);
        edges(columnBand, {L, R}, Border::Thin, a);
    } else if (family == 0) { // Light 15-21: every line, banded rows
        edges(whole, {L, R, T, B, H, V}, Border::Thin, a);
        header.bold = true;
        edges(total, {T}, Border::Double, a);
        fill(band, tint(0.8));
        fill(columnBand, tint(0.8));
    } else if (family == 1 && group == 0) { // Medium 1-7: a solid header, light lines, banded rows
        edges(whole, {L, R, T, B, H}, Border::Thin, tint(0.4));
        fill(header, a);
        text(header, white, true);
        edges(total, {T}, Border::Double, a);
        fill(band, tint(0.8));
        fill(columnBand, tint(0.8));
    } else if (family == 1 && group == 1) { // Medium 8-14 (Dark 8-11 below): shaded, white lines
        fill(whole, tint(0.8));
        edges(whole, {H, V}, Border::Thin, white);
        fill(header, a);
        text(header, white, true);
        edges(header, {B}, Border::Thick, white);
        fill(total, a);
        text(total, white, true);
        edges(total, {T}, Border::Thick, white);
        fill(band, tint(0.6));
        fill(columnBand, tint(0.6));
        fill(first, a);
        text(first, white, true);
        fill(last, a);
        text(last, white, true);
    } else if (family == 1 && group == 2) { // Medium 15-21: a solid header, black lines, banded rows
        edges(whole, {L, R, T, B, H, V}, Border::Thin, black);
        fill(header, a);
        text(header, white, true);
        edges(header, {B}, Border::Medium, black);
        edges(total, {T}, Border::Double, black);
        fill(band, tint(0.8));
        fill(columnBand, tint(0.8));
    } else if (family == 1) { // Medium 22-28: shaded with light lines, a plain bold header
        fill(whole, tint(0.8));
        edges(whole, {L, R, T, B, H, V}, Border::Thin, tint(0.4));
        header.bold = true;
        edges(total, {T}, Border::Double, a);
        fill(band, tint(0.6));
        fill(columnBand, tint(0.6));
    } else { // Dark 1-7: dark rows with white text, a black header
        fill(whole, ooxml::applyTint(k == 0 ? ooxml::hexColor("737373") : a, -0.25));
        text(whole, white, false);
        fill(header, black);
        text(header, white, true);
        edges(header, {B}, Border::Medium, white);
        fill(total, ooxml::applyTint(k == 0 ? ooxml::hexColor("737373") : a, -0.5));
        text(total, white, true);
        edges(total, {T}, Border::Double, white);
        fill(band, ooxml::applyTint(k == 0 ? ooxml::hexColor("737373") : a, -0.5));
        fill(columnBand, ooxml::applyTint(k == 0 ? ooxml::hexColor("737373") : a, -0.5));
        fill(first, ooxml::applyTint(k == 0 ? ooxml::hexColor("737373") : a, -0.5));
        fill(last, ooxml::applyTint(k == 0 ? ooxml::hexColor("737373") : a, -0.5));
    }
    if (family == 2 && n >= 8) { // Dark 8-11: as Medium 8-14 under a black header
        fill(header, black);
        text(header, white, true);
    }
    return true;
}

void apply(const Table& table, const Style& style, Workbook& book, Sheet& sheet)
{
    const Merge& m = table.ref;
    const std::uint64_t rows = std::uint64_t(m.row1 - m.row0) + 1, columns = std::uint64_t(m.col1 - m.col0) + 1;
    if (m.row1 < m.row0 || m.col1 < m.col0 || rows * columns > 200000)
        return;
    const std::uint32_t headerRows = std::min<std::uint32_t>(table.headerRows, std::uint32_t(rows));
    const std::uint32_t totalRows = std::min<std::uint32_t>(table.totalRows, std::uint32_t(rows) - headerRows);
    const std::uint32_t bodyTop = m.row0 + headerRows;
    const std::uint32_t bodyEnd = m.row1 + 1 - totalRows; // one past the body's last row
    std::vector<std::pair<Border, std::uint16_t>> interned; // the borders this table added, to share
    for (std::uint32_t r = m.row0; r <= m.row1; ++r)
        for (std::uint32_t c = m.col0; c <= m.col1; ++c) {
            Part acc;
            std::uint8_t edge[4] = {Border::None, Border::None, Border::None, Border::None};
            std::uint32_t edgeColor[4] = {0, 0, 0, 0};
            // A part over rows [r0, r1] and columns [c0, c1]: its outline on the cells at its sides,
            // its inside lines between the others.
            auto lay = [&](const Part& p, std::uint32_t r0, std::uint32_t r1, std::uint32_t c0, std::uint32_t c1) {
                if (p.hasFill) {
                    acc.hasFill = true;
                    acc.fill = p.fill;
                }
                if (p.hasColor) {
                    acc.hasColor = true;
                    acc.color = p.color;
                }
                acc.bold = acc.bold || p.bold;
                const int from[4] = {c == c0 ? L : V, c == c1 ? R : V, r == r0 ? T : H, r == r1 ? B : H};
                for (int i = 0; i < 4; ++i)
                    if (p.edge[from[i]] != Border::None) {
                        edge[i] = p.edge[from[i]];
                        edgeColor[i] = p.edgeColor[from[i]];
                    }
            };
            const bool body = r >= bodyTop && r < bodyEnd;
            lay(style.parts[Whole], m.row0, m.row1, m.col0, m.col1);
            if (table.columnStripes && body)
                lay(style.parts[(c - m.col0) % 2 == 0 ? ColumnStripe1 : ColumnStripe2], bodyTop, bodyEnd - 1, c, c);
            if (table.rowStripes && body)
                lay(style.parts[(r - bodyTop) % 2 == 0 ? RowStripe1 : RowStripe2], r, r, m.col0, m.col1);
            if (table.firstColumn && c == m.col0)
                lay(style.parts[FirstColumn], m.row0, m.row1, c, c);
            if (table.lastColumn && c == m.col1)
                lay(style.parts[LastColumn], m.row0, m.row1, c, c);
            if (r < bodyTop)
                lay(style.parts[Header], m.row0, bodyTop - 1, m.col0, m.col1);
            if (r >= bodyEnd)
                lay(style.parts[Total], bodyEnd, m.row1, m.col0, m.col1);
            const bool anyEdge = edge[0] || edge[1] || edge[2] || edge[3];
            if (!acc.hasFill && !acc.hasColor && !acc.bold && !anyEdge)
                continue;
            Cell& cell = cellAt(sheet, r, c);
            if (acc.hasFill && cell.fill == 0 && cell.fillStyle == 0)
                cell.fill = acc.fill;
            if (acc.hasColor && cell.color == 0)
                cell.color = acc.color == rgba(0, 0, 0) ? 0 : acc.color;
            if (acc.bold)
                cell.flags |= Cell::Bold;
            if (!anyEdge)
                continue;
            Border b = cell.border && cell.border < book.borders.size() ? book.borders[cell.border] : Border();
            for (int i = 0; i < 4; ++i)
                if (b.style[i] == Border::None && edge[i] != Border::None) {
                    b.style[i] = edge[i];
                    b.color[i] = edgeColor[i];
                }
            const auto known = std::find_if(interned.begin(), interned.end(), [&](const auto& x) { return same(x.first, b); });
            if (known != interned.end()) {
                cell.border = known->second;
            } else if (book.borders.size() < 65535) {
                if (book.borders.empty())
                    book.borders.emplace_back(); // [0]: none
                cell.border = std::uint16_t(book.borders.size());
                book.borders.push_back(b);
                if (interned.size() < 256)
                    interned.emplace_back(b, cell.border);
            }
        }
}

} // namespace enp::model::viewer::table
