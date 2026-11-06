// Excel (.xlsx): the workbook's sheets, shared strings and cell formats into the Workbook model.
// Each cell keeps its saved value formatted by its number format, its formula's text, and its
// font weight, colours and alignment. Values are as saved: nothing is recalculated.
#include "enp/model/viewer/NumberFormat.h"
#include "enp/model/viewer/Viewer.h"

#include "Ooxml.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace enp::model::viewer {

const Row* Sheet::row(std::uint32_t index) const
{
    const auto it = std::lower_bound(rows.begin(), rows.end(), index, [](const Row& r, std::uint32_t i) { return r.index < i; });
    return it != rows.end() && it->index == index ? &*it : nullptr;
}

const Cell* Sheet::cell(std::uint32_t r, std::uint32_t c) const
{
    const Row* rw = row(r);
    if (!rw)
        return nullptr;
    const auto it = std::lower_bound(rw->cells.begin(), rw->cells.end(), c, [](const Cell& cell, std::uint32_t i) { return cell.column < i; });
    return it != rw->cells.end() && it->column == c ? &*it : nullptr;
}

float Sheet::columnWidth(std::uint32_t column) const
{
    const auto it = columnWidths.find(column);
    return it == columnWidths.end() ? defaultColumnWidth : it->second;
}

namespace {

using ooxml::XmlReader;
using Event = XmlReader::Event;

struct CellFormat {
    std::string code; // number format code
    bool date = false;
    std::uint8_t flags = 0; // Cell::Bold / Italic / Underline
    std::uint32_t color = 0, fill = 0;
    std::int8_t align = -1;
};

struct Font {
    std::uint8_t flags = 0;
    std::uint32_t color = 0;
};

// "B12" -> row 11, column 1. False when it is not a cell reference.
bool parseReference(std::string_view ref, std::uint32_t& row, std::uint32_t& column)
{
    std::size_t i = 0;
    std::uint64_t col = 0;
    while (i < ref.size() && ref[i] == '$')
        ++i;
    const std::size_t letters = i;
    while (i < ref.size() && std::isalpha(static_cast<unsigned char>(ref[i]))) {
        col = col * 26 + std::uint64_t(std::toupper(static_cast<unsigned char>(ref[i])) - 'A' + 1);
        ++i;
        if (col > 16384)
            return false;
    }
    if (i == letters)
        return false;
    while (i < ref.size() && ref[i] == '$')
        ++i;
    std::uint64_t r = 0;
    const std::size_t digits = i;
    while (i < ref.size() && std::isdigit(static_cast<unsigned char>(ref[i]))) {
        r = r * 10 + std::uint64_t(ref[i] - '0');
        ++i;
        if (r > 1048576)
            return false;
    }
    if (i == digits || i != ref.size() || r == 0)
        return false;
    row = std::uint32_t(r - 1);
    column = std::uint32_t(col - 1);
    return true;
}

class XlsxReader {
public:
    explicit XlsxReader(ooxml::ZipArchive& zip) : zip_(zip) {}

    ViewerDocument read()
    {
        ViewerDocument doc;
        doc.format = "Excel workbook";
        const std::string main = ooxml::mainPart(zip_, "xl/workbook.xml");
        const std::optional<std::string> xml = zip_.read(main);
        if (!xml) {
            doc.error = "the workbook part (" + main + ") is missing";
            return doc;
        }
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, main);
        theme_ = ooxml::readTheme(zip_, ooxml::targetOfType(rels, "theme"));
        readSharedStrings(ooxml::targetOfType(rels, "sharedStrings"));
        readStyles(ooxml::targetOfType(rels, "styles"));

        struct SheetRef {
            std::string name, part;
            bool hidden = false;
        };
        std::vector<SheetRef> refs;
        XmlReader x(*xml);
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start)
                continue;
            if (x.localName() == "workbookPr") {
                const std::string d = x.attribute("date1904");
                doc.book.date1904 = d == "1" || d == "true";
            } else if (x.localName() == "sheet") {
                const auto it = rels.find(x.attribute("r:id"));
                if (it == rels.end())
                    continue;
                if (it->second.type != "worksheet") { // a chart sheet or a dialog sheet
                    ++otherSheets_;
                    continue;
                }
                const std::string state = x.attribute("state");
                refs.push_back({x.attribute("name"), it->second.target, state == "hidden" || state == "veryHidden"});
            }
        }
        date1904_ = doc.book.date1904;
        for (const SheetRef& ref : refs) {
            Sheet sheet;
            sheet.name = ref.name;
            sheet.hidden = ref.hidden;
            readSheet(ref.part, sheet);
            doc.book.sheets.push_back(std::move(sheet));
        }
        if (doc.book.sheets.empty()) {
            doc.error = "the workbook has no worksheets";
            return doc;
        }
        if (otherSheets_)
            doc.notShown.push_back(std::to_string(otherSheets_) + (otherSheets_ == 1 ? " chart sheet" : " chart sheets"));
        if (charts_)
            doc.notShown.push_back(std::to_string(charts_) + (charts_ == 1 ? " chart or drawing" : " charts or drawings"));
        if (truncated_)
            doc.notShown.push_back("cells beyond the first 2,000,000");
        doc.shape = Shape::Sheet;
        return doc;
    }

