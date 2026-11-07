#include "Chart.h"

#include "enp/model/viewer/NumberFormat.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace enp::model::viewer::chart {
namespace {

using ooxml::XmlReader;
using Event = XmlReader::Event;
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr float kPi = 3.14159265358979f;

// Calls f(localName) for each child element of the element whose Start the reader is at; a child
// f leaves unread is skipped. Returns at the element's End.
template <class F>
void children(XmlReader& x, F f)
{
    const int depth = x.depth();
    for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
        if (e == Event::End && x.depth() < depth)
            return;
        if (e != Event::Start)
            continue;
        f(x.localName());
        if (x.depth() > depth)
            x.skip();
    }
}

// The text of every a:t under the element (a title), or c:v (a cached string).
std::string textIn(XmlReader& x)
{
    std::string out;
    children(x, [&](std::string_view n) {
        if (n == "t" || n == "v")
            out += x.elementText();
        else if (n == "p" && !out.empty())
            out += ' ', out += textIn(x);
        else
            out += textIn(x);
    });
    return out;
}

// A cache of points (strCache, numCache, or a literal): values by c:pt idx.
void readPoints(XmlReader& x, std::vector<std::string>& out)
{
    children(x, [&](std::string_view n) {
        if (n == "ptCount") {
            const long count = std::atol(x.attribute("val").c_str());
            if (count > 0 && count < 100000 && out.size() < std::size_t(count))
                out.resize(std::size_t(count));
        } else if (n == "pt") {
            const long idx = std::atol(x.attribute("idx").c_str());
            std::string v;
            children(x, [&](std::string_view m) {
                if (m == "v")
                    v = x.elementText();
            });
            if (idx >= 0 && idx < 100000) {
                if (out.size() <= std::size_t(idx))
                    out.resize(std::size_t(idx) + 1);
                out[std::size_t(idx)] = std::move(v);
            }
        } else if (n == "lvl") { // a multi-level category: its first level
            if (out.empty())
                readPoints(x, out);
        } else {
            readPoints(x, out); // strRef / numRef / multiLvlStrRef: descend to the cache
        }
    });
}

std::vector<double> numbers(const std::vector<std::string>& in)
{
    std::vector<double> out(in.size(), kNaN);
    for (std::size_t i = 0; i < in.size(); ++i) {
        char* end = nullptr;
        const double v = std::strtod(in[i].c_str(), &end);
        if (end != in[i].c_str() && std::isfinite(v))
            out[i] = v;
    }
    return out;
}

// spPr: its fill and its line's colour.
void readShapeProperties(XmlReader& x, const ooxml::Theme& theme, std::uint32_t& fill, std::uint32_t& line, bool* noLine)
{
    children(x, [&](std::string_view n) {
        if (n == "solidFill" || n == "gradFill") {
            bool found = false;
            const std::uint32_t c = ooxml::readFillIn(x, theme, nullptr, &found);
            if (found && c)
                fill = c;
        } else if (n == "ln") {
            children(x, [&](std::string_view m) {
                if (m == "solidFill") {
                    bool found = false;
                    const std::uint32_t c = ooxml::readFillIn(x, theme, nullptr, &found);
                    if (found && c)
                        line = c;
                } else if (m == "noFill" && noLine) {
                    *noLine = true;
                }
            });
        }
    });
}

void readSeries(XmlReader& x, const ooxml::Theme& theme, Series& s)
{
    std::vector<std::string> categories, values, xs;
    children(x, [&](std::string_view n) {
        if (n == "tx") {
            s.name = textIn(x);
        } else if (n == "spPr") {
            readShapeProperties(x, theme, s.fill, s.line, &s.noLine);
        } else if (n == "cat") {
            readPoints(x, categories);
        } else if (n == "xVal") {
            readPoints(x, xs);
        } else if (n == "val" || n == "yVal") {
            readPoints(x, values);
        } else if (n == "dPt") {
            long idx = -1;
            std::uint32_t fill = 0, line = 0;
            children(x, [&](std::string_view m) {
                if (m == "idx")
                    idx = std::atol(x.attribute("val").c_str());
                else if (m == "spPr")
                    readShapeProperties(x, theme, fill, line, nullptr);
            });
            if (idx >= 0 && fill)
                s.pointFills.push_back({std::size_t(idx), fill});
        }
    });
    s.values = numbers(values);
    s.categories = std::move(categories);
    if (!xs.empty()) {
        s.xs = numbers(xs);
        // Text x values: the points go 1, 2, 3, ... and the texts label them.
        if (std::all_of(s.xs.begin(), s.xs.end(), [](double v) { return std::isnan(v); })) {
            s.categories = std::move(xs);
            s.xs.clear();
        }
    }
}

