#include "Shapes.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace enp::model::viewer::dml {
namespace {

using ooxml::XmlReader;
using Event = XmlReader::Event;
constexpr float kPi = 3.14159265358979f;
constexpr double kEmu = 12700.0;

void ellipse(std::vector<Point>& out, Rect r)
{
    const int n = 48;
    for (int i = 0; i < n; ++i) {
        const float a = 2.0f * kPi * float(i) / float(n);
        out.push_back({r.x + r.w * 0.5f * (1.0f + std::cos(a)), r.y + r.h * 0.5f * (1.0f + std::sin(a))});
    }
}

void rounded(std::vector<Point>& out, Rect r, float radius)
{
    radius = std::min({radius, r.w * 0.5f, r.h * 0.5f});
    const int n = 6;
    auto corner = [&](float cx, float cy, float start) {
        for (int i = 0; i <= n; ++i) {
            const float a = start + (kPi / 2.0f) * float(i) / float(n);
            out.push_back({cx + radius * std::cos(a), cy + radius * std::sin(a)});
        }
    };
    corner(r.x + r.w - radius, r.y + radius, -kPi / 2.0f);
    corner(r.x + r.w - radius, r.y + r.h - radius, 0.0f);
    corner(r.x + radius, r.y + r.h - radius, kPi / 2.0f);
    corner(r.x + radius, r.y + radius, kPi);
}

void star(std::vector<Point>& out, Rect r, int points, float inner)
{
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    for (int i = 0; i < points * 2; ++i) {
        const float a = -kPi / 2.0f + kPi * float(i) / float(points);
        const float k = i % 2 == 0 ? 1.0f : inner;
        out.push_back({cx + r.w * 0.5f * k * std::cos(a), cy + r.h * 0.5f * k * std::sin(a)});
    }
}

void polygon(std::vector<Point>& out, Rect r, int sides)
{
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    for (int i = 0; i < sides; ++i) {
        const float a = -kPi / 2.0f + 2.0f * kPi * float(i) / float(sides) + (sides % 2 == 0 ? kPi / float(sides) : 0.0f);
        out.push_back({cx + r.w * 0.5f * std::cos(a), cy + r.h * 0.5f * std::sin(a)});
    }
}

// An arrow pointing right in a unit square, then turned: 0 right, 1 down, 2 left, 3 up.
void arrow(std::vector<Point>& out, Rect r, int direction, bool both)
{
    const bool vertical = direction == 1 || direction == 3;
    const float len = vertical ? r.h : r.w, thick = vertical ? r.w : r.h;
    const float head = std::min(len * (both ? 0.35f : 0.5f), thick);
    std::vector<Point> u; // along = x, across = y, in [0,len] x [0,thick]
    if (both)
        u = {{0, thick * 0.5f}, {head, 0}, {head, thick * 0.25f}, {len - head, thick * 0.25f}, {len - head, 0}, {len, thick * 0.5f},
             {len - head, thick}, {len - head, thick * 0.75f}, {head, thick * 0.75f}, {head, thick}};
    else
        u = {{0, thick * 0.25f}, {len - head, thick * 0.25f}, {len - head, 0}, {len, thick * 0.5f},
             {len - head, thick}, {len - head, thick * 0.75f}, {0, thick * 0.75f}};
    for (const Point& p : u) {
        switch (direction) {
        case 0: out.push_back({r.x + p.x, r.y + p.y}); break;
        case 2: out.push_back({r.x + r.w - p.x, r.y + p.y}); break;
        case 1: out.push_back({r.x + p.y, r.y + p.x}); break;
        default: out.push_back({r.x + p.y, r.y + r.h - p.x}); break;
        }
    }
}

bool on(const XmlReader& x, const char* name)
{
    const std::string v = x.attribute(name);
    return v == "1" || v == "true";
}

} // namespace

void rotate(std::vector<Point>& points, Rect r, float degrees)
{
    if (std::fabs(degrees) < 0.01f)
        return;
    const float a = degrees * kPi / 180.0f, c = std::cos(a), s = std::sin(a);
    const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    for (Point& p : points) {
        const float dx = p.x - cx, dy = p.y - cy;
        p = {cx + dx * c - dy * s, cy + dx * s + dy * c};
    }
}

