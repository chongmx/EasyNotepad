// Excel's conditional formatting on the saved values: cell rules, top / bottom, above / below the
// average, text, blanks, errors, duplicates, colour scales, data bars and icon sets.
#include "ConditionalFormat.h"

#include "enp/model/viewer/NumberFormat.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace enp::model::viewer::cf {

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

namespace {

// One end of a range: a cell, or a whole column ("C") or row ("4") from its first or to its last cell.
bool endpoint(std::string_view s, bool last, std::uint32_t& row, std::uint32_t& column)
{
    if (parseReference(s, row, column))
        return true;
    std::string probe(s);
    probe.erase(std::remove(probe.begin(), probe.end(), '$'), probe.end());
    if (probe.empty() || probe.size() > 8)
        return false;
    if (std::all_of(probe.begin(), probe.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; })) {
        std::uint32_t r = 0;
        if (!parseReference(probe + "1", r, column))
            return false;
        row = last ? 1048575u : 0u;
        return true;
    }
    if (std::all_of(probe.begin(), probe.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)) != 0; })) {
        std::uint32_t c = 0;
        if (!parseReference("A" + probe, row, c))
            return false;
        column = last ? 16383u : 0u;
        return true;
    }
    return false;
}

std::uint64_t key(std::uint32_t row, std::uint32_t column) { return (std::uint64_t(row) << 32) | column; }

bool inside(const Merge& m, std::uint32_t r, std::uint32_t c) { return r >= m.row0 && r <= m.row1 && c >= m.col0 && c <= m.col1; }

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

const double* valueAt(const std::vector<Value>& values, std::uint32_t r, std::uint32_t c)
{
    const std::uint64_t k = key(r, c);
    const auto it = std::lower_bound(values.begin(), values.end(), k, [](const Value& v, std::uint64_t at) { return key(v.row, v.column) < at; });
    return it != values.end() && it->row == r && it->column == c ? &it->value : nullptr;
}

// An operand of a rule: a number, a quoted text, or a cell's value (empty: 0).
struct Operand {
    bool ok = false, text = false;
    double number = 0.0;
    std::string str;
};

Operand operand(std::string_view f, const Sheet& sheet, const std::vector<Value>& values)
{
    Operand o;
    while (!f.empty() && (f.front() == ' ' || f.front() == '='))
        f.remove_prefix(1);
    while (!f.empty() && f.back() == ' ')
        f.remove_suffix(1);
    if (f.size() >= 2 && f.front() == '"' && f.back() == '"') {
        o.ok = o.text = true;
        for (std::size_t i = 1; i + 1 < f.size(); ++i) {
            o.str += f[i];
            if (f[i] == '"' && i + 2 < f.size() && f[i + 1] == '"')
                ++i; // "" is one quote
        }
        return o;
    }
    const std::string s(f);
    char* end = nullptr;
    const double v = std::strtod(s.c_str(), &end);
    if (!s.empty() && end && *end == 0) {
        o.ok = true;
        o.number = v;
        return o;
    }
    std::uint32_t r = 0, c = 0;
    if (parseReference(f, r, c)) {
        o.ok = true;
        if (const double* at = valueAt(values, r, c))
            o.number = *at;
        else if (const Cell* cell = sheet.cell(r, c); cell && !cell->text.empty()) {
            o.text = true;
            o.str = cell->text;
        }
    }
    return o;
}

std::uint32_t lerp(std::uint32_t a, std::uint32_t b, double t)
{
    auto ch = [&](int shift) {
        const double x = double((a >> shift) & 0xFF), y = double((b >> shift) & 0xFF);
        return std::uint32_t(std::lround(x + (y - x) * t)) << shift;
    };
    return ch(0) | ch(8) | ch(16) | 0xFF000000u;
}

// Excel's PERCENTILE.INC of sorted numbers.
double percentile(const std::vector<double>& sorted, double p)
{
    if (sorted.empty())
        return 0.0;
    const double rank = std::clamp(p, 0.0, 1.0) * double(sorted.size() - 1);
    const std::size_t i = std::size_t(rank);
    const double frac = rank - double(i);
    return i + 1 < sorted.size() ? sorted[i] + (sorted[i + 1] - sorted[i]) * frac : sorted[i];
}