bool kindOf(std::string_view n, Kind& kind)
{
    if (n == "barChart" || n == "bar3DChart")
        kind = Kind::Column;
    else if (n == "lineChart" || n == "line3DChart" || n == "stockChart")
        kind = Kind::Line;
    else if (n == "areaChart" || n == "area3DChart")
        kind = Kind::Area;
    else if (n == "pieChart" || n == "pie3DChart" || n == "ofPieChart")
        kind = Kind::Pie;
    else if (n == "doughnutChart")
        kind = Kind::Doughnut;
    else if (n == "scatterChart" || n == "bubbleChart")
        kind = Kind::Scatter;
    else if (n == "radarChart")
        kind = Kind::Radar;
    else
        return false;
    return true;
}

void readGroup(XmlReader& x, const ooxml::Theme& theme, Group& g)
{
    const bool pie = g.kind == Kind::Pie || g.kind == Kind::Doughnut;
    g.varyColors = pie; // Excel's default for pies
    children(x, [&](std::string_view n) {
        const std::string val = x.attribute("val");
        if (n == "barDir")
            g.kind = val == "bar" ? Kind::Bar : Kind::Column;
        else if (n == "grouping")
            g.grouping = val == "stacked" ? Grouping::Stacked : val == "percentStacked" ? Grouping::Percent : Grouping::Clustered;
        else if (n == "varyColors")
            g.varyColors = val.empty() || val == "1" || val == "true";
        else if (n == "gapWidth")
            g.gapWidth = std::clamp(float(std::atof(val.c_str())), 0.0f, 500.0f);
        else if (n == "holeSize")
            g.holeSize = std::clamp(float(std::atof(val.c_str())), 10.0f, 90.0f);
        else if (n == "ser" && g.series.size() < 256) {
            g.series.emplace_back();
            readSeries(x, theme, g.series.back());
        }
    });
}

std::uint32_t seriesColor(const ooxml::Theme& theme, std::size_t index)
{
    static const char* const kAccents[] = {"accent1", "accent2", "accent3", "accent4", "accent5", "accent6"};
    const std::uint32_t base = theme.color(kAccents[index % 6], rgba(68, 114, 196));
    const std::size_t round = index / 6;
    if (round == 0)
        return base;
    return round % 2 == 1 ? ooxml::lumModOff(base, 0.6, 0.0) : ooxml::lumModOff(base, 0.8, 0.2);
}

// ---- drawing helpers ------------------------------------------------------------------------------

void rect(Page& page, float x0, float y0, float x1, float y1, std::uint32_t fill, std::uint32_t stroke = 0, float width = 0.75f)
{
    PathItem p;
    p.subpaths.push_back({{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}});
    p.closed.push_back(true);
    p.fill = fill;
    p.stroke = stroke;
    p.lineWidth = width;
    page.add(std::move(p));
}

void line(Page& page, std::vector<Point> points, std::uint32_t color, float width, bool closed = false, std::uint32_t fill = 0)
{
    if (points.size() < 2)
        return;
    PathItem p;
    p.subpaths.push_back(std::move(points));
    p.closed.push_back(closed);
    p.stroke = color;
    p.fill = fill;
    p.lineWidth = width;
    page.add(std::move(p));
}

// A label in a box: one line, aligned (0 left, 1 centre, 2 right), centred vertically.
void label(Page& page, Rect box, const std::string& text, float size, std::uint32_t color, int align, bool bold = false)
{
    if (text.empty())
        return;
    TextBox b;
    b.box = box;
    b.anchor = 1;
    b.insetL = b.insetR = b.insetT = b.insetB = 0.0f;
    b.wrap = false;
    TextParagraph p;
    p.align = align;
    TextRun r;
    r.text = text;
    r.size = size;
    r.color = color;
    r.flags = bold ? Bold : 0;
    p.runs.push_back(std::move(r));
    b.paragraphs.push_back(std::move(p));
    page.add(std::move(b));
}