Rect turnedBox(Rect r, float degrees)
{
    float d = std::fmod(degrees, 180.0f);
    if (d < 0.0f)
        d += 180.0f;
    if (d < 45.0f || d >= 135.0f)
        return r;
    return Rect{r.x + (r.w - r.h) * 0.5f, r.y + (r.h - r.w) * 0.5f, r.h, r.w};
}

std::vector<float> LineStyle::pattern() const
{
    // DrawingML's preset dashes, in line widths (ECMA-376 20.1.10.48); the system ones are tighter.
    static const std::pair<const char*, std::vector<float>> kDashes[] = {
        {"dot", {1, 3}},           {"dash", {4, 3}},           {"lgDash", {8, 3}},
        {"dashDot", {4, 3, 1, 3}}, {"lgDashDot", {8, 3, 1, 3}}, {"lgDashDotDot", {8, 3, 1, 3, 1, 3}},
        {"sysDot", {1, 1}},        {"sysDash", {3, 1}},        {"sysDashDot", {3, 1, 1, 1}},
        {"sysDashDotDot", {3, 1, 1, 1, 1, 1}}};
    for (const auto& [name, units] : kDashes)
        if (dash == name) {
            std::vector<float> out;
            for (float u : units)
                out.push_back(u * std::max(0.5f, width));
            return out;
        }
    return {};
}

namespace {
LineEnd lineEndOf(const std::string& type)
{
    if (type == "triangle")
        return LineEnd::Triangle;
    if (type == "arrow")
        return LineEnd::Arrow;
    if (type == "stealth")
        return LineEnd::Stealth;
    if (type == "diamond")
        return LineEnd::Diamond;
    if (type == "oval")
        return LineEnd::Oval;
    return LineEnd::None;
}
} // namespace

void readLine(XmlReader& x, const ooxml::Theme& theme, const std::map<std::string, std::string>* schemeMap, LineStyle& out)
{
    if (x.hasAttribute("w")) {
        out.width = std::max(0.25f, float(std::atof(x.attribute("w").c_str()) / kEmu));
        out.hasWidth = true;
    }
    const int depth = x.depth();
    for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
        if (e == Event::End && x.depth() < depth)
            break;
        if (e != Event::Start)
            continue;
        const std::string_view n = x.localName();
        if (n == "noFill") {
            out.hasColor = true;
            out.color = 0;
        } else if (n == "solidFill" || n == "gradFill") {
            bool found = false;
            const std::uint32_t c = ooxml::readFillIn(x, theme, schemeMap, &found);
            if (found) {
                out.hasColor = true;
                out.color = c;
            }
        } else if (n == "prstDash") {
            const std::string v = x.attribute("val");
            out.dash = v == "solid" ? std::string() : v;
        } else if (n == "headEnd") {
            out.head = lineEndOf(x.attribute("type"));
        } else if (n == "tailEnd") {
            out.tail = lineEndOf(x.attribute("type"));
        } else {
            x.skip();
        }
    }
}

bool isLine(std::string_view g)
{
    return g == "line" || g == "straightConnector1" || g.find("Connector") != std::string_view::npos;
}

void connectorPath(std::string_view g, Rect r, bool flipH, bool flipV, std::vector<Point>& out)
{
    out.clear();
    // In the box's own terms (0..1, start at the top left), then placed and flipped.
    auto put = [&](float u, float v) {
        out.push_back({r.x + (flipH ? 1.0f - u : u) * r.w, r.y + (flipV ? 1.0f - v : v) * r.h});
    };
    if (g == "bentConnector2") { // one corner: across, then down
        put(0.0f, 0.0f);
        put(1.0f, 0.0f);
        put(1.0f, 1.0f);
    } else if (g.rfind("bentConnector", 0) == 0) { // across to the middle, down, across (adj1: 50%)
        put(0.0f, 0.0f);
        put(0.5f, 0.0f);
        put(0.5f, 1.0f);
        put(1.0f, 1.0f);
    } else if (g.rfind("curvedConnector", 0) == 0) { // a cubic leaving level and arriving level
        for (int i = 0; i <= 24; ++i) {             // controls at (0.5, 0) and (0.5, 1)
            const float t = float(i) / 24.0f, s = 1.0f - t;
            put(1.5f * s * s * t + 1.5f * s * t * t + t * t * t, 3.0f * s * t * t + t * t * t);
        }
    } else {
        put(0.0f, 0.0f);
        put(1.0f, 1.0f);
    }
}

