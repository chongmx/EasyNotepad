// DrawingML shapes the OOXML readers share: preset geometry as outlines, and a shape element
// (xdr:sp, wps:wsp) read and drawn into a page. PowerPoint's own reader adds placeholders and
// master styles on top. Internal to the readers.
#pragma once

#include "Ooxml.h"

#include "enp/model/viewer/Page.h"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::viewer::dml {

// The outline of preset shape `prst` filling `r`, closed (rect for presets not known here).
// Lines and connectors are not outlines: isLine() says so and they are drawn corner to corner.
void presetOutline(std::string_view prst, Rect r, std::vector<Point>& out);
bool isLine(std::string_view prst);
// The path of line or connector `prst` across `r` from its top left to its bottom right (flipped
// as the shape says): straight, bent at right angles (bentConnector2-5) or curved (curvedConnector2-5).
void connectorPath(std::string_view prst, Rect r, bool flipH, bool flipV, std::vector<Point>& out);
// Turns `points` by `degrees` clockwise about the centre of `r` (DrawingML's rot, in degrees).
void rotate(std::vector<Point>& points, Rect r, float degrees);
// The rectangle a box turned by a quarter turn covers (same centre, sides swapped); `r` otherwise.
Rect turnedBox(Rect r, float degrees);

// A DrawingML line (a:ln): colour, width, dashes and ends.
struct LineStyle {
    bool hasColor = false; // noFill counts: color 0
    std::uint32_t color = 0;
    float width = 0.75f;   // points
    bool hasWidth = false;
    std::string dash;      // prstDash's value ("dash", "sysDot", ...); empty: solid
    LineEnd head = LineEnd::None, tail = LineEnd::None;
    // The dash pattern in points for this width (empty: solid).
    std::vector<float> pattern() const;
};
// At the Start of a:ln: reads it into `out` (to its End).
void readLine(ooxml::XmlReader& x, const ooxml::Theme& theme, const std::map<std::string, std::string>* schemeMap, LineStyle& out);

// Where a group's children go: a child at (x, y) EMU of the group's own coordinates is at
// (x0 + (x - chX) * sx, y0 + (y - chY) * sy) points; its size in EMU times sx, sy.
struct Placement {
    float x0 = 0.0f, y0 = 0.0f;
    double chX = 0.0, chY = 0.0;
    double sx = 1.0 / 12700.0, sy = 1.0 / 12700.0;
    // A group inside this one (its xfrm: off, ext, chOff, chExt in this one's EMU).
    Placement inner(double offX, double offY, double extX, double extY, double chOffX, double chOffY, double chExtX, double chExtY) const
    {
        Placement p;
        p.x0 = x0 + float((offX - chX) * sx);
        p.y0 = y0 + float((offY - chY) * sy);
        p.chX = chOffX;
        p.chY = chOffY;
        p.sx = chExtX > 0.0 ? sx * extX / chExtX : sx;
        p.sy = chExtY > 0.0 ? sy * extY / chExtY : sy;
        return p;
    }
};

// At the Start of custGeom: its paths, normalised to 0..1 of the shape's box, and whether each is closed.
void customGeometry(ooxml::XmlReader& x, std::vector<std::vector<Point>>& paths, std::vector<bool>& closed);

// At the Start of a shape element (sp, wsp, cxnSp): draws its geometry (preset or custom), fill,
// line and text into `page`, filling `box` (points; the anchor's rectangle) or, in a group
// (`place`), the box its own xfrm gives. Returns whether anything was drawn.
bool shape(ooxml::XmlReader& x, const ooxml::Theme& theme, Rect box, Page& page, const Placement* place = nullptr);

} // namespace enp::model::viewer::dml