// A width estimate for text in the UI font: about half an em per character.
float textWidth(const std::string& text, float size)
{
    std::size_t points = 0;
    for (unsigned char c : text)
        if ((c & 0xC0) != 0x80)
            ++points;
    return float(points) * size * 0.55f;
}

// "Nice" axis steps: 1, 2 or 5 times a power of ten.
double niceStep(double range, int ticks)
{
    if (!(range > 0.0))
        return 1.0;
    const double raw = range / std::max(1, ticks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double f = raw / mag;
    return (f <= 1.0 ? 1.0 : f <= 2.0 ? 2.0 : f <= 5.0 ? 5.0 : 10.0) * mag;
}

std::string axisText(double v, const std::string& format, bool percent)
{
    if (std::fabs(v) < 1e-12)
        v = 0.0;
    if (percent)
        return formatNumber(v / 100.0, "0%");
    return formatNumber(v, format.empty() ? std::string_view("General") : std::string_view(format));
}

struct Axis {
    double min = 0.0, max = 1.0, step = 0.2;
    void fit(double lo, double hi, int ticks, bool fromZero)
    {
        if (fromZero) {
            lo = std::min(lo, 0.0);
            hi = std::max(hi, 0.0);
        }
        if (!(hi > lo)) {
            hi = lo + 1.0;
            if (lo != 0.0)
                lo -= 1.0;
        }
        step = niceStep(hi - lo, ticks);
        min = std::floor(lo / step) * step;
        max = std::ceil(hi / step) * step;
        if (max - min < step)
            max = min + step;
    }
    float at(double v, float from, float to) const { return from + float((v - min) / (max - min)) * (to - from); }
};

void arc(std::vector<Point>& out, float cx, float cy, float r, float a0, float a1)
{
    const int n = std::max(2, int(std::fabs(a1 - a0) / (kPi / 48.0f)) + 1);
    for (int i = 0; i <= n; ++i) {
        const float a = a0 + (a1 - a0) * float(i) / float(n);
        out.push_back({cx + r * std::sin(a), cy - r * std::cos(a)}); // 0 at twelve o'clock, clockwise
    }
}

} // namespace

bool Chart::empty() const
{
    for (const Group& g : groups)
        for (const Series& s : g.series)
            for (double v : s.values)
                if (!std::isnan(v))
                    return false;
    return true;
}