void presetOutline(std::string_view g, Rect r, std::vector<Point>& pts)
{
    const float x0 = r.x, y0 = r.y, x1 = r.x + r.w, y1 = r.y + r.h, cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
    if (g == "ellipse" || g == "circle" || g == "donut" || g == "pie" || g == "chord" || g == "flowChartConnector" ||
        g == "cloud" || g == "cloudCallout" || g == "wedgeEllipseCallout" || g == "smileyFace") {
        ellipse(pts, r);
    } else if (g == "roundRect" || g == "round2SameRect" || g == "round1Rect" || g == "snipRoundRect" || g == "wedgeRoundRectCallout" ||
               g == "flowChartAlternateProcess") {
        rounded(pts, r, std::min(r.w, r.h) * 0.1667f);
    } else if (g == "flowChartTerminator") {
        rounded(pts, r, std::min(r.w, r.h) * 0.5f);
    } else if (g == "triangle" || g == "flowChartExtract") {
        pts = {{cx, y0}, {x1, y1}, {x0, y1}};
    } else if (g == "flowChartMerge") {
        pts = {{x0, y0}, {x1, y0}, {cx, y1}};
    } else if (g == "rtTriangle") {
        pts = {{x0, y0}, {x1, y1}, {x0, y1}};
    } else if (g == "diamond" || g == "flowChartDecision") {
        pts = {{cx, y0}, {x1, cy}, {cx, y1}, {x0, cy}};
    } else if (g == "parallelogram" || g == "flowChartInputOutput") {
        const float k = r.w * 0.2f;
        pts = {{x0 + k, y0}, {x1, y0}, {x1 - k, y1}, {x0, y1}};
    } else if (g == "trapezoid" || g == "flowChartManualOperation") {
        const float k = r.w * 0.2f;
        pts = {{x0, y0}, {x1, y0}, {x1 - k, y1}, {x0 + k, y1}};
    } else if (g == "hexagon" || g == "flowChartPreparation") {
        const float k = r.w * 0.25f;
        pts = {{x0 + k, y0}, {x1 - k, y0}, {x1, cy}, {x1 - k, y1}, {x0 + k, y1}, {x0, cy}};
    } else if (g == "octagon") {
        const float k = std::min(r.w, r.h) * 0.29f;
        pts = {{x0 + k, y0}, {x1 - k, y0}, {x1, y0 + k}, {x1, y1 - k}, {x1 - k, y1}, {x0 + k, y1}, {x0, y1 - k}, {x0, y0 + k}};
    } else if (g == "pentagon") {
        polygon(pts, r, 5);
    } else if (g == "chevron" || g == "homePlate") {
        const float k = std::min(r.w, r.h) * 0.5f;
        pts = {{x0, y0}, {x1 - k, y0}, {x1, cy}, {x1 - k, y1}, {x0, y1}};
        if (g == "chevron")
            pts.push_back({x0 + k, cy});
    } else if (g == "heptagon") {
        polygon(pts, r, 7);
    } else if (g == "decagon") {
        polygon(pts, r, 10);
    } else if (g == "star4") {
        star(pts, r, 4, 0.38f);
    } else if (g == "star5") {
        star(pts, r, 5, 0.38f);
    } else if (g == "star6") {
        star(pts, r, 6, 0.5f);
    } else if (g == "star8" || g == "star10" || g == "star12" || g == "star16" || g == "star24" || g == "star32" || g == "irregularSeal1" ||
               g == "irregularSeal2") {
        star(pts, r, g == "star8" ? 8 : g == "star10" ? 10 : g == "star12" ? 12 : 16, 0.75f);
    } else if (g == "plus") {
        const float kx = r.w * 0.25f, ky = r.h * 0.25f;
        pts = {{x0 + kx, y0}, {x1 - kx, y0}, {x1 - kx, y0 + ky}, {x1, y0 + ky}, {x1, y1 - ky}, {x1 - kx, y1 - ky},
               {x1 - kx, y1}, {x0 + kx, y1}, {x0 + kx, y1 - ky}, {x0, y1 - ky}, {x0, y0 + ky}, {x0 + kx, y0 + ky}};
    } else if (g == "rightArrow" || g == "notchedRightArrow" || g == "stripedRightArrow") {
        arrow(pts, r, 0, false);
    } else if (g == "downArrow") {
        arrow(pts, r, 1, false);
    } else if (g == "leftArrow") {
        arrow(pts, r, 2, false);
    } else if (g == "upArrow") {
        arrow(pts, r, 3, false);
    } else if (g == "leftRightArrow") {
        arrow(pts, r, 0, true);
    } else if (g == "upDownArrow") {
        arrow(pts, r, 1, true);
    } else if (g == "wedgeRectCallout" || g == "flowChartProcess" || g == "flowChartPredefinedProcess" || g == "flowChartInternalStorage") {
        pts = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    } else if (g == "flowChartDocument" || g == "flowChartMultidocument") {
        pts = {{x0, y0}, {x1, y0}, {x1, y1 - r.h * 0.12f}};
        for (int i = 0; i <= 12; ++i) { // a wave along the bottom
            const float t = float(i) / 12.0f;
            pts.push_back({x1 - r.w * t, y1 - r.h * 0.12f + r.h * 0.1f * std::sin(t * 2.0f * kPi)});
        }
    } else if (g == "can" || g == "flowChartMagneticDisk") {
        rounded(pts, r, std::min(r.w * 0.5f, r.h * 0.15f));
    } else {
        pts = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
    }
}

