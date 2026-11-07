#include "Ooxml.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace enp::model::viewer::ooxml {

std::string resolvePart(std::string_view basePart, std::string_view target)
{
    std::vector<std::string> parts;
    auto push = [&](std::string_view path) {
        std::size_t start = 0;
        while (start <= path.size()) {
            const std::size_t slash = path.find('/', start);
            const std::string_view seg = path.substr(start, slash == std::string_view::npos ? std::string_view::npos : slash - start);
            if (seg == "..") {
                if (!parts.empty())
                    parts.pop_back();
            } else if (!seg.empty() && seg != ".") {
                parts.emplace_back(seg);
            }
            if (slash == std::string_view::npos)
                break;
            start = slash + 1;
        }
    };
    if (target.empty() || target.front() != '/') {
        const std::size_t slash = basePart.rfind('/');
        if (slash != std::string_view::npos)
            push(basePart.substr(0, slash));
    }
    push(target);
    std::string out;
    for (const std::string& p : parts) {
        if (!out.empty())
            out += '/';
        out += p;
    }
    return out;
}

Relationships readRelationships(const ZipArchive& zip, std::string_view part)
{
    Relationships rels;
    std::string relsName;
    if (part.empty()) {
        relsName = "_rels/.rels";
    } else {
        const std::size_t slash = part.rfind('/');
        relsName = (slash == std::string_view::npos ? std::string() : std::string(part.substr(0, slash + 1))) + "_rels/" +
                   std::string(part.substr(slash == std::string_view::npos ? 0 : slash + 1)) + ".rels";
    }
    const std::optional<std::string> xml = zip.read(relsName, std::size_t(16) << 20);
    if (!xml)
        return rels;
    XmlReader x(*xml);
    for (XmlReader::Event e; (e = x.next()) != XmlReader::Event::Eof && e != XmlReader::Event::Error;) {
        if (e != XmlReader::Event::Start || x.localName() != "Relationship")
            continue;
        Relationship r;
        std::string type = x.attribute("Type");
        const std::size_t slash = type.rfind('/');
        r.type = slash == std::string::npos ? type : type.substr(slash + 1);
        r.external = x.attribute("TargetMode") == "External";
        const std::string target = x.attribute("Target");
        r.target = r.external ? target : resolvePart(part.empty() ? std::string_view() : part, target);
        rels[x.attribute("Id")] = std::move(r);
    }
    return rels;
}

std::string targetOfType(const Relationships& rels, std::string_view type)
{
    for (const auto& [id, r] : rels)
        if (r.type == type)
            return r.target;
    return {};
}

std::string mainPart(const ZipArchive& zip, std::string_view fallback)
{
    const Relationships rels = readRelationships(zip, {});
    for (const auto& [id, r] : rels)
        if (r.type == "officeDocument" && zip.contains(r.target))
            return r.target;
    return std::string(fallback);
}

std::uint32_t hexColor(std::string_view s, std::uint32_t otherwise)
{
    if (s.size() == 8) // AARRGGBB (Excel)
        s.remove_prefix(2);
    if (s.size() != 6)
        return otherwise;
    char* end = nullptr;
    const std::string digits(s);
    const unsigned long v = std::strtoul(digits.c_str(), &end, 16);
    if (*end != 0)
        return otherwise;
    return std::uint32_t((v >> 16) & 0xFF) | (std::uint32_t((v >> 8) & 0xFF) << 8) | (std::uint32_t(v & 0xFF) << 16) | 0xFF000000u;
}