bool read(const ooxml::ZipArchive& zip, std::string_view part, const ooxml::Theme& theme, Chart& out)
{
    const std::optional<std::string> xml = zip.read(part, std::size_t(32) << 20);
    if (!xml)
        return false;
    XmlReader x(*xml);
    bool autoTitleDeleted = false, sawChart = false;
    int seriesInPlot = 0;
    for (Event e; (e = x.next()) != Event::Eof && e != Event::Error;) {
        if (e != Event::Start)
            continue;
        const std::string_view n = x.localName();
        if (n == "chartSpace")
            continue;
        if (n == "spPr" && !sawChart) { // the chart area's own fill and border
            std::uint32_t fill = 0, border = 0;
            bool noLine = false;
            readShapeProperties(x, theme, fill, border, &noLine);
            if (fill)
                out.background = fill;
            out.border = noLine ? 0 : border ? border : out.border;
            continue;
        }
        if (n != "chart") {
            x.skip();
            continue;
        }
        sawChart = true;
        children(x, [&](std::string_view m) {
            if (m == "title") {
                children(x, [&](std::string_view t) {
                    if (t == "tx")
                        out.title = textIn(x);
                });
                if (out.title.empty())
                    out.title = "\x01"; // a title without text: the series' name (below)
            } else if (m == "autoTitleDeleted") {
                const std::string v = x.attribute("val");
                autoTitleDeleted = v.empty() || v == "1" || v == "true";
            } else if (m == "plotArea") {
                children(x, [&](std::string_view p) {
                    Kind kind;
                    if (kindOf(p, kind)) {
                        Group g;
                        g.kind = kind;
                        readGroup(x, theme, g);
                        seriesInPlot += int(g.series.size());
                        out.groups.push_back(std::move(g));
                    } else if (p == "valAx" || p == "catAx" || p == "dateAx") {
                        const bool value = p == "valAx";
                        children(x, [&](std::string_view a) {
                            const std::string v = x.attribute("val");
                            if (a == "delete" && (v.empty() || v == "1" || v == "true")) {
                                if (value)
                                    out.valueAxis = false;
                                else
                                    out.categoryAxis = false;
                            } else if (a == "numFmt" && value && out.valueFormat.empty() && x.attribute("sourceLinked") != "1") {
                                out.valueFormat = x.attribute("formatCode");
                            } else if (a == "majorGridlines" && value) {
                                out.gridlines = true;
                            }
                        });
                    }
                });
            } else if (m == "legend") {
                out.legend = 0;
                children(x, [&](std::string_view l) {
                    if (l == "legendPos") {
                        const std::string v = x.attribute("val");
                        out.legend = v == "b" ? 1 : v == "t" ? 2 : v == "l" ? 3 : 0;
                    }
                });
            }
        });
        break;
    }
    if (!sawChart || out.groups.empty())
        return false;
    // A chart without a legend element shows none.
    bool hasLegend = false;
    {
        XmlReader y(*xml);
        for (Event e; (e = y.next()) != Event::Eof && e != Event::Error;)
            if (e == Event::Start && y.localName() == "legend") {
                hasLegend = true;
                break;
            }
    }
    if (!hasLegend)
        out.legend = -1;
    // A title element without text shows the only series' name (Excel's automatic title).
    if (out.title == "\x01")
        out.title = seriesInPlot == 1 ? out.groups[0].series[0].name : std::string();
    else if (out.title.empty() && !autoTitleDeleted && seriesInPlot == 1)
        out.title = out.groups[0].series[0].name;
    return true;
}