bool shape(XmlReader& x, const ooxml::Theme& theme, Rect box, Page& page)
{
    std::string geometry = "rect";
    std::uint32_t fill = 0, line = 0, styleFill = 0, styleLine = 0, fontColor = 0;
    bool hasFill = false, hasLine = false, hasStyleFill = false, hasStyleLine = false, flipH = false, flipV = false;
    float rotation = 0.0f;
    float lineWidth = 0.75f;
    Gradient gradient;
    LineStyle lineStyle;
    float textTurn = 0.0f; // vertical text: 90 or 270
    TextBox text;
    text.box = box;
    text.anchor = 0;
    bool hasText = false;
    const int depth = x.depth();
    for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
        if (e == Event::End && x.depth() < depth)
            break;
        if (e != Event::Start)
            continue;
        const std::string_view n = x.localName();
        if (n == "xfrm") {
            flipH = on(x, "flipH");
            flipV = on(x, "flipV");
            rotation = float(std::atof(x.attribute("rot").c_str()) / 60000.0);
        } else if (n == "prstGeom") {
            geometry = x.attribute("prst");
        } else if (n == "noFill" && x.depth() == depth + 2) { // spPr's own (not a line's)
            hasFill = true;
            fill = 0;
        } else if (n == "solidFill" && x.depth() == depth + 2) {
            bool found = false;
            fill = ooxml::readFillIn(x, theme, nullptr, &found);
            hasFill = found;
            gradient = Gradient();
        } else if (n == "gradFill" && x.depth() == depth + 2) {
            fill = ooxml::readGradientFill(x, theme, nullptr, gradient);
            hasFill = !gradient.stops.empty();
        } else if (n == "ln" && x.depth() == depth + 2) {
            readLine(x, theme, nullptr, lineStyle);
            if (lineStyle.hasWidth)
                lineWidth = lineStyle.width;
            if (lineStyle.hasColor) {
                hasLine = true;
                line = lineStyle.color;
            }
        } else if (n == "fillRef" || n == "lnRef" || n == "fontRef") {
            const int idx = std::atoi(x.attribute("idx").c_str());
            const bool font = n == "fontRef";
            bool found = false;
            const std::uint32_t c = ooxml::readFillIn(x, theme, nullptr, &found);
            if (!found)
                continue;
            if (font)
                fontColor = c;
            else if (n == "fillRef" && idx > 0)
                styleFill = c, hasStyleFill = true;
            else if (n == "lnRef" && idx > 0)
                styleLine = c, hasStyleLine = true;
        } else if (n == "bodyPr") {
            const std::string a = x.attribute("anchor");
            text.anchor = a == "ctr" ? 1 : a == "b" ? 2 : 0;
            auto inset = [&](const char* name, float& v) {
                if (x.hasAttribute(name))
                    v = float(std::atof(x.attribute(name).c_str()) / kEmu);
            };
            inset("lIns", text.insetL);
            inset("tIns", text.insetT);
            inset("rIns", text.insetR);
            inset("bIns", text.insetB);
            text.wrap = x.attribute("wrap") != "none";
            const std::string vert = x.attribute("vert");
            textTurn = vert == "vert" || vert == "eaVert" || vert == "mongolianVert" ? 90.0f : vert == "vert270" ? 270.0f : 0.0f;
            x.skip();
        } else if (n == "p") {
            TextParagraph para;
            const int d2 = x.depth();
            TextRun* run = nullptr;
            for (Event f; (f = x.next()) != Event::Eof && f != Event::Error;) {
                if (f == Event::End && x.depth() < d2)
                    break;
                if (f != Event::Start)
                    continue;
                const std::string_view m = x.localName();
                if (m == "pPr") {
                    const std::string a = x.attribute("algn");
                    para.align = a == "ctr" ? 1 : a == "r" ? 2 : 0;
                } else if (m == "r" || m == "fld") {
                    para.runs.emplace_back();
                    run = &para.runs.back();
                    run->size = 11.0f;
                    run->color = fontColor ? fontColor : theme.color("dk1", kBlack);
                    run->font = theme.minorFont;
                } else if (m == "rPr" && run) {
                    if (x.hasAttribute("sz"))
                        run->size = std::max(1.0f, float(std::atof(x.attribute("sz").c_str()) / 100.0));
                    if (on(x, "b"))
                        run->flags |= Bold;
                    if (on(x, "i"))
                        run->flags |= Italic;
                    if (!x.attribute("u").empty() && x.attribute("u") != "none")
                        run->flags |= Underline;
                } else if (m == "latin" && run) {
                    const std::string face = x.attribute("typeface");
                    run->font = face.empty() || face.compare(0, 3, "+mn") == 0 ? theme.minorFont
                              : face.compare(0, 3, "+mj") == 0                 ? theme.majorFont
                                                                               : face;
                } else if (m == "solidFill" && run) {
                    bool found = false;
                    const std::uint32_t c = ooxml::readFillIn(x, theme, nullptr, &found);
                    if (found && c)
                        run->color = c;
                } else if (m == "t" && run) {
                    run->text += x.elementText();
                } else if (m == "br") {
                    TextRun br;
                    br.text = "\n";
                    para.runs.push_back(br);
                    run = nullptr;
                }
            }
            if (para.runs.empty()) { // an empty line keeps its height
                TextRun empty;
                empty.size = 11.0f;
                para.runs.push_back(empty);
            } else {
                hasText = true;
            }
            text.paragraphs.push_back(std::move(para));
        }
    }
    const std::uint32_t fillColor = hasFill ? fill : hasStyleFill ? styleFill : 0;
    const std::uint32_t lineColor = hasLine ? line : hasStyleLine ? styleLine : 0;
    bool drawn = false;
    if (isLine(geometry)) {
        PathItem p;
        std::vector<Point> ends;
        connectorPath(geometry, box, flipH, flipV, ends);
        rotate(ends, box, rotation);
        p.subpaths.push_back(std::move(ends));
        p.closed.push_back(false);
        p.stroke = lineColor ? lineColor : kBlack;
        p.lineWidth = lineWidth;
        lineStyle.width = lineWidth;
        p.dash = lineStyle.pattern();
        p.head = lineStyle.head;
        p.tail = lineStyle.tail;
        page.add(std::move(p));
        drawn = true;
    } else if (fillColor || lineColor) {
        PathItem p;
        std::vector<Point> pts;
        presetOutline(geometry, box, pts);
        if (flipH || flipV)
            for (Point& q : pts) {
                if (flipH)
                    q.x = box.x + box.x + box.w - q.x;
                if (flipV)
                    q.y = box.y + box.y + box.h - q.y;
            }
        rotate(pts, box, rotation);
        p.subpaths.push_back(std::move(pts));
        p.closed.push_back(true);
        p.fill = fillColor;
        p.gradient = hasFill ? gradient : Gradient();
        p.stroke = lineColor;
        p.lineWidth = lineWidth;
        lineStyle.width = lineWidth;
        p.dash = lineStyle.pattern();
        page.add(std::move(p));
        drawn = true;
    }
    if (hasText) {
        // Text turns with its shape; vertical text is laid out across the box and turned a quarter.
        text.box = textTurn != 0.0f ? turnedBox(box, 90.0f) : box;
        text.rotation = rotation + textTurn;
        page.add(std::move(text));
        drawn = true;
    }
    return drawn;
}

} // namespace enp::model::viewer::dml