// A threshold's value among the rule's sorted numbers.
bool threshold(const Cfvo& v, const std::vector<double>& sorted, const Sheet& sheet, const std::vector<Value>& values, double& out)
{
    const double lo = sorted.front(), hi = sorted.back();
    if (v.type == "min" || v.type == "autoMin") {
        out = lo;
    } else if (v.type == "max" || v.type == "autoMax") {
        out = hi;
    } else if (v.type == "percent") {
        out = lo + (hi - lo) * std::atof(v.val.c_str()) / 100.0;
    } else if (v.type == "percentile") {
        out = percentile(sorted, std::atof(v.val.c_str()) / 100.0);
    } else if (v.type == "num" || v.type == "formula") {
        const Operand o = operand(v.val, sheet, values);
        if (!o.ok || o.text)
            return false;
        out = o.number;
    } else {
        return false;
    }
    return true;
}

// Icon `i` (0: the lowest) of a set of `n`.
void iconOf(std::string_view set, int i, CellMark& m)
{
    const std::uint32_t red = rgba(0xE0, 0x40, 0x40), yellow = rgba(0xF0, 0xB4, 0x29), green = rgba(0x3C, 0xA0, 0x50);
    const std::uint32_t gray = rgba(0x80, 0x80, 0x80), black = rgba(0x40, 0x40, 0x40), pink = rgba(0xF0, 0x9C, 0x9C);
    const std::uint32_t gold = rgba(0xF2, 0xB0, 0x1E), blue = rgba(0x3B, 0x6E, 0xC4);
    const bool grey = set.find("Gray") != std::string_view::npos;
    auto pick = [&](std::initializer_list<CellMark::Icon> icons, std::initializer_list<std::uint32_t> colors) {
        const int k = std::clamp(i, 0, int(icons.size()) - 1);
        m.icon = icons.begin()[k];
        m.iconColor = grey ? gray : colors.begin()[std::min(k, int(colors.size()) - 1)];
    };
    using I = CellMark;
    if (set == "3Arrows" || set == "3ArrowsGray")
        pick({I::ArrowDown, I::ArrowRight, I::ArrowUp}, {red, yellow, green});
    else if (set == "4Arrows" || set == "4ArrowsGray")
        pick({I::ArrowDown, I::ArrowDownRight, I::ArrowUpRight, I::ArrowUp}, {red, yellow, yellow, green});
    else if (set == "5Arrows" || set == "5ArrowsGray")
        pick({I::ArrowDown, I::ArrowDownRight, I::ArrowRight, I::ArrowUpRight, I::ArrowUp}, {red, yellow, yellow, yellow, green});
    else if (set == "3Flags")
        pick({I::Flag, I::Flag, I::Flag}, {red, yellow, green});
    else if (set == "4TrafficLights")
        pick({I::Circle, I::Circle, I::Circle, I::Circle}, {black, red, yellow, green});
    else if (set == "4RedToBlack")
        pick({I::Circle, I::Circle, I::Circle, I::Circle}, {black, gray, pink, red});
    else if (set == "3Signs")
        pick({I::Diamond, I::TriangleUp, I::Circle}, {red, yellow, green});
    else if (set == "3Symbols")
        pick({I::CrossCircled, I::ExclamationCircled, I::CheckCircled}, {red, yellow, green});
    else if (set == "3Symbols2")
        pick({I::Cross, I::Exclamation, I::Check}, {red, yellow, green});
    else if (set == "3Triangles")
        pick({I::TriangleDown, I::Dash, I::TriangleUp}, {red, yellow, green});
    else if (set == "3Stars") {
        m.icon = I::Star;
        m.iconColor = gold;
        m.level = std::uint8_t(std::clamp(i, 0, 2));
    } else if (set == "4Rating" || set == "5Rating" || set == "5Quarters" || set == "5Boxes") {
        m.icon = set == "5Quarters" ? I::Quarters : set == "5Boxes" ? I::Boxes : I::Rating;
        m.iconColor = set == "5Quarters" ? black : blue;
        m.level = std::uint8_t(std::clamp(set == "4Rating" ? i + 1 : i, 0, 4));
    } else // 3TrafficLights1, 3TrafficLights2 and anything unknown
        pick({I::Circle, I::Circle, I::Circle}, {red, yellow, green});
}

