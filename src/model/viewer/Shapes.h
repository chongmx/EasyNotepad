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

// At the Start of a shape element (sp, wsp, cxnSp): draws its geometry, fill, line and text into
// `page`, filling `box` (points; the anchor's rectangle). Returns whether anything was drawn.
bool shape(ooxml::XmlReader& x, const ooxml::Theme& theme, Rect box, Page& page);

} // namespace enp::model::viewer::dml
