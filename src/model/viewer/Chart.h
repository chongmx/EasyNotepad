// DrawingML charts (c:chartSpace parts), the same in Word, Excel and PowerPoint: read from the
// values the file keeps with the chart (its caches; nothing is recalculated) and drawn as page
// items (paths and text boxes), so the page painter draws them like any shape. Internal to the
// readers.
#pragma once

#include "Ooxml.h"

#include "enp/model/viewer/Page.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::viewer::chart {

enum class Kind { Column, Bar, Line, Area, Pie, Doughnut, Scatter, Radar };
enum class Grouping { Clustered, Stacked, Percent };

struct Series {
    std::string name;
    std::vector<std::string> categories; // by point index ("" where none)
    std::vector<double> values;          // by point index (NaN where none)
    std::vector<double> xs;              // scatter: x values (empty: 1, 2, 3, ...)
    std::uint32_t fill = 0;              // 0: the theme's colour for the series
    std::uint32_t line = 0;
    bool noLine = false;                 // line and scatter: markers only
    std::vector<std::pair<std::size_t, std::uint32_t>> pointFills; // pie slices with their own colour
};

struct Group {
    Kind kind = Kind::Column;
    Grouping grouping = Grouping::Clustered;
    bool varyColors = false;
    float gapWidth = 150.0f; // % of a bar's width between categories
    float holeSize = 50.0f;  // doughnut: % of the radius
    std::vector<Series> series;
};

struct Chart {
    std::string title;
    std::vector<Group> groups;
    int legend = 0; // -1 none, 0 right, 1 bottom, 2 top, 3 left
    bool valueAxis = true, categoryAxis = true, gridlines = true;
    std::string valueFormat; // number format of the value axis ("" General)
    std::uint32_t background = kWhite;
    std::uint32_t border = rgba(217, 217, 217);
    bool empty() const;
};

// Reads chart part `part`. False when it is not a chart this draws (a chartex part, broken XML).
bool read(const ooxml::ZipArchive& zip, std::string_view part, const ooxml::Theme& theme, Chart& out);
// Appends the chart's drawing to `page`, inside `box` (points).
void draw(const Chart& chart, const ooxml::Theme& theme, Rect box, Page& page);

} // namespace enp::model::viewer::chart