namespace {

void toHsl(std::uint32_t c, double& h, double& s, double& l)
{
    const double r = (c & 0xFF) / 255.0, g = ((c >> 8) & 0xFF) / 255.0, b = ((c >> 16) & 0xFF) / 255.0;
    const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
    l = (mx + mn) / 2.0;
    if (mx == mn) {
        h = s = 0.0;
        return;
    }
    const double d = mx - mn;
    s = l > 0.5 ? d / (2.0 - mx - mn) : d / (mx + mn);
    if (mx == r)
        h = (g - b) / d + (g < b ? 6.0 : 0.0);
    else if (mx == g)
        h = (b - r) / d + 2.0;
    else
        h = (r - g) / d + 4.0;
    h /= 6.0;
}

double hue(double p, double q, double t)
{
    if (t < 0)
        t += 1;
    if (t > 1)
        t -= 1;
    if (t < 1.0 / 6)
        return p + (q - p) * 6 * t;
    if (t < 0.5)
        return q;
    if (t < 2.0 / 3)
        return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

std::uint32_t fromHsl(double h, double s, double l, std::uint32_t alpha)
{
    double r, g, b;
    if (s == 0.0) {
        r = g = b = l;
    } else {
        const double q = l < 0.5 ? l * (1 + s) : l + s - l * s;
        const double p = 2 * l - q;
        r = hue(p, q, h + 1.0 / 3);
        g = hue(p, q, h);
        b = hue(p, q, h - 1.0 / 3);
    }
    auto byte = [](double v) { return std::uint32_t(std::clamp(int(std::lround(v * 255.0)), 0, 255)); };
    return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (alpha & 0xFF000000u);
}

double toLinear(double v)
{
    return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
}

double toSrgb(double v)
{
    return v <= 0.0031308 ? v * 12.92 : 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
}

// Each channel in linear light: a tint moves it toward white (c * v + 1 - v), a shade toward black (c * v).
std::uint32_t linearTintShade(std::uint32_t color, double v, bool tint)
{
    v = std::clamp(v, 0.0, 1.0);
    std::uint32_t out = color & 0xFF000000u;
    for (int shift = 0; shift < 24; shift += 8) {
        double c = toLinear(double((color >> shift) & 0xFFu) / 255.0);
        c = tint ? c * v + (1.0 - v) : c * v;
        out |= std::uint32_t(std::clamp(int(std::lround(toSrgb(c) * 255.0)), 0, 255)) << shift;
    }
    return out;
}

} // namespace

std::uint32_t applyTint(std::uint32_t color, double tint)
{
    if (tint == 0.0)
        return color;
    double h, s, l;
    toHsl(color, h, s, l);
    l = tint < 0 ? l * (1.0 + tint) : l * (1.0 - tint) + tint;
    return fromHsl(h, s, std::clamp(l, 0.0, 1.0), color);
}

std::uint32_t lumModOff(std::uint32_t color, double mod, double off)
{
    double h, s, l;
    toHsl(color, h, s, l);
    return fromHsl(h, s, std::clamp(l * mod + off, 0.0, 1.0), color);
}

std::uint32_t Theme::color(std::string_view name, std::uint32_t otherwise) const
{
    const auto it = colors.find(std::string(name));
    return it == colors.end() ? otherwise : it->second;
}

Theme readTheme(const ZipArchive& zip, std::string_view part)
{
    Theme t;
    // Office's defaults, for a theme that is missing.
    t.colors = {{"dk1", hexColor("000000")},  {"lt1", hexColor("FFFFFF")},     {"dk2", hexColor("44546A")},
                {"lt2", hexColor("E7E6E6")},  {"accent1", hexColor("4472C4")}, {"accent2", hexColor("ED7D31")},
                {"accent3", hexColor("A5A5A5")}, {"accent4", hexColor("FFC000")}, {"accent5", hexColor("5B9BD5")},
                {"accent6", hexColor("70AD47")}, {"hlink", hexColor("0563C1")},  {"folHlink", hexColor("954F72")}};
    if (part.empty())
        return t;
    std::optional<std::string> xml = zip.read(part, std::size_t(16) << 20);
    if (!xml)
        return t;
    t.xml = std::make_shared<const std::string>(std::move(*xml));
    XmlReader x(*t.xml);
    std::string current;
    bool inScheme = false;
    std::string fontKind;
    for (XmlReader::Event e; (e = x.next()) != XmlReader::Event::Eof && e != XmlReader::Event::Error;) {
        if (e == XmlReader::Event::End) {
            if (x.localName() == "clrScheme")
                inScheme = false;
            continue;
        }
        if (e != XmlReader::Event::Start)
            continue;
        const std::string_view n = x.localName();
        if (n == "clrScheme") {
            inScheme = true;
        } else if (inScheme && (n == "srgbClr" || n == "sysClr") && !current.empty()) {
            t.colors[current] = hexColor(n == "srgbClr" ? x.attribute("val") : x.attribute("lastClr"), t.color(current));
        } else if (inScheme) {
            current = std::string(n);
        } else if (n == "majorFont" || n == "minorFont") {
            fontKind = std::string(n);
        } else if (n == "latin" && !fontKind.empty()) {
            (fontKind == "majorFont" ? t.majorFont : t.minorFont) = x.attribute("typeface");
            fontKind.clear();
        }
    }
    return t;
}

bool themeFill(const Theme& theme, int index, bool background, std::uint32_t placeholder,
               const std::map<std::string, std::string>* schemeMap, std::uint32_t& color, Gradient& gradient)
{
    if (!theme.xml || index < 0)
        return false;
    Theme withPlaceholder = theme; // phClr: the colour the style reference names
    withPlaceholder.colors["phClr"] = placeholder;
    const std::string_view list = background ? "bgFillStyleLst" : "fillStyleLst";
    XmlReader x(*theme.xml);
    int listDepth = -1, n = -1;
    for (XmlReader::Event e; (e = x.next()) != XmlReader::Event::Eof && e != XmlReader::Event::Error;) {
        if (e == XmlReader::Event::End && listDepth >= 0 && x.depth() < listDepth)
            return false; // the list ended before the style
        if (e != XmlReader::Event::Start)
            continue;
        if (listDepth < 0) {
            if (x.localName() == list)
                listDepth = x.depth();
            continue;
        }
        if (x.depth() != listDepth + 1)
            continue;
        if (++n != index) {
            x.skip();
            continue;
        }
        gradient = Gradient();
        const std::string_view kind = x.localName();
        if (kind == "gradFill") {
            color = readGradientFill(x, withPlaceholder, schemeMap, gradient);
            return !gradient.stops.empty();
        }
        if (kind == "noFill") {
            color = 0;
            return true;
        }
        if (kind == "solidFill") {
            const int depth = x.depth();
            for (XmlReader::Event f; (f = x.next()) != XmlReader::Event::Eof && f != XmlReader::Event::Error;) {
                if (f == XmlReader::Event::End && x.depth() < depth)
                    break;
                if (f == XmlReader::Event::Start) {
                    color = readColor(x, withPlaceholder, schemeMap);
                    return true;
                }
            }
        }
        return false; // a picture or pattern fill
    }
    return false;
}

std::uint32_t readColor(XmlReader& x, const Theme& theme, const std::map<std::string, std::string>* schemeMap)
{
    const std::string kind(x.localName());
    std::uint32_t c = 0xFF000000u;
    if (kind == "srgbClr") {
        c = hexColor(x.attribute("val"), 0xFF000000u);
    } else if (kind == "sysClr") {
        c = hexColor(x.attribute("lastClr"), x.attribute("val") == "window" ? 0xFFFFFFFFu : 0xFF000000u);
    } else if (kind == "schemeClr") {
        std::string name = x.attribute("val");
        if (schemeMap) {
            const auto it = schemeMap->find(name);
            if (it != schemeMap->end())
                name = it->second;
        }
        if (name == "tx1")
            name = "dk1";
        else if (name == "bg1")
            name = "lt1";
        else if (name == "tx2")
            name = "dk2";
        else if (name == "bg2")
            name = "lt2";
        c = theme.color(name, 0xFF000000u);
    } else if (kind == "prstClr") {
        static const std::map<std::string, const char*> kPreset = {
            {"black", "000000"}, {"white", "FFFFFF"}, {"red", "FF0000"},  {"green", "008000"}, {"blue", "0000FF"},
            {"yellow", "FFFF00"}, {"gray", "808080"}, {"orange", "FFA500"}, {"purple", "800080"}};
        const auto it = kPreset.find(x.attribute("val"));
        c = hexColor(it == kPreset.end() ? "000000" : it->second);
    } else if (kind == "scrgbClr") {
        auto pct = [&](const char* a) { return std::uint32_t(std::clamp(std::atof(x.attribute(a).c_str()) / 100000.0, 0.0, 1.0) * 255.0); };
        c = pct("r") | (pct("g") << 8) | (pct("b") << 16) | 0xFF000000u;
    }
    // The modifiers, in the order written (Office applies them one after another).
    auto hsl = [&](auto change) {
        double h, s, l;
        toHsl(c, h, s, l);
        change(h, s, l);
        c = fromHsl(h - std::floor(h), std::clamp(s, 0.0, 1.0), std::clamp(l, 0.0, 1.0), c);
    };
    const int depth = x.depth();
    for (XmlReader::Event e; (e = x.next()) != XmlReader::Event::Eof && e != XmlReader::Event::Error;) {
        if (e == XmlReader::Event::End && x.depth() < depth)
            break;
        if (e != XmlReader::Event::Start)
            continue;
        const std::string_view n = x.localName();
        const double v = std::atof(x.attribute("val").c_str()) / 100000.0;
        if (n == "lumMod") {
            hsl([&](double&, double&, double& l) { l *= v; });
        } else if (n == "lumOff") {
            hsl([&](double&, double&, double& l) { l += v; });
        } else if (n == "lum") {
            hsl([&](double&, double&, double& l) { l = v; });
        } else if (n == "satMod") {
            hsl([&](double&, double& s, double&) { s *= v; });
        } else if (n == "satOff") {
            hsl([&](double&, double& s, double&) { s += v; });
        } else if (n == "sat") {
            hsl([&](double&, double& s, double&) { s = v; });
        } else if (n == "hueOff") { // in 60,000ths of a degree
            const double turn = std::atof(x.attribute("val").c_str()) / 60000.0 / 360.0;
            hsl([&](double& h, double&, double&) { h += turn; });
        } else if (n == "alphaMod") {
            const double a = double((c >> 24) & 0xFFu) * std::clamp(v, 0.0, 1.0);
            c = (c & 0x00FFFFFFu) | (std::uint32_t(a) << 24);
        } else if (n == "shade" || n == "tint") {
            // DrawingML's tint and shade work in linear light (scRGB), as Office draws them: a tint
            // of 75% takes black to #898989, not to HSL's #404040 (SpreadsheetML's tint is HSL).
            c = linearTintShade(c, v, n == "tint");
        } else if (n == "alpha") {
            c = (c & 0x00FFFFFFu) | (std::uint32_t(std::clamp(v, 0.0, 1.0) * 255.0) << 24);
        }
    }
    return c;
}

std::uint32_t readFillIn(XmlReader& x, const Theme& theme, const std::map<std::string, std::string>* schemeMap, bool* found)
{
    std::uint32_t color = 0;
    bool got = false;
    const int depth = x.depth();
    for (XmlReader::Event e; (e = x.next()) != XmlReader::Event::Eof && e != XmlReader::Event::Error;) {
        if (e == XmlReader::Event::End && x.depth() < depth)
            break;
        if (e != XmlReader::Event::Start)
            continue;
        const std::string_view n = x.localName();
        if (got) {
            x.skip();
            continue;
        }
        if (n == "noFill") {
            got = true;
            color = 0;
            x.skip();
        } else if (n == "srgbClr" || n == "schemeClr" || n == "sysClr" || n == "prstClr" || n == "scrgbClr") {
            color = readColor(x, theme, schemeMap);
            got = true;
        }
    }
    if (found)
        *found = got;
    return color;
}

std::uint32_t readGradientFill(XmlReader& x, const Theme& theme, const std::map<std::string, std::string>* schemeMap, Gradient& out)
{
    out = Gradient();
    out.angle = 90.0f; // a gradient without a direction runs top to bottom
    const int depth = x.depth();
    for (XmlReader::Event e; (e = x.next()) != XmlReader::Event::Eof && e != XmlReader::Event::Error;) {
        if (e == XmlReader::Event::End && x.depth() < depth)
            break;
        if (e != XmlReader::Event::Start)
            continue;
        const std::string_view n = x.localName();
        if (n == "gs") {
            GradientStop stop;
            stop.position = float(std::clamp(std::atof(x.attribute("pos").c_str()) / 100000.0, 0.0, 1.0));
            bool found = false;
            stop.color = readFillIn(x, theme, schemeMap, &found);
            if (found && out.stops.size() < 16)
                out.stops.push_back(stop);
        } else if (n == "lin") {
            out.angle = float(std::atof(x.attribute("ang").c_str()) / 60000.0);
            out.radial = false;
        } else if (n == "path") {
            out.radial = true; // circle, rect and shape paths: from the centre out
        } else if (n == "fillToRect" && out.radial) {
            // The focus rectangle as insets from the bounds' sides: its centre is the gradient's.
            auto inset = [&](const char* a) { return std::atof(x.attribute(a).c_str()) / 100000.0; };
            out.cx = float(std::clamp((inset("l") + 1.0 - inset("r")) * 0.5, -1.0, 2.0));
            out.cy = float(std::clamp((inset("t") + 1.0 - inset("b")) * 0.5, -1.0, 2.0));
        }
    }
    std::stable_sort(out.stops.begin(), out.stops.end(), [](const GradientStop& a, const GradientStop& b) { return a.position < b.position; });
    return out.stops.empty() ? 0 : out.stops.front().color;
}

} // namespace enp::model::viewer::ooxml