void draw(const Chart& chart, const ooxml::Theme& theme, Rect box, Page& page)
{
    if (box.w < 8.0f || box.h < 8.0f)
        return;
    const std::uint32_t textColor = rgba(89, 89, 89), gridColor = rgba(217, 217, 217), axisColor = rgba(191, 191, 191);
    const float scale = std::clamp(std::min(box.w / 360.0f, box.h / 216.0f), 0.6f, 1.6f);
    const float small = 9.0f * scale, titleSize = 13.0f * scale, pad = 6.0f * scale;
    rect(page, box.x, box.y, box.x + box.w, box.y + box.h, chart.background, chart.border);
    float top = box.y + pad, bottom = box.y + box.h - pad, left = box.x + pad, right = box.x + box.w - pad;
    if (!chart.title.empty()) {
        label(page, Rect{left, top, right - left, titleSize * 1.5f}, chart.title, titleSize, textColor, 1);
        top += titleSize * 1.7f;
    }
    if (chart.groups.empty())
        return;

    // The legend: series (or, for a pie, its categories) with their colours.
    const Group& first = chart.groups.front();
    const bool round = first.kind == Kind::Pie || first.kind == Kind::Doughnut;
    struct Entry {
        std::string name;
        std::uint32_t color;
    };
    std::vector<Entry> entries;
    std::size_t seriesIndex = 0;
    auto colorOf = [&](const Series& s, std::size_t index) { return s.fill ? s.fill : seriesColor(theme, index); };
    if (round && !first.series.empty()) {
        const Series& s = first.series.front();
        for (std::size_t i = 0; i < s.values.size() && i < 64; ++i) {
            std::uint32_t c = seriesColor(theme, i);
            for (const auto& [idx, fill] : s.pointFills)
                if (idx == i)
                    c = fill;
            entries.push_back({i < s.categories.size() && !s.categories[i].empty() ? s.categories[i] : std::to_string(i + 1), c});
        }
    } else {
        for (const Group& g : chart.groups)
            for (const Series& s : g.series) {
                const bool lineLike = g.kind == Kind::Line || g.kind == Kind::Scatter || g.kind == Kind::Radar;
                const std::uint32_t c = lineLike && s.line ? s.line : colorOf(s, seriesIndex);
                entries.push_back({s.name.empty() ? "Series " + std::to_string(seriesIndex + 1) : s.name, c});
                ++seriesIndex;
            }
    }
    if (chart.legend >= 0 && !entries.empty()) {
        const float swatch = small * 0.8f, rowH = small * 1.5f;
        if (chart.legend == 0 || chart.legend == 3) { // a column at the right or the left
            float widest = 0.0f;
            for (const Entry& e : entries)
                widest = std::max(widest, textWidth(e.name, small));
            const float w = std::min(widest + swatch + 8.0f * scale, box.w * 0.35f);
            const float x0 = chart.legend == 0 ? right - w : left;
            float y = top + std::max(0.0f, (bottom - top - rowH * float(entries.size())) * 0.5f);
            for (const Entry& e : entries) {
                if (y + rowH > bottom)
                    break;
                rect(page, x0, y + (rowH - swatch) * 0.5f, x0 + swatch, y + (rowH + swatch) * 0.5f, e.color);
                label(page, Rect{x0 + swatch + 4.0f * scale, y, w - swatch - 4.0f * scale, rowH}, e.name, small, textColor, 0);
                y += rowH;
            }
            if (chart.legend == 0)
                right -= w + pad;
            else
                left += w + pad;
        } else { // a row at the bottom or the top
            float total = 0.0f;
            for (const Entry& e : entries)
                total += swatch + 4.0f * scale + textWidth(e.name, small) + 10.0f * scale;
            float x = left + std::max(0.0f, (right - left - total) * 0.5f);
            const float y = chart.legend == 1 ? bottom - rowH : top;
            for (const Entry& e : entries) {
                const float w = swatch + 4.0f * scale + textWidth(e.name, small);
                if (x + w > right)
                    break;
                rect(page, x, y + (rowH - swatch) * 0.5f, x + swatch, y + (rowH + swatch) * 0.5f, e.color);
                label(page, Rect{x + swatch + 4.0f * scale, y, w, rowH}, e.name, small, textColor, 0);
                x += w + 10.0f * scale;
            }
            if (chart.legend == 1)
                bottom -= rowH + pad * 0.5f;
            else
                top += rowH + pad * 0.5f;
        }
    }
    if (right - left < 20.0f || bottom - top < 20.0f)
        return;

    // Pie and doughnut: the first series, slices clockwise from twelve o'clock.
    if (round) {
        if (first.series.empty())
            return;
        const Series& s = first.series.front();
        double total = 0.0;
        for (double v : s.values)
            if (!std::isnan(v) && v > 0.0)
                total += v;
        if (!(total > 0.0))
            return;
        const float cx = (left + right) * 0.5f, cy = (top + bottom) * 0.5f;
        const float r = std::min(right - left, bottom - top) * 0.47f;
        const float hole = first.kind == Kind::Doughnut ? r * first.holeSize / 100.0f : 0.0f;
        float a = 0.0f;
        for (std::size_t i = 0; i < s.values.size(); ++i) {
            const double v = s.values[i];
            if (std::isnan(v) || v <= 0.0)
                continue;
            const float sweep = float(v / total) * 2.0f * kPi;
            std::vector<Point> pts;
            if (hole > 0.0f) {
                arc(pts, cx, cy, r, a, a + sweep);
                std::vector<Point> inner;
                arc(inner, cx, cy, hole, a, a + sweep);
                pts.insert(pts.end(), inner.rbegin(), inner.rend());
            } else {
                pts.push_back({cx, cy});
                arc(pts, cx, cy, r, a, a + sweep);
            }
            line(page, std::move(pts), kWhite, 1.0f * scale, true, entries.size() > i ? entries[i].color : seriesColor(theme, i));
            a += sweep;
        }
        return;
    }

    // Radar: categories around a circle, each series a closed polygon.
    if (first.kind == Kind::Radar) {
        const float cx = (left + right) * 0.5f, cy = (top + bottom) * 0.5f;
        const float r = std::min(right - left, bottom - top) * 0.42f;
        std::size_t n = 0;
        double hi = 0.0;
        for (const Series& s : first.series) {
            n = std::max(n, s.values.size());
            for (double v : s.values)
                if (!std::isnan(v))
                    hi = std::max(hi, v);
        }
        if (n < 3)
            return;
        Axis axis;
        axis.fit(0.0, hi, 4, true);
        for (double t = axis.min + axis.step; t <= axis.max + axis.step * 0.01; t += axis.step) {
            std::vector<Point> ring;
            const float rr = r * float((t - axis.min) / (axis.max - axis.min));
            for (std::size_t i = 0; i < n; ++i) {
                const float ang = 2.0f * kPi * float(i) / float(n);
                ring.push_back({cx + rr * std::sin(ang), cy - rr * std::cos(ang)});
            }
            line(page, std::move(ring), gridColor, 0.75f, true);
        }
        for (std::size_t si = 0; si < first.series.size(); ++si) {
            const Series& s = first.series[si];
            std::vector<Point> poly;
            for (std::size_t i = 0; i < s.values.size(); ++i) {
                const double v = std::isnan(s.values[i]) ? 0.0 : s.values[i];
                const float rr = r * float((v - axis.min) / (axis.max - axis.min));
                const float ang = 2.0f * kPi * float(i) / float(n);
                poly.push_back({cx + rr * std::sin(ang), cy - rr * std::cos(ang)});
            }
            line(page, std::move(poly), s.line ? s.line : colorOf(s, si), 2.0f * scale, true);
        }
        const Series& s0 = first.series.front();
        for (std::size_t i = 0; i < n && i < s0.categories.size(); ++i) {
            const float ang = 2.0f * kPi * float(i) / float(n);
            const float lx = cx + (r + small) * std::sin(ang), ly = cy - (r + small) * std::cos(ang);
            const float w = textWidth(s0.categories[i], small) + 4.0f;
            label(page, Rect{lx - w * 0.5f, ly - small * 0.75f, w, small * 1.5f}, s0.categories[i], small, textColor, 1);
        }
        return;
    }

    // Axis charts: column, bar, line, area and scatter, sharing one value axis.
    std::size_t categories = 0;
    double lo = std::numeric_limits<double>::infinity(), hi = -lo, xlo = lo, xhi = -lo;
    bool percent = false, horizontal = false, scatter = false;
    const std::vector<std::string>* names = nullptr;
    for (const Group& g : chart.groups) {
        horizontal = horizontal || g.kind == Kind::Bar;
        scatter = scatter || g.kind == Kind::Scatter;
        for (const Series& s : g.series) {
            categories = std::max(categories, s.values.size());
            if (!names && !s.categories.empty())
                names = &s.categories;
            for (std::size_t i = 0; i < s.xs.size(); ++i)
                if (!std::isnan(s.xs[i])) {
                    xlo = std::min(xlo, s.xs[i]);
                    xhi = std::max(xhi, s.xs[i]);
                }
        }
        if (g.grouping == Grouping::Percent && g.kind != Kind::Line && g.kind != Kind::Scatter) {
            percent = true;
            lo = std::min(lo, 0.0);
            hi = std::max(hi, 100.0);
            continue;
        }
        if (g.grouping == Grouping::Stacked && g.kind != Kind::Scatter) {
            for (std::size_t i = 0; i < categories; ++i) {
                double pos = 0.0, neg = 0.0;
                for (const Series& s : g.series)
                    if (i < s.values.size() && !std::isnan(s.values[i]))
                        (s.values[i] >= 0.0 ? pos : neg) += s.values[i];
                lo = std::min(lo, neg);
                hi = std::max(hi, pos);
            }
            continue;
        }
        for (const Series& s : g.series)
            for (double v : s.values)
                if (!std::isnan(v)) {
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
    }
    if (categories == 0 || !(hi >= lo))
        return;
    Axis value;
    const bool zero = !scatter || chart.groups.front().kind != Kind::Scatter;
    value.fit(lo, hi, 5, zero || lo >= 0.0);
    Axis xAxis;
    if (scatter && xhi >= xlo)
        xAxis.fit(xlo, xhi, 5, false);
    else if (scatter)
        xAxis.fit(1.0, double(categories), 5, false);

    // Room for the value labels (beside or under the plot) and the category labels.
    float widestValue = 0.0f;
    for (double t = value.min; t <= value.max + value.step * 0.01; t += value.step)
        widestValue = std::max(widestValue, textWidth(axisText(t, chart.valueFormat, percent), small));
    const float labelH = small * 1.6f;
    float plotL = left, plotR = right, plotT = top + small * 0.5f, plotB = bottom;
    if (horizontal) {
        float widestCat = 0.0f;
        if (names)
            for (const std::string& c : *names)
                widestCat = std::max(widestCat, textWidth(c, small));
        if (chart.categoryAxis)
            plotL += std::min(widestCat + 6.0f * scale, (right - left) * 0.35f);
        if (chart.valueAxis)
            plotB -= labelH;
    } else {
        if (chart.valueAxis)
            plotL += widestValue + 6.0f * scale;
        if (chart.categoryAxis || scatter)
            plotB -= labelH;
    }
    if (plotR - plotL < 10.0f || plotB - plotT < 10.0f)
        return;

    // Gridlines and value labels.
    for (double t = value.min; t <= value.max + value.step * 0.01; t += value.step) {
        const std::string text = axisText(t, chart.valueFormat, percent);
        if (horizontal) {
            const float x = value.at(t, plotL, plotR);
            if (chart.gridlines)
                line(page, {{x, plotT}, {x, plotB}}, gridColor, 0.75f);
            if (chart.valueAxis) {
                const float w = textWidth(text, small) + 4.0f;
                label(page, Rect{x - w * 0.5f, plotB, w, labelH}, text, small, textColor, 1);
            }
        } else {
            const float y = value.at(t, plotB, plotT);
            if (chart.gridlines)
                line(page, {{plotL, y}, {plotR, y}}, gridColor, 0.75f);
            if (chart.valueAxis)
                label(page, Rect{left, y - labelH * 0.5f, plotL - left - 4.0f * scale, labelH}, text, small, textColor, 2);
        }
    }
    // The category axis line, at zero.
    {
        const double base = std::clamp(0.0, value.min, value.max);
        if (horizontal) {
            const float x = value.at(base, plotL, plotR);
            line(page, {{x, plotT}, {x, plotB}}, axisColor, 1.0f);
        } else {
            const float y = value.at(base, plotB, plotT);
            line(page, {{plotL, y}, {plotR, y}}, axisColor, 1.0f);
        }
    }
    // Category labels (every k-th when they do not fit).
    const float slot = (horizontal ? plotB - plotT : plotR - plotL) / float(categories);
    if (chart.categoryAxis && names && !scatter) {
        float widest = 0.0f;
        for (const std::string& c : *names)
            widest = std::max(widest, textWidth(c, small));
        const std::size_t every = horizontal ? std::size_t(std::ceil(labelH / std::max(1.0f, slot)))
                                             : std::size_t(std::ceil((widest + 4.0f) / std::max(1.0f, slot)));
        for (std::size_t i = 0; i < categories && i < names->size(); i += std::max<std::size_t>(1, every)) {
            if (horizontal) { // the first category at the bottom
                const float y = plotB - slot * (float(i) + 0.5f);
                label(page, Rect{left, y - labelH * 0.5f, plotL - left - 4.0f * scale, labelH}, (*names)[i], small, textColor, 2);
            } else {
                const float x = plotL + slot * (float(i) + 0.5f);
                const float w = std::max(slot * float(std::max<std::size_t>(1, every)), widest + 4.0f);
                label(page, Rect{x - w * 0.5f, plotB, w, labelH}, (*names)[i], small, textColor, 1);
            }
        }
    }
    if (scatter) {
        for (double t = xAxis.min; t <= xAxis.max + xAxis.step * 0.01; t += xAxis.step) {
            const float x = xAxis.at(t, plotL, plotR);
            const std::string text = axisText(t, "", false);
            const float w = textWidth(text, small) + 4.0f;
            label(page, Rect{x - w * 0.5f, plotB, w, labelH}, text, small, textColor, 1);
        }
    }

    // The series.
    seriesIndex = 0;
    for (const Group& g : chart.groups) {
        const std::size_t count = g.series.size();
        const float groupW = slot / (1.0f + g.gapWidth / 100.0f);
        std::vector<double> posBase(categories, 0.0), negBase(categories, 0.0);
        std::vector<double> totals(categories, 0.0);
        if (g.grouping == Grouping::Percent)
            for (const Series& s : g.series)
                for (std::size_t i = 0; i < s.values.size(); ++i)
                    if (!std::isnan(s.values[i]))
                        totals[i] += std::fabs(s.values[i]);
        std::vector<double> areaBase(categories, 0.0);
        for (std::size_t si = 0; si < count; ++si, ++seriesIndex) {
            const Series& s = g.series[si];
            const std::uint32_t fill = colorOf(s, seriesIndex);
            const std::uint32_t stroke = s.line ? s.line : fill;
            auto valueAt = [&](std::size_t i) {
                double v = i < s.values.size() ? s.values[i] : kNaN;
                if (!std::isnan(v) && g.grouping == Grouping::Percent)
                    v = totals[i] > 0.0 ? v / totals[i] * 100.0 : 0.0;
                return v;
            };
            if (g.kind == Kind::Column || g.kind == Kind::Bar) {
                const bool stacked = g.grouping != Grouping::Clustered;
                const float barW = stacked ? groupW : groupW / float(std::max<std::size_t>(1, count));
                for (std::size_t i = 0; i < s.values.size(); ++i) {
                    const double v = valueAt(i);
                    if (std::isnan(v))
                        continue;
                    double from = 0.0, to = v;
                    if (stacked) {
                        double& base = v >= 0.0 ? posBase[i] : negBase[i];
                        from = base;
                        to = base + v;
                        base = to;
                    }
                    const float offset = (slot - groupW) * 0.5f + (stacked ? 0.0f : barW * float(si));
                    if (g.kind == Kind::Column) {
                        const float x0 = plotL + slot * float(i) + offset;
                        rect(page, x0, value.at(std::max(from, to), plotB, plotT), x0 + barW, value.at(std::min(from, to), plotB, plotT), fill);
                    } else {
                        const float y1 = plotB - slot * float(i) - offset;
                        rect(page, value.at(std::min(from, to), plotL, plotR), y1 - barW, value.at(std::max(from, to), plotL, plotR), y1, fill);
                    }
                }
            } else if (g.kind == Kind::Line || g.kind == Kind::Area) {
                const bool stacked = g.grouping != Grouping::Clustered;
                std::vector<Point> pts;
                std::vector<Point> lower;
                for (std::size_t i = 0; i < categories; ++i) {
                    double v = valueAt(i);
                    if (std::isnan(v)) {
                        if (g.kind == Kind::Area)
                            v = 0.0;
                        else
                            continue;
                    }
                    const double base = stacked ? areaBase[i] : 0.0;
                    const float x = plotL + slot * (float(i) + 0.5f);
                    pts.push_back({x, value.at(base + v, plotB, plotT)});
                    lower.push_back({x, value.at(base, plotB, plotT)});
                    if (stacked)
                        areaBase[i] = base + v;
                }
                if (g.kind == Kind::Area) {
                    std::vector<Point> poly = pts;
                    poly.insert(poly.end(), lower.rbegin(), lower.rend());
                    line(page, std::move(poly), 0, 0.0f, true, fill);
                } else {
                    line(page, pts, stroke, 2.25f * scale);
                }
            } else if (g.kind == Kind::Scatter) {
                std::vector<Point> pts;
                for (std::size_t i = 0; i < s.values.size(); ++i) {
                    const double v = s.values[i];
                    const double xv = i < s.xs.size() ? s.xs[i] : double(i + 1);
                    if (std::isnan(v) || std::isnan(xv))
                        continue;
                    pts.push_back({xAxis.at(xv, plotL, plotR), value.at(v, plotB, plotT)});
                }
                if (!s.noLine) // a series whose line is switched off (markers only) has noFill
                    line(page, pts, stroke, 1.5f * scale);
                const float m = 2.5f * scale;
                for (const Point& p : pts)
                    rect(page, p.x - m, p.y - m, p.x + m, p.y + m, fill);
            }
        }
    }
}

} // namespace enp::model::viewer::chart
