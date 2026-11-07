// What the three OOXML readers share: part names, relationships, the theme and DrawingML colours.
// Internal to the readers.
#pragma once

#include "enp/model/package/Xml.h"
#include "enp/model/package/Zip.h"
#include "enp/model/viewer/Page.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace enp::model::viewer::ooxml {

using package::XmlReader;
using package::ZipArchive;

struct Relationship {
    std::string type;   // the last segment of the type URI ("image", "slideLayout", "hyperlink")
    std::string target; // a part name (resolved, no leading '/'), or the URL when external
    bool external = false;
};
using Relationships = std::map<std::string, Relationship>; // by id

// "word/document.xml" + "media/image1.png" -> "word/media/image1.png"; "../x" climbs; "/x" is absolute.
std::string resolvePart(std::string_view basePart, std::string_view target);
// The relationships of `part` (its _rels/<name>.rels), or of the package when `part` is empty.
Relationships readRelationships(const ZipArchive& zip, std::string_view part);
// The first target of a relationship of `type`, or "".
std::string targetOfType(const Relationships& rels, std::string_view type);
// The main part: the package's officeDocument relationship, else `fallback`.
std::string mainPart(const ZipArchive& zip, std::string_view fallback);

// Colours, packed like model::viewer colours (0xAABBGGRR).
std::uint32_t hexColor(std::string_view rrggbb, std::uint32_t otherwise = 0);
// Excel's tint and DrawingML's lumMod/lumOff/shade/tint, on a colour.
std::uint32_t applyTint(std::uint32_t color, double tint); // -1..1
std::uint32_t lumModOff(std::uint32_t color, double mod, double off);

struct Theme {
    // dk1 lt1 dk2 lt2 accent1..6 hlink folHlink
    std::map<std::string, std::uint32_t> colors;
    std::string majorFont, minorFont;
    std::shared_ptr<const std::string> xml; // the theme part, for its style lists (themeFill); shared by copies
    std::uint32_t color(std::string_view name, std::uint32_t otherwise = 0) const;
};
Theme readTheme(const ZipArchive& zip, std::string_view part);

// A theme's fill style: number `index` (from 0) of its fillStyleLst, or of its bgFillStyleLst when
// `background`, with `placeholder` as its phClr. Sets `color` (a gradient's first stop) and, for a
// gradient fill, `gradient` (else clears it). False when the theme has no such style, or it is a
// picture or pattern fill.
bool themeFill(const Theme& theme, int index, bool background, std::uint32_t placeholder,
               const std::map<std::string, std::string>* schemeMap, std::uint32_t& color, Gradient& gradient);

// DrawingML: at the Start of a colour element (srgbClr, schemeClr, sysClr, prstClr, scrgbClr),
// reads it with its modifiers and leaves the reader at its End. `schemeMap` maps tx1/bg1/... to
// theme names (a master's clrMap); null maps tx1->dk1, bg1->lt1, tx2->dk2, bg2->lt2.
std::uint32_t readColor(XmlReader& x, const Theme& theme, const std::map<std::string, std::string>* schemeMap = nullptr);
// At the Start of a fill-holding element: reads the first solidFill / gradFill colour inside it
// (to its End). `found` says whether a fill element was there at all (noFill gives 0, found).
std::uint32_t readFillIn(XmlReader& x, const Theme& theme, const std::map<std::string, std::string>* schemeMap, bool* found);
// At the Start of a:gradFill: its stops and its direction into `out` (to its End). Returns the first
// stop's colour (what a painter without gradients fills with).
std::uint32_t readGradientFill(XmlReader& x, const Theme& theme, const std::map<std::string, std::string>* schemeMap, Gradient& out);

} // namespace enp::model::viewer::ooxml
