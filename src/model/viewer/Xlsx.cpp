// Excel (.xlsx): the workbook's sheets, shared strings and cell formats into the Workbook model.
// Each cell keeps its saved value formatted by its number format, its formula's text, and its
// font weight, colours and alignment. Values are as saved: nothing is recalculated.
#include "enp/model/viewer/NumberFormat.h"
#include "enp/model/viewer/Viewer.h"

#include "Chart.h"
#include "Ooxml.h"
#include "Shapes.h"

#include <algorithm>
#include <cmath>
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
    std::uint8_t flags = 0; // Cell::Bold / Italic / Underline / Strike / Wrap
    std::uint32_t color = 0, fill = 0;
    std::int8_t align = -1, valign = -1;
    std::uint8_t halfPoints = 0, indent = 0, font = 0;
    std::int16_t rotation = 0;
    std::uint16_t border = 0, fillStyle = 0;
};

struct Font {
    std::uint8_t flags = 0;
    std::uint32_t color = 0;
    float size = 0.0f;
    std::string name;
};

// A fill once read: its colour, and the pattern or gradient over it.
struct Fill {
    std::uint32_t color = 0;
    std::uint16_t style = 0; // into Workbook::fills
};

// The share of the pattern's colour in the grey patterns (dots too fine to draw as dots).
float greyShare(std::string_view pattern)
{
    return pattern == "darkGray" ? 0.75f : pattern == "mediumGray" ? 0.5f : pattern == "lightGray" ? 0.25f
         : pattern == "gray125" ? 0.125f : pattern == "gray0625" ? 0.0625f : -1.0f;
}

