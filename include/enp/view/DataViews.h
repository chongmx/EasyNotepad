// Structured views of data files, next to their text: CSV as a table; JSON and YAML as a tree with
// the chosen value beside it, or as nested sections that open and close (an accordion).
//
// Memory: a document's parsed form (model::csv::Table, model::data::Tree) and its view state are
// held only while that document is shown in one of these views. They are freed the moment it
// closes (releaseClosed, called from the close), when it goes back to its text (release), and
// before a re-parse after an edit. Each view draws into one shared window, and open/closed state
// lives in the entry's arrays, not in Dear ImGui's per-window storage (which never shrinks).
#pragma once

#include "enp/model/Csv.h"
#include "enp/model/DataTree.h"
#include "enp/model/Document.h"
#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace enp::view {

class DataViews {
public:
    DataViews(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    // Each fills `size` at the cursor (inside the editor's area).
    void renderTable(model::Document& doc, ImVec2 size);
    void renderTree(model::Document& doc, ImVec2 size);
    void renderAccordion(model::Document& doc, ImVec2 size);

    // The view switch of CSV (Text | Table) and JSON/YAML (Text | Tree | Accordion), right-aligned
    // at `topRight`; drawn in the views' toolbars and over the text editor.
    static bool hasSwitch(const model::Document& doc);
    ImRect switchRect(const model::Document& doc, ImVec2 topRight) const;
    void viewSwitch(model::Document& doc, ImVec2 topRight);

    void release(int documentId); // back to the text: the parsed form goes at once
    void releaseClosed();         // every entry whose document has closed
    bool holds(int documentId) const { return entries_.count(documentId) != 0; }
    size_t entryCount() const { return entries_.size(); }

private:
    struct Entry {
        std::uint64_t revision = UINT64_MAX;
        bool table = false;
        model::csv::Table csv;
        model::data::Tree tree;
        // Table
        bool header = true;
        std::vector<std::uint32_t> order; // data rows sorted by a column; empty: file order
        // Tree and accordion
        std::vector<std::uint8_t> open;   // per node
        std::vector<std::uint32_t> rows;  // tree: the visible nodes in order
        std::vector<std::uint16_t> depth; // tree: per visible row
        bool rowsDirty = true;
        std::uint32_t selected = 0;
        bool scrollToSelected = false;
        std::unordered_map<std::uint32_t, std::uint32_t> more; // accordion: extra children shown
    };
    static constexpr std::uint32_t kShownChildren = 200; // accordion: children listed before "show more"

    Entry& entryFor(model::Document& doc, bool table);
    // A toolbar row: what was read on the left, the switch on the right. Returns its height.
    float toolbar(model::Document& doc, const char* summary, float width);
    bool parseError(model::Document& doc, const Entry& e, ImVec2 size);
    void rebuildRows(Entry& e);
    void select(Entry& e, std::uint32_t node); // opens its ancestors, scrolls it into view
    void sortTable(Entry& e, const ImGuiTableSortSpecs* specs);
    void accordionChildren(Entry& e, std::uint32_t node, int depth);
    ImU32 valueColor(model::data::Kind kind) const;
    void resetScrollFor(int documentId);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    std::map<int, Entry> entries_; // by document id: documents shown in one of these views only
    int shownDoc_ = -1;            // the document the shared windows showed last
    float treeWidth_ = 0.0f;       // tree view: the list's width (a preference, kept)
    char label_[512] = {};         // scratch for labels drawn this frame (no allocation per frame)
};

} // namespace enp::view