private:
    void readSharedStrings(const std::string& part)
    {
        if (part.empty())
            return;
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return;
        XmlReader x(*xml);
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start || x.localName() != "si")
                continue;
            strings_.push_back(richText(x));
        }
    }

    // At the Start of si / is: its text runs, without phonetic hints (rPh).
    static std::string richText(XmlReader& x)
    {
        std::string out;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            if (x.localName() == "rPh" || x.localName() == "phoneticPr")
                x.skip();
            else if (x.localName() == "t")
                out += x.elementText();
        }
        return out;
    }

    std::uint32_t colorOf(const XmlReader& x) const
    {
        std::uint32_t c = 0;
        if (x.hasAttribute("rgb")) {
            c = ooxml::hexColor(x.attribute("rgb"));
        } else if (x.hasAttribute("theme")) {
            // Excel's theme indexes: 0 lt1, 1 dk1, 2 lt2, 3 dk2, then the accents.
            static const char* const kNames[] = {"lt1", "dk1", "lt2", "dk2", "accent1", "accent2", "accent3",
                                                 "accent4", "accent5", "accent6", "hlink", "folHlink"};
            const int i = std::atoi(x.attribute("theme").c_str());
            if (i >= 0 && i < 12)
                c = theme_.color(kNames[i]);
        } else if (x.hasAttribute("indexed")) {
            static const char* const kIndexed[] = {"000000", "FFFFFF", "FF0000", "00FF00", "0000FF", "FFFF00", "FF00FF", "00FFFF"};
            const int i = std::atoi(x.attribute("indexed").c_str());
            if (i >= 0 && i < 8)
                c = ooxml::hexColor(kIndexed[i]);
            else if (i >= 8 && i < 16)
                c = ooxml::hexColor(kIndexed[i - 8]);
        }
        if (c && x.hasAttribute("tint"))
            c = ooxml::applyTint(c, std::atof(x.attribute("tint").c_str()));
        return c;
    }

    void readStyles(const std::string& part)
    {
        if (part.empty())
            return;
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return;
        std::map<int, std::string> codes;
        std::vector<Font> fonts;
        std::vector<std::uint32_t> fills;
        XmlReader x(*xml);
        enum class In { None, Fonts, Fills, CellXfs, Other } in = In::None;
        bool solid = false;
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                const std::string_view n = x.localName();
                if (n == "fonts" || n == "fills" || n == "cellXfs" || n == "cellStyleXfs" || n == "dxfs")
                    in = In::None;
                continue;
            }
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "numFmt") {
                codes[std::atoi(x.attribute("numFmtId").c_str())] = x.attribute("formatCode");
            } else if (n == "fonts") {
                in = In::Fonts;
            } else if (n == "fills") {
                in = In::Fills;
            } else if (n == "cellXfs") {
                in = In::CellXfs;
            } else if (n == "cellStyleXfs" || n == "dxfs") {
                in = In::Other;
            } else if (in == In::Fonts && n == "font") {
                fonts.emplace_back();
            } else if (in == In::Fonts && !fonts.empty()) {
                const std::string v = x.attribute("val");
                const bool set = v.empty() || (v != "0" && v != "false" && v != "none");
                if (n == "b" && set)
                    fonts.back().flags |= Cell::Bold;
                else if (n == "i" && set)
                    fonts.back().flags |= Cell::Italic;
                else if (n == "u" && set)
                    fonts.back().flags |= Cell::Underline;
                else if (n == "color")
                    fonts.back().color = colorOf(x);
            } else if (in == In::Fills && n == "fill") {
                fills.push_back(0);
            } else if (in == In::Fills && n == "patternFill" && !fills.empty()) {
                solid = x.attribute("patternType") == "solid";
            } else if (in == In::Fills && n == "fgColor" && !fills.empty() && solid) {
                fills.back() = colorOf(x);
            } else if (in == In::CellXfs && n == "xf") {
                CellFormat f;
                const int numFmt = std::atoi(x.attribute("numFmtId").c_str());
                const auto custom = codes.find(numFmt);
                f.code = custom != codes.end() ? custom->second : std::string(builtinNumberFormat(numFmt));
                f.date = isDateFormat(f.code);
                const int font = std::atoi(x.attribute("fontId").c_str());
                if (font >= 0 && font < int(fonts.size())) {
                    f.flags = fonts[size_t(font)].flags;
                    f.color = fonts[size_t(font)].color;
                    if (f.color == theme_.color("dk1") || f.color == ooxml::hexColor("000000"))
                        f.color = 0; // the default: the view picks a readable colour
                }
                const int fill = std::atoi(x.attribute("fillId").c_str());
                if (fill >= 2 && fill < int(fills.size())) // 0 and 1 are reserved (none, gray125)
                    f.fill = fills[size_t(fill)];
                formats_.push_back(std::move(f));
            } else if (in == In::CellXfs && n == "alignment" && !formats_.empty()) {
                const std::string h = x.attribute("horizontal");
                formats_.back().align = h == "center" || h == "centerContinuous" ? 1 : h == "right" ? 2 : h == "left" ? 0 : -1;
            }
        }
    }

    void readSheet(const std::string& part, Sheet& sheet)
    {
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return;
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, part);
        for (const auto& [id, r] : rels)
            if (r.type == "drawing")
                ++charts_;
        XmlReader x(*xml);
        Row* row = nullptr;
        std::uint32_t nextRow = 0, nextColumn = 0;
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                if (x.localName() == "row")
                    row = nullptr;
                continue;
            }
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "sheetFormatPr") {
                if (x.hasAttribute("defaultRowHeight"))
                    sheet.defaultRowHeight = std::max(1.0f, float(std::atof(x.attribute("defaultRowHeight").c_str())));
                if (x.hasAttribute("defaultColWidth"))
                    sheet.defaultColumnWidth = std::max(0.5f, float(std::atof(x.attribute("defaultColWidth").c_str())));
                else if (x.hasAttribute("baseColWidth"))
                    sheet.defaultColumnWidth = float(std::atof(x.attribute("baseColWidth").c_str())) + 0.71f;
            } else if (n == "sheetView") {
                sheet.showGrid = x.attribute("showGridLines") != "0" && x.attribute("showGridLines") != "false";
            } else if (n == "pane") {
                const std::string state = x.attribute("state");
                if (state == "frozen" || state == "frozenSplit") {
                    sheet.frozenColumns = std::uint32_t(std::max(0, std::atoi(x.attribute("xSplit").c_str())));
                    sheet.frozenRows = std::uint32_t(std::max(0, std::atoi(x.attribute("ySplit").c_str())));
                }
            } else if (n == "col") {
                const int lo = std::max(1, std::atoi(x.attribute("min").c_str()));
                const int hi = std::min(16384, std::atoi(x.attribute("max").c_str()));
                const bool hidden = x.attribute("hidden") == "1" || x.attribute("hidden") == "true";
                const float width = hidden ? 0.0f : float(std::atof(x.attribute("width").c_str()));
                if (hi - lo < 1024 || width != sheet.defaultColumnWidth) // a range over the whole sheet is the default
                    for (int c = lo; c <= hi && c - lo < 16384; ++c)
                        sheet.columnWidths[std::uint32_t(c - 1)] = width;
            } else if (n == "row") {
                std::uint32_t r = nextRow;
                if (x.hasAttribute("r"))
                    r = std::uint32_t(std::max(1, std::atoi(x.attribute("r").c_str())) - 1);
                nextRow = r + 1;
                nextColumn = 0;
                if (!sheet.rows.empty() && sheet.rows.back().index >= r) { // out of order: ignored
                    x.skip();
                    continue;
                }
                sheet.rows.emplace_back();
                row = &sheet.rows.back();
                row->index = r;
                const bool hidden = x.attribute("hidden") == "1" || x.attribute("hidden") == "true";
                if (hidden)
                    row->height = 0.01f;
                else if (x.attribute("customHeight") == "1" || x.attribute("customHeight") == "true" || x.hasAttribute("ht"))
                    row->height = float(std::atof(x.attribute("ht").c_str()));
            } else if (n == "c" && row) {
                if (cellCount_ >= 2000000) {
                    truncated_ = true;
                    x.skip();
                    continue;
                }
                std::uint32_t r = row->index, c = nextColumn;
                if (x.hasAttribute("r"))
                    parseReference(x.attribute("r"), r, c);
                nextColumn = c + 1;
                cell(x, *row, c);
                sheet.columnCount = std::max(sheet.columnCount, c + 1);
                sheet.rowCount = std::max(sheet.rowCount, row->index + 1);
            } else if (n == "mergeCell") {
                const std::string ref = x.attribute("ref");
                const std::size_t colon = ref.find(':');
                Merge m;
                if (colon != std::string::npos && parseReference(std::string_view(ref).substr(0, colon), m.row0, m.col0) &&
                    parseReference(std::string_view(ref).substr(colon + 1), m.row1, m.col1) && m.row1 >= m.row0 && m.col1 >= m.col0)
                    sheet.merges.push_back(m);
            }
        }
        // Rows that hold nothing and keep the default height add nothing.
        sheet.rows.erase(std::remove_if(sheet.rows.begin(), sheet.rows.end(), [](const Row& r) { return r.cells.empty() && r.height == 0.0f; }),
                         sheet.rows.end());
        for (const Merge& m : sheet.merges) {
            sheet.rowCount = std::max(sheet.rowCount, m.row1 + 1);
            sheet.columnCount = std::max(sheet.columnCount, m.col1 + 1);
        }
    }

    // At the Start of c: the cell's value, formula and format.
    void cell(XmlReader& x, Row& row, std::uint32_t column)
    {
        const std::string type = x.attribute("t");
        const int styleIndex = std::atoi(x.attribute("s").c_str());
        std::string value, formula, inlineText;
        bool hasValue = false, hasInline = false;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "v") {
                value = x.elementText();
                hasValue = true;
            } else if (n == "f") {
                formula = x.elementText();
            } else if (n == "is") {
                inlineText = richText(x);
                hasInline = true;
            }
        }
        if (!hasValue && !hasInline && formula.empty() && styleIndex <= 0)
            return;
        Cell c;
        c.column = column;
        c.formula = std::move(formula);
        const CellFormat* f = styleIndex >= 0 && styleIndex < int(formats_.size()) ? &formats_[size_t(styleIndex)] : nullptr;
        if (f) {
            c.flags = f->flags;
            c.color = f->color;
            c.fill = f->fill;
            c.align = f->align;
        }
        if (type == "s") {
            const long i = std::atol(value.c_str());
            if (hasValue && i >= 0 && std::size_t(i) < strings_.size())
                c.text = strings_[size_t(i)];
        } else if (type == "inlineStr") {
            c.text = std::move(inlineText);
        } else if (type == "str") {
            c.text = std::move(value);
        } else if (type == "b") {
            c.text = value == "1" || value == "true" ? "TRUE" : "FALSE";
            if (c.align < 0)
                c.align = 1;
        } else if (type == "e") {
            c.text = std::move(value);
            c.flags |= Cell::Error;
            if (c.align < 0)
                c.align = 1;
        } else if (type == "d") { // an ISO 8601 date: shown as written
            c.text = std::move(value);
            c.flags |= Cell::Number;
        } else if (hasValue) {
            char* end = nullptr;
            const double v = std::strtod(value.c_str(), &end);
            if (end != value.c_str()) {
                c.text = formatNumber(v, f ? f->code : std::string_view(), date1904_);
                c.flags |= Cell::Number;
            } else {
                c.text = std::move(value);
            }
        }
        if (c.text.empty() && c.formula.empty() && c.fill == 0)
            return;
        if (!row.cells.empty() && row.cells.back().column >= column) // out of order: ignored
            return;
        row.cells.push_back(std::move(c));
        ++cellCount_;
    }

    ooxml::ZipArchive& zip_;
    ooxml::Theme theme_;
    std::vector<std::string> strings_;
    std::vector<CellFormat> formats_;
    bool date1904_ = false;
    int otherSheets_ = 0, charts_ = 0;
    std::size_t cellCount_ = 0;
    bool truncated_ = false;
};

} // namespace

ViewerDocument readXlsx(std::string bytes)
{
    ooxml::ZipArchive zip;
    std::string error;
    if (!zip.open(std::move(bytes), &error)) {
        ViewerDocument doc;
        doc.format = "Excel workbook";
        doc.error = error;
        return doc;
    }
    return XlsxReader(zip).read();
}

} // namespace enp::model::viewer