std::uint32_t mix(std::uint32_t a, std::uint32_t b, float t) // a toward b by t, packed like Page colours
{
    auto ch = [&](int shift) {
        const float x = float((a >> shift) & 0xFF), y = float((b >> shift) & 0xFF);
        return std::uint32_t(std::lround(x + (y - x) * t)) << shift;
    };
    return ch(0) | ch(8) | ch(16) | 0xFF000000u;
}

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
        book_ = &doc.book;
        const std::string main = ooxml::mainPart(zip_, "xl/workbook.xml");
        const std::optional<std::string> xml = zip_.read(main);
        if (!xml) {
            doc.error = "the workbook part (" + main + ") is missing";
            return doc;
        }
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, main);
        theme_ = ooxml::readTheme(zip_, ooxml::targetOfType(rels, "theme"));
        readStyles(ooxml::targetOfType(rels, "styles")); // first: runs name fonts against the default one
        readSharedStrings(ooxml::targetOfType(rels, "sharedStrings"));

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
            std::uint32_t rich = 0;
            strings_.push_back(richText(x, &rich));
            stringRich_.push_back(rich);
        }
    }

    // At the Start of si / is: its text runs, without phonetic hints (rPh). Runs in fonts of their
    // own go to the workbook's rich texts, their index to `rich`.
    std::string richText(XmlReader& x, std::uint32_t* rich)
    {
        std::string out;
        std::vector<CellRun> runs;
        bool anyOwn = false;
        CellRun* run = nullptr;
        const int depth = x.depth();
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End && x.depth() < depth)
                break;
            if (e == Event::End) {
                if (x.localName() == "r" && run) {
                    run->end = std::uint32_t(out.size());
                    run = nullptr;
                }
                continue;
            }
            if (e != Event::Start)
                continue;
            const std::string_view n = x.localName();
            if (n == "rPh" || n == "phoneticPr") {
                x.skip();
            } else if (n == "r") {
                runs.emplace_back();
                run = &runs.back();
                run->begin = std::uint32_t(out.size());
            } else if (n == "rPr" && run) {
                run->own = anyOwn = true;
            } else if (n == "t") {
                out += x.elementText();
            } else if (run && run->own) {
                const std::string v = x.attribute("val");
                const bool set = v.empty() || (v != "0" && v != "false" && v != "none");
                if (n == "b" && set)
                    run->flags |= Cell::Bold;
                else if (n == "i" && set)
                    run->flags |= Cell::Italic;
                else if (n == "u" && set)
                    run->flags |= Cell::Underline;
                else if (n == "strike" && set)
                    run->flags |= Cell::Strike;
                else if (n == "sz")
                    run->halfPoints = std::uint8_t(std::clamp(std::lround(std::atof(v.c_str()) * 2.0), 1L, 255L));
                else if (n == "color")
                    run->color = textColor(colorOf(x));
                else if (n == "rFont")
                    run->font = fontNumber(v);
            }
        }
        if (rich && anyOwn && runs.size() <= 4096) {
            if (book_->richTexts.empty())
                book_->richTexts.emplace_back(); // [0]: none
            *rich = std::uint32_t(book_->richTexts.size());
            book_->richTexts.push_back(std::move(runs));
        }
        return out;
    }

    // A text colour: black (the automatic colour) as 0, so the view picks a readable one.
    std::uint32_t textColor(std::uint32_t c) const
    {
        return c == theme_.color("dk1") || c == ooxml::hexColor("000000") ? 0 : c;
    }

    // A font's number in Workbook::fontNames (from 1; 0: the default font, or too many).
    std::uint8_t fontNumber(const std::string& name)
    {
        if (name.empty() || name == book_->defaultFont)
            return 0;
        for (std::size_t i = 0; i < book_->fontNames.size(); ++i)
            if (book_->fontNames[i] == name)
                return std::uint8_t(i + 1);
        if (book_->fontNames.size() >= 255)
            return 0;
        book_->fontNames.push_back(name);
        return std::uint8_t(book_->fontNames.size());
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
            // Excel's default palette (8-63; 0-7 repeat 8-15); 64 and up: the system's colours (automatic).
            static const char* const kIndexed[] = {
                "000000", "FFFFFF", "FF0000", "00FF00", "0000FF", "FFFF00", "FF00FF", "00FFFF", "800000", "008000", "000080", "808000",
                "800080", "008080", "C0C0C0", "808080", "9999FF", "993366", "FFFFCC", "CCFFFF", "660066", "FF8080", "0066CC", "CCCCFF",
                "000080", "FF00FF", "FFFF00", "00FFFF", "800080", "800000", "008080", "0000FF", "00CCFF", "CCFFFF", "CCFFCC", "FFFF99",
                "99CCFF", "FF99CC", "CC99FF", "FFCC99", "3366FF", "33CCCC", "99CC00", "FFCC00", "FF9900", "FF6600", "666699", "969696",
                "003366", "339966", "003300", "333300", "993300", "993366", "333399", "333333"};
            const int i = std::atoi(x.attribute("indexed").c_str());
            if (i >= 0 && i < 8)
                c = ooxml::hexColor(kIndexed[i]);
            else if (i >= 8 && i < 64)
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
        std::vector<Fill> fills;
        std::vector<Border> borders;
        std::map<int, std::uint16_t> borderIndex; // borderId -> Workbook::borders
        int edge = -1;                            // the border edge being read
        XmlReader x(*xml);
        enum class In { None, Fonts, Fills, Borders, CellXfs, Other } in = In::None;
        // The fill being read: its pattern's colours, or its gradient.
        std::string pattern;
        std::uint32_t fg = 0, bg = 0;
        bool hasFg = false, hasBg = false;
        FillStyle gradient;
        bool inGradient = false;
        float stopAt = 0.0f;
        auto endFill = [&]() {
            if (fills.empty())
                return;
            Fill& f = fills.back();
            const FillStyle* style = nullptr;
            FillStyle hatch;
            if (inGradient) {
                if (gradient.stopCount == 0)
                    return;
                f.color = gradient.stop[0];
                style = &gradient;
            } else if (pattern == "solid") {
                f.color = hasFg ? fg : hasBg ? bg : 0;
            } else if (pattern.empty() || pattern == "none") {
                f.color = 0;
            } else {
                // The pattern's colour (black when not given) over the background (white).
                const std::uint32_t ink = hasFg ? fg : ooxml::hexColor("000000"), paper = hasBg ? bg : ooxml::hexColor("FFFFFF");
                const float share = greyShare(pattern);
                if (share >= 0.0f) {
                    f.color = mix(paper, ink, share);
                } else {
                    f.color = paper;
                    const std::string_view p = pattern;
                    hatch.dark = p.substr(0, 4) == "dark";
                    const std::string_view kind = p.substr(hatch.dark ? 4 : std::min<std::size_t>(5, p.size()));
                    hatch.lines = kind == "Vertical" ? FillStyle::Vertical : kind == "Down" ? FillStyle::Down : kind == "Up" ? FillStyle::Up
                                : kind == "Grid" ? FillStyle::Grid : kind == "Trellis" ? FillStyle::Trellis : FillStyle::Horizontal;
                    hatch.color = ink;
                    style = &hatch;
                }
            }
            if (style && book_->fills.size() < 65535) {
                if (book_->fills.empty())
                    book_->fills.emplace_back(); // [0]: none
                f.style = std::uint16_t(book_->fills.size());
                book_->fills.push_back(*style);
            }
        };
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e == Event::End) {
                const std::string_view n = x.localName();
                if (n == "fonts" || n == "fills" || n == "borders" || n == "cellXfs" || n == "cellStyleXfs" || n == "dxfs")
                    in = In::None;
                else if (n == "left" || n == "right" || n == "top" || n == "bottom" || n == "start" || n == "end" || n == "diagonal")
                    edge = -1;
                else if (in == In::Fills && n == "fill")
                    endFill();
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
            } else if (n == "borders") {
                in = In::Borders;
            } else if (in == In::Borders && n == "border") {
                borders.emplace_back();
                edge = -1;
                borders.back().diagonalUp = x.attribute("diagonalUp") == "1" || x.attribute("diagonalUp") == "true";
                borders.back().diagonalDown = x.attribute("diagonalDown") == "1" || x.attribute("diagonalDown") == "true";
            } else if (in == In::Borders && !borders.empty() &&
                       (n == "left" || n == "right" || n == "top" || n == "bottom" || n == "start" || n == "end" || n == "diagonal")) {
                edge = n == "left" || n == "start" ? 0 : n == "right" || n == "end" ? 1 : n == "top" ? 2 : n == "bottom" ? 3 : 4;
                const std::string st = x.attribute("style");
                borders.back().style[edge] = st.empty() || st == "none" ? Border::None
                                             : st == "medium" || st == "mediumDashed" || st == "mediumDashDot" || st == "mediumDashDotDot" || st == "slantDashDot" ? Border::Medium
                                             : st == "thick" ? Border::Thick
                                             : st == "dashed" || st == "dashDot" || st == "dashDotDot" ? Border::Dashed
                                             : st == "dotted" ? Border::Dotted
                                             : st == "double" ? Border::Double
                                             : st == "hair" ? Border::Hair : Border::Thin;
            } else if (in == In::Borders && n == "color" && edge >= 0 && !borders.empty()) {
                borders.back().color[edge] = textColor(colorOf(x));
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
                else if (n == "strike" && set)
                    fonts.back().flags |= Cell::Strike;
                else if (n == "sz")
                    fonts.back().size = float(std::atof(v.c_str()));
                else if (n == "color")
                    fonts.back().color = colorOf(x);
                else if (n == "name")
                    fonts.back().name = v;
            } else if (in == In::Fills && n == "fill") {
                fills.emplace_back();
                pattern.clear();
                hasFg = hasBg = inGradient = false;
                gradient = FillStyle();
            } else if (in == In::Fills && n == "patternFill" && !fills.empty()) {
                pattern = x.attribute("patternType");
            } else if (in == In::Fills && (n == "fgColor" || n == "bgColor") && !fills.empty() && !inGradient) {
                const bool automatic = x.attribute("auto") == "1" || (x.hasAttribute("indexed") && std::atoi(x.attribute("indexed").c_str()) >= 64);
                if (!automatic)
                    (n == "fgColor" ? fg : bg) = colorOf(x);
                (n == "fgColor" ? hasFg : hasBg) = !automatic;
            } else if (in == In::Fills && n == "gradientFill" && !fills.empty()) {
                inGradient = true;
                gradient.kind = x.attribute("type") == "path" ? FillStyle::Path : FillStyle::Linear;
                gradient.degree = float(std::atof(x.attribute("degree").c_str()));
            } else if (in == In::Fills && n == "stop" && inGradient) {
                stopAt = std::clamp(float(std::atof(x.attribute("position").c_str())), 0.0f, 1.0f);
            } else if (in == In::Fills && n == "color" && inGradient && gradient.stopCount < 4) {
                gradient.position[gradient.stopCount] = stopAt;
                gradient.stop[gradient.stopCount++] = colorOf(x);
            } else if (in == In::CellXfs && n == "xf") {
                CellFormat f;
                const int numFmt = std::atoi(x.attribute("numFmtId").c_str());
                const auto custom = codes.find(numFmt);
                f.code = custom != codes.end() ? custom->second : std::string(builtinNumberFormat(numFmt));
                f.date = isDateFormat(f.code);
                const int font = std::atoi(x.attribute("fontId").c_str());
                if (!fonts.empty() && fonts[0].size > 0.0f)
                    book_->defaultFontSize = fonts[0].size;
                if (!fonts.empty() && !fonts[0].name.empty())
                    book_->defaultFont = fonts[0].name;
                if (font >= 0 && font < int(fonts.size())) {
                    f.flags = fonts[size_t(font)].flags;
                    f.font = fontNumber(fonts[size_t(font)].name);
                    f.color = textColor(fonts[size_t(font)].color); // black: the view picks a readable colour
                    const float size = fonts[size_t(font)].size;
                    if (size > 0.0f && std::fabs(size - book_->defaultFontSize) > 0.01f)
                        f.halfPoints = std::uint8_t(std::clamp(std::lround(size * 2.0f), 1L, 255L));
                }
                const int border = std::atoi(x.attribute("borderId").c_str());
                if (border > 0 && border < int(borders.size())) {
                    const Border& b = borders[size_t(border)];
                    if (b.style[0] || b.style[1] || b.style[2] || b.style[3] || (b.style[4] && (b.diagonalUp || b.diagonalDown))) {
                        auto [it, added] = borderIndex.emplace(border, std::uint16_t(0));
                        if (added && book_->borders.size() < 65535) {
                            if (book_->borders.empty())
                                book_->borders.emplace_back(); // [0]: none
                            it->second = std::uint16_t(book_->borders.size());
                            book_->borders.push_back(b);
                        }
                        f.border = it->second;
                    }
                }
                const int fill = std::atoi(x.attribute("fillId").c_str());
                if (fill >= 2 && fill < int(fills.size())) { // 0 and 1 are reserved (none, gray125)
                    f.fill = fills[size_t(fill)].color;
                    f.fillStyle = fills[size_t(fill)].style;
                }
                formats_.push_back(std::move(f));
            } else if (in == In::CellXfs && n == "alignment" && !formats_.empty()) {
                CellFormat& f = formats_.back();
                const std::string h = x.attribute("horizontal");
                f.align = h == "center" || h == "centerContinuous" ? 1 : h == "right" ? 2 : h == "left" ? 0 : -1;
                const std::string v = x.attribute("vertical");
                f.valign = v == "top" ? 0 : v == "center" || v == "justify" || v == "distributed" ? 1 : -1;
                const std::string wrap = x.attribute("wrapText"), shrink = x.attribute("shrinkToFit");
                if (wrap == "1" || wrap == "true")
                    f.flags |= Cell::Wrap;
                else if (shrink == "1" || shrink == "true")
                    f.flags |= Cell::Shrink;
                f.indent = std::uint8_t(std::clamp(std::atoi(x.attribute("indent").c_str()), 0, 250));
                // 0-90 anticlockwise, 91-180 clockwise (as 90 - n), 255 stacked.
                const int rot = std::atoi(x.attribute("textRotation").c_str());
                f.rotation = std::int16_t(rot == 255 ? 255 : rot > 90 && rot <= 180 ? 90 - rot : std::clamp(rot, 0, 90));
            }
        }
    }

    void readSheet(const std::string& part, Sheet& sheet)
    {
        const std::optional<std::string> xml = zip_.read(part);
        if (!xml)
            return;
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, part);
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
        // Pictures and charts over the cells (after the columns and rows: they size the drawings).
        for (const auto& [id, r] : rels)
            if (r.type == "drawing" && !r.external)
                readDrawing(r.target, sheet);
    }

    // A sheet's size in points from its start to column `c` / row `r` (Excel's widths: characters
    // of 7 pixels plus 5 pixels of padding, at 96 dpi).
    static float columnX(const Sheet& sheet, std::uint32_t c)
    {
        float x = 0.0f;
        for (std::uint32_t i = 0; i < c && i < 16384; ++i) {
            const float w = sheet.columnWidth(i);
            x += w <= 0.0f ? 0.0f : (w * 7.0f + 5.0f) * 0.75f;
        }
        return x;
    }
    static float rowY(const Sheet& sheet, std::uint32_t r)
    {
        float y = float(r) * sheet.defaultRowHeight;
        for (const Row& row : sheet.rows) {
            if (row.index >= r)
                break;
            if (row.height > 0.0f)
                y += row.height - sheet.defaultRowHeight;
        }
        return y;
    }

    // A drawing part (xdr:wsDr): each anchor's picture or chart becomes a SheetDrawing. Shapes and
    // anything else are counted as not shown.
    void readDrawing(const std::string& part, Sheet& sheet)
    {
        const std::optional<std::string> xml = zip_.read(part, std::size_t(16) << 20);
        if (!xml)
            return;
        const ooxml::Relationships rels = ooxml::readRelationships(zip_, part);
        constexpr double kEmu = 12700.0;
        XmlReader x(*xml);
        for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
            if (e != Event::Start)
                continue;
            const std::string_view kind = x.localName();
            if (kind != "twoCellAnchor" && kind != "oneCellAnchor" && kind != "absoluteAnchor")
                continue;
            SheetDrawing d;
            float width = 0.0f, height = 0.0f, absX = 0.0f, absY = 0.0f;
            bool absolute = kind == "absoluteAnchor", twoCell = kind == "twoCellAnchor";
            std::string picture, chartId;
            bool other = false;
            Page shapes; // a shape anchored here, drawn filling the anchor
            int corner = -1; // 0 from, 1 to
            const int depth = x.depth();
            for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                if (f == Event::End) {
                    if (x.depth() < depth)
                        break;
                    if (x.localName() == "from" || x.localName() == "to")
                        corner = -1;
                    continue;
                }
                if (f != Event::Start)
                    continue;
                const std::string_view n = x.localName();
                if (n == "from" || n == "to") {
                    corner = n == "from" ? 0 : 1;
                } else if (corner >= 0 && (n == "col" || n == "row" || n == "colOff" || n == "rowOff")) {
                    const std::string text = x.elementText();
                    const double v = std::atof(text.c_str());
                    const std::uint32_t index = std::uint32_t(std::clamp(v, 0.0, 1048575.0));
                    if (n == "col")
                        (corner == 0 ? d.col0 : d.col1) = index;
                    else if (n == "row")
                        (corner == 0 ? d.row0 : d.row1) = index;
                    else if (n == "colOff")
                        (corner == 0 ? d.dx0 : d.dx1) = float(v / kEmu);
                    else
                        (corner == 0 ? d.dy0 : d.dy1) = float(v / kEmu);
                } else if (n == "pos" && absolute) {
                    absX = float(std::atof(x.attribute("x").c_str()) / kEmu);
                    absY = float(std::atof(x.attribute("y").c_str()) / kEmu);
                } else if (n == "ext" && width == 0.0f && x.hasAttribute("cx") && !twoCell) {
                    width = float(std::atof(x.attribute("cx").c_str()) / kEmu);
                    height = float(std::atof(x.attribute("cy").c_str()) / kEmu);
                } else if (n == "blip" && picture.empty()) {
                    picture = x.attribute("r:embed");
                } else if (n == "chart" && chartId.empty()) {
                    chartId = x.attribute("r:id");
                } else if (n == "sp" || n == "cxnSp") {
                    // from / to (and ext) come before the shape: its rectangle is known here.
                    const float sx0 = absolute ? absX : columnX(sheet, d.col0) + d.dx0;
                    const float sy0 = absolute ? absY : rowY(sheet, d.row0) + d.dy0;
                    const float sw = twoCell ? columnX(sheet, d.col1) + d.dx1 - sx0 : width;
                    const float sh = twoCell ? rowY(sheet, d.row1) + d.dy1 - sy0 : height;
                    if (sw < 1.0f || sh < 1.0f)
                        x.skip();
                    else if (dml::shape(x, theme_, Rect{0.0f, 0.0f, sw, sh}, shapes)) // reads the element to its end
                        continue;
                    other = true;
                } else if (n == "Fallback") {
                    x.skip();
                }
            }
            // Where it is and how big, in points.
            const float x0 = absolute ? absX : columnX(sheet, d.col0) + d.dx0;
            const float y0 = absolute ? absY : rowY(sheet, d.row0) + d.dy0;
            if (twoCell) {
                width = columnX(sheet, d.col1) + d.dx1 - x0;
                height = rowY(sheet, d.row1) + d.dy1 - y0;
            }
            if (absolute) {
                d.col0 = d.row0 = 0;
                d.dx0 = absX;
                d.dy0 = absY;
            }
            if (!shapes.items.empty() && width >= 1.0f && height >= 1.0f && picture.empty() && chartId.empty()) {
                shapes.width = width;
                shapes.height = height;
                shapes.background = 0;
                d.page = std::move(shapes);
                sheet.rowCount = std::max(sheet.rowCount, (twoCell ? d.row1 : d.row0) + 1);
                sheet.columnCount = std::max(sheet.columnCount, (twoCell ? d.col1 : d.col0) + 1);
                if (sheet.drawings.size() < 1000)
                    sheet.drawings.push_back(std::move(d));
                continue;
            }
            if (width < 2.0f || height < 2.0f || (picture.empty() && chartId.empty())) {
                if (other || !chartId.empty())
                    ++charts_;
                continue;
            }
            d.page.width = width;
            d.page.height = height;
            d.page.background = 0;
            bool drawn = false;
            if (!chartId.empty()) {
                const auto it = rels.find(chartId);
                chart::Chart c;
                if (it != rels.end() && !it->second.external && chart::read(zip_, it->second.target, theme_, c)) {
                    chart::draw(c, theme_, Rect{0.0f, 0.0f, width, height}, d.page);
                    drawn = true;
                }
            } else {
                const auto it = rels.find(picture);
                if (it != rels.end() && !it->second.external) {
                    if (!book_->media.count(it->second.target))
                        if (std::optional<std::string> bytes = zip_.read(it->second.target, std::size_t(64) << 20))
                            book_->media[it->second.target] = std::move(*bytes);
                    if (book_->media.count(it->second.target)) {
                        d.page.add(ImageItem{Rect{0.0f, 0.0f, width, height}, it->second.target});
                        drawn = true;
                    }
                }
            }
            if (!drawn) {
                ++charts_;
                continue;
            }
            // The used range reaches the drawing (so the grid scrolls to it).
            sheet.rowCount = std::max(sheet.rowCount, (twoCell ? d.row1 : d.row0) + 1);
            sheet.columnCount = std::max(sheet.columnCount, (twoCell ? d.col1 : d.col0) + 1);
            if (sheet.drawings.size() < 1000)
                sheet.drawings.push_back(std::move(d));
        }
    }

    // At the Start of c: the cell's value, formula and format.
    void cell(XmlReader& x, Row& row, std::uint32_t column)
    {
        const std::string type = x.attribute("t");
        const int styleIndex = std::atoi(x.attribute("s").c_str());
        std::string value, formula, inlineText;
        bool hasValue = false, hasInline = false;
        std::uint32_t inlineRich = 0;
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
                inlineText = richText(x, &inlineRich);
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
            c.valign = f->valign;
            c.halfPoints = f->halfPoints;
            c.rotation = f->rotation;
            c.border = f->border;
            c.indent = f->indent;
            c.font = f->font;
            c.fillStyle = f->fillStyle;
        }
        if (type == "s") {
            const long i = std::atol(value.c_str());
            if (hasValue && i >= 0 && std::size_t(i) < strings_.size()) {
                c.text = strings_[size_t(i)];
                c.rich = stringRich_[size_t(i)];
            }
        } else if (type == "inlineStr") {
            c.text = std::move(inlineText);
            c.rich = inlineRich;
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
                std::uint32_t tag = 0; // [Red] and the like
                c.text = formatNumber(v, f ? f->code : std::string_view(), date1904_, &tag);
                c.flags |= Cell::Number;
                if (tag)
                    c.color = textColor(tag);
            } else {
                c.text = std::move(value);
            }
        }
        if (c.text.empty() && c.formula.empty() && c.fill == 0 && c.fillStyle == 0 && c.border == 0)
            return;
        if (!row.cells.empty() && row.cells.back().column >= column) // out of order: ignored
            return;
        row.cells.push_back(std::move(c));
        ++cellCount_;
    }

    ooxml::ZipArchive& zip_;
    Workbook* book_ = nullptr; // the workbook being read (its pictures)
    ooxml::Theme theme_;
    std::vector<std::string> strings_;
    std::vector<std::uint32_t> stringRich_; // per shared string: its Workbook::richTexts index (0: none)
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
