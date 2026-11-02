// What a compiled library or program contains, shown in place of its bytes: a tree of lists on the
// left (overview, sections, exported functions and data, imports per library, archive members) and
// the chosen list on the right as a sortable, filterable table. C++, Rust and D names are shown
// demangled and marked as such; the stored (mangled) name is a switch or a hover away.
//
// Memory: a file's parsed form (model::bin::Image) and its view state are held only while that
// document is shown here. They are freed when it closes (releaseClosed), when it is opened as text
// (release), and the tables draw only their visible rows.
#pragma once

#include "enp/model/BinaryInfo.h"
#include "enp/model/Document.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "imgui.h"

#include <cstdint>
#include <map>
#include <vector>

namespace enp::view {

class LibraryView {
public:
    LibraryView(viewmodel::NotepadViewModel& vm, const Palette& palette, float uiScale);

    // Draws the file at the cursor, filling `size`. False when it is not a format this reads (ELF,
    // PE/COFF, ar archive): the caller shows its notice instead.
    bool render(model::Document& doc, ImVec2 size);

    void release(int documentId); // opened as text: the parsed form goes at once
    void releaseClosed();         // every entry whose document has closed
    bool holds(int documentId) const { return entries_.count(documentId) != 0; }
    size_t entryCount() const { return entries_.size(); }

private:
    struct Entry {
        int id = -1; // the document's
        model::bin::Image image;
        bool readable = false;
        std::uint32_t list = 0;            // the chosen node of image.lists
        std::vector<std::uint8_t> open;    // per node: its children shown
        std::vector<std::uint32_t> tree;   // the visible nodes in order
        bool treeDirty = true;
        std::vector<std::uint32_t> rows;   // the chosen list's rows (indexes into symbols, sections, ...), filtered and sorted
        bool rowsDirty = true;
        std::uint32_t selected = UINT32_MAX; // the chosen row's index (a symbol, a section, ...)
        bool raw = false;                  // names as stored (mangled) instead of demangled
        char filter[160] = {};
        std::uint32_t usedColumns = ~0u;  // symbol table: a bit per column with a value in the chosen list
        int sortColumn = -1;
        bool sortDescending = false;
    };

    Entry& entryFor(model::Document& doc);
    void rebuildTree(Entry& e);
    void rebuildRows(Entry& e);
    void choose(Entry& e, std::uint32_t list);
    void toolbar(model::Document& doc, Entry& e, float width);
    void treePanel(Entry& e, float height);
    void overview(Entry& e);
    void symbolTable(Entry& e, float height);
    void sectionTable(Entry& e, float height);
    void libraryTable(Entry& e, float height);
    void memberTable(Entry& e, float height);
    void symbolDetails(Entry& e);
    void sortSpecs(Entry& e);
    void resetFor(int documentId);
    // The one table the lists draw into. A list shown for the first time since another was resets the
    // columns to its defaults; columnsReady() goes after the TableSetupColumn calls.
    bool beginTable(const Entry& e, int columns, ImGuiTableFlags flags, ImVec2 size);
    void columnsReady();
    std::uint32_t usedColumns(const Entry& e) const;
    // A small rounded tag ("C++", "MSVC", "Rust", "D") before a demangled name.
    float tag(ImVec2 at, const char* text, ImU32 color);

    viewmodel::NotepadViewModel& vm_;
    const Palette& palette_;
    float s_;
    std::map<int, Entry> entries_; // by document id: documents shown here only
    int shownDoc_ = -1;            // the document the shared windows showed last
    std::uint32_t shownList_ = UINT32_MAX;
    bool freshColumns_ = false;          // the table shows another list than last frame: its columns reset
    std::uint32_t menuRow_ = UINT32_MAX; // the overview line the copy menu is for
    float treeWidth_ = 0.0f;       // the tree's width (a preference, kept)
    char label_[1024] = {};        // scratch for labels drawn this frame (no allocation per frame)
};

} // namespace enp::view