struct Target {
    Cell* cell;
    std::uint32_t row;
    const double* value; // a number's saved value
};

} // namespace

void parseRanges(std::string_view sqref, std::vector<Merge>& out)
{
    std::size_t i = 0;
    while (i < sqref.size()) {
        while (i < sqref.size() && sqref[i] == ' ')
            ++i;
        std::size_t j = i;
        while (j < sqref.size() && sqref[j] != ' ')
            ++j;
        const std::string_view part = sqref.substr(i, j - i);
        i = j;
        if (part.empty() || out.size() >= 256)
            continue;
        Merge m;
        const std::size_t colon = part.find(':');
        const bool ok = colon == std::string_view::npos
                            ? endpoint(part, false, m.row0, m.col0) && endpoint(part, true, m.row1, m.col1)
                            : endpoint(part.substr(0, colon), false, m.row0, m.col0) && endpoint(part.substr(colon + 1), true, m.row1, m.col1);
        if (!ok)
            continue;
        if (m.row1 < m.row0)
            std::swap(m.row0, m.row1);
        if (m.col1 < m.col0)
            std::swap(m.col0, m.col1);
        out.push_back(m);
    }
}

int apply(std::vector<Rule>& rules, const std::vector<Dxf>& dxfs, const std::vector<Value>& values, Sheet& sheet, bool date1904)
{
    int unsupported = 0;
    std::stable_sort(rules.begin(), rules.end(), [](const Rule& a, const Rule& b) { return a.priority < b.priority; });
    // Per cell: what a rule before set (kept from the rules after it), and whether its rules stopped.
    enum : std::uint8_t { SetFill = 1, SetColor = 2, SetFont = 4, SetFormat = 8, SetBar = 16, SetIcon = 32, Stopped = 128 };
    std::map<std::uint64_t, std::uint8_t> done;
    std::map<std::uint64_t, CellMark> marks;
    std::vector<Target> targets;
    std::vector<double> sorted;
    static const char* const kKnown[] = {"cellIs", "top10", "aboveAverage", "containsText", "notContainsText", "beginsWith", "endsWith",
                                         "containsBlanks", "notContainsBlanks", "containsErrors", "notContainsErrors", "duplicateValues",
                                         "uniqueValues", "colorScale", "dataBar", "iconSet"};
    for (const Rule& rule : rules) {
        if (std::none_of(std::begin(kKnown), std::end(kKnown), [&](const char* k) { return rule.type == k; })) {
            ++unsupported; // expressions, time periods
            continue;
        }
        // The cells in the rule's ranges (once each), and their numbers.
        targets.clear();
        sorted.clear();
        for (std::size_t k = 0; k < rule.ranges.size(); ++k) {
            const Merge& m = rule.ranges[k];
            auto row = std::lower_bound(sheet.rows.begin(), sheet.rows.end(), m.row0, [](const Row& r, std::uint32_t i) { return r.index < i; });
            for (; row != sheet.rows.end() && row->index <= m.row1; ++row) {
                auto c = std::lower_bound(row->cells.begin(), row->cells.end(), m.col0, [](const Cell& cell, std::uint32_t i) { return cell.column < i; });
                for (; c != row->cells.end() && c->column <= m.col1; ++c) {
                    bool seen = false;
                    for (std::size_t p = 0; p < k && !seen; ++p)
                        seen = inside(rule.ranges[p], row->index, c->column);
                    if (seen)
                        continue;
                    const double* v = (c->flags & Cell::Number) ? valueAt(values, row->index, c->column) : nullptr;
                    targets.push_back(Target{&*c, row->index, v});
                    if (v)
                        sorted.push_back(*v);
                }
            }
        }
        if (targets.empty())
            continue;
        std::sort(sorted.begin(), sorted.end());
        const Dxf* dxf = rule.dxf >= 0 && std::size_t(rule.dxf) < dxfs.size() ? &dxfs[std::size_t(rule.dxf)] : nullptr;
        // What the rule's format sets, where a rule before has not set it.
        auto format = [&](const Target& t) {
            std::uint8_t& set = done[key(t.row, t.cell->column)];
            if (set & Stopped)
                return;
            if (dxf) {
                Cell& c = *t.cell;
                if (dxf->hasFill && !(set & SetFill)) {
                    c.fill = dxf->fill;
                    c.fillStyle = 0;
                    set |= SetFill;
                }
                if (dxf->hasColor && !(set & SetColor)) {
                    c.color = dxf->color;
                    set |= SetColor;
                }
                if ((dxf->setFlags || dxf->clearFlags) && !(set & SetFont)) {
                    c.flags = std::uint8_t((c.flags & ~dxf->clearFlags) | dxf->setFlags);
                    set |= SetFont;
                }
                if (!dxf->format.empty() && t.value && !(set & SetFormat)) {
                    std::uint32_t tag = 0;
                    c.text = formatNumber(*t.value, dxf->format, date1904, &tag);
                    if (tag && !(set & SetColor))
                        c.color = tag;
                    set |= SetFormat;
                }
            }
            if (rule.stop)
                set |= Stopped;
        };
        const std::string& type = rule.type;
        if (type == "cellIs") {
            const bool range = rule.op == "between" || rule.op == "notBetween";
            const Operand a = rule.formulas.empty() ? Operand() : operand(rule.formulas[0], sheet, values);
            const Operand b = rule.formulas.size() > 1 ? operand(rule.formulas[1], sheet, values) : Operand();
            if (!a.ok || (range && (!b.ok || a.text || b.text))) {
                ++unsupported;
                continue;
            }
            const std::string needle = a.text ? lower(a.str) : std::string();
            for (const Target& t : targets) {
                bool match = false;
                if (range) {
                    if (!t.value)
                        continue;
                    const double lo = std::min(a.number, b.number), hi = std::max(a.number, b.number);
                    const bool in = *t.value >= lo && *t.value <= hi;
                    match = rule.op == "between" ? in : !in;
                } else {
                    // Text sorts after every number, as in Excel.
                    int cmp;
                    if (a.text)
                        cmp = t.value ? -1 : lower(t.cell->text).compare(needle);
                    else
                        cmp = !t.value ? 1 : *t.value < a.number ? -1 : *t.value > a.number ? 1 : 0;
                    const std::string& op = rule.op;
                    match = op == "lessThan" ? cmp < 0 : op == "lessThanOrEqual" ? cmp <= 0 : op == "equal" ? cmp == 0
                          : op == "notEqual" ? cmp != 0 : op == "greaterThanOrEqual" ? cmp >= 0 : op == "greaterThan" ? cmp > 0 : false;
                }
                if (match)
                    format(t);
            }
        } else if (type == "top10") {
            if (sorted.empty())
                continue;
            const std::size_t count = sorted.size();
            std::size_t n = rule.percent ? std::size_t(std::floor(double(count) * rule.rank / 100.0)) : std::size_t(std::max(0, rule.rank));
            n = std::clamp<std::size_t>(n, 1, count);
            const double cut = rule.bottom ? sorted[n - 1] : sorted[count - n];
            for (const Target& t : targets)
                if (t.value && (rule.bottom ? *t.value <= cut : *t.value >= cut))
                    format(t);
        } else if (type == "aboveAverage") {
            if (sorted.empty())
                continue;
            double mean = 0.0, var = 0.0;
            for (double v : sorted)
                mean += v;
            mean /= double(sorted.size());
            for (double v : sorted)
                var += (v - mean) * (v - mean);
            const double sd = std::sqrt(var / double(sorted.size()));
            const double limit = mean + (rule.above ? 1.0 : -1.0) * double(rule.stdDev) * sd;
            for (const Target& t : targets)
                if (t.value && (rule.above ? (rule.equal ? *t.value >= limit : *t.value > limit) : (rule.equal ? *t.value <= limit : *t.value < limit)))
                    format(t);
        } else if (type == "containsText" || type == "notContainsText" || type == "beginsWith" || type == "endsWith") {
            const std::string needle = lower(rule.text);
            for (const Target& t : targets) {
                const std::string hay = lower(t.cell->text);
                const bool match = type == "containsText"      ? hay.find(needle) != std::string::npos
                                 : type == "notContainsText" ? hay.find(needle) == std::string::npos
                                 : type == "beginsWith"      ? hay.compare(0, needle.size(), needle) == 0
                                                             : hay.size() >= needle.size() && hay.compare(hay.size() - needle.size(), needle.size(), needle) == 0;
                if (match)
                    format(t);
            }
        } else if (type == "containsBlanks" || type == "notContainsBlanks" || type == "containsErrors" || type == "notContainsErrors") {
            for (const Target& t : targets) {
                const bool blank = t.cell->text.empty(), error = (t.cell->flags & Cell::Error) != 0;
                if (type == "containsBlanks" ? blank : type == "notContainsBlanks" ? !blank : type == "containsErrors" ? error : !error)
                    format(t);
            }
        } else if (type == "duplicateValues" || type == "uniqueValues") {
            std::map<std::string, int> counts;
            for (const Target& t : targets)
                if (!t.cell->text.empty())
                    ++counts[lower(t.cell->text)];
            for (const Target& t : targets)
                if (!t.cell->text.empty() && (counts[lower(t.cell->text)] > 1) == (type == "duplicateValues"))
                    format(t);
        } else if (type == "colorScale" || type == "dataBar" || type == "iconSet") {
            if (sorted.empty())
                continue;
            std::vector<double> at(rule.cfvos.size(), 0.0);
            bool ok = rule.cfvos.size() >= 2 || (type == "dataBar" && rule.cfvos.empty());
            for (std::size_t k = 0; ok && k < rule.cfvos.size(); ++k)
                ok = threshold(rule.cfvos[k], sorted, sheet, values, at[k]);
            if (!ok || (type == "colorScale" && rule.colors.size() < rule.cfvos.size())) {
                ++unsupported;
                continue;
            }
            for (const Target& t : targets) {
                if (!t.value)
                    continue;
                std::uint8_t& set = done[key(t.row, t.cell->column)];
                if (set & Stopped)
                    continue;
                const double v = *t.value;
                if (type == "colorScale") {
                    if (set & SetFill)
                        continue;
                    std::uint32_t color = rule.colors.back();
                    if (v <= at.front()) {
                        color = rule.colors.front();
                    } else {
                        for (std::size_t k = 1; k < at.size(); ++k)
                            if (v <= at[k]) {
                                const double span = at[k] - at[k - 1];
                                color = lerp(rule.colors[k - 1], rule.colors[k], span > 0.0 ? (v - at[k - 1]) / span : 1.0);
                                break;
                            }
                    }
                    t.cell->fill = color;
                    t.cell->fillStyle = 0;
                    set |= SetFill;
                } else {
                    CellMark& mark = marks[key(t.row, t.cell->column)];
                    mark.row = t.row;
                    mark.column = t.cell->column;
                    if (type == "dataBar" && !(set & SetBar)) {
                        // Excel 2007's bars: from a tenth of the cell (the lowest) to nine tenths (the highest).
                        const double lo = at.empty() ? sorted.front() : at.front(), hi = at.empty() ? sorted.back() : at.back();
                        const double share = hi > lo ? std::clamp((v - lo) / (hi - lo), 0.0, 1.0) : 1.0;
                        mark.bar = float(0.1 + 0.8 * share);
                        mark.barColor = rule.colors.empty() ? rgba(0x63, 0x8E, 0xC6) : rule.colors.front();
                        set |= SetBar;
                    } else if (type == "iconSet" && !(set & SetIcon)) {
                        int i = 0;
                        for (std::size_t k = 1; k < at.size(); ++k)
                            if (rule.cfvos[k].gte ? v >= at[k] : v > at[k])
                                i = int(k);
                        if (rule.reverse)
                            i = int(at.size()) - 1 - i;
                        iconOf(rule.iconSet.empty() ? "3TrafficLights1" : rule.iconSet, i, mark);
                        set |= SetIcon;
                    }
                    if (!rule.showValue)
                        std::string().swap(t.cell->text);
                }
                if (rule.stop)
                    set |= Stopped;
            }
        }
    }
    sheet.marks.reserve(marks.size());
    for (const auto& [k, mark] : marks) // by row, then column: the map's order
        sheet.marks.push_back(mark);
    return unsupported;
}

} // namespace enp::model::viewer::cf
