// SmartArt without a drawing of it (written by other programs than PowerPoint): laid out from its
// data model by the kind of layout it names (dgm:layoutDef's uniqueId), as PowerPoint's default
// look draws it: shapes in the theme's first accent with white text, one text size for all.
// Block lists, lists, processes and chevrons, cycles, hierarchies and pyramids; any other layout
// as a block list. Internal to the readers.
#pragma once

#include "Ooxml.h"

#include "enp/model/viewer/Page.h"

#include <string>
#include <string_view>
#include <vector>

namespace enp::model::viewer::smartart {

// The data model's nodes as a tree: [0] is the document (its children the top-level nodes).
struct Node {
    std::string text;           // its paragraphs, a line each
    std::vector<int> children;  // in order
};
struct Tree {
    std::vector<Node> nodes;
};

// Reads a data part (dgm:dataModel). False when it has no document node.
bool readTree(std::string_view dataXml, Tree& out);

// The layout's kind from its uniqueId ("urn:microsoft.com/office/officeart/2005/8/layout/process1").
enum class Kind { Blocks, List, HorizontalList, Process, VerticalProcess, Chevrons, Cycle, Hierarchy, Pyramid };
Kind kindOf(std::string_view layoutId);

// Draws `tree` into `r` as a `kind` layout. False when it has nothing to draw.
bool draw(const Tree& tree, Kind kind, const ooxml::Theme& theme, Rect r, Page& page);

} // namespace enp::model::viewer::smartart
