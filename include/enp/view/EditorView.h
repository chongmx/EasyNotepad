// Custom-drawn code editor for the active document: gutter (line numbers, bookmarks, review
// markers, fold arrows), syntax colouring, selection, find highlights, word wrap, folded regions
// shown as AI summaries, the layers map, and keyboard/mouse editing.
#pragma once

#include "enp/model/Git.h"
#include "enp/view/Fonts.h"
#include "enp/view/LayersView.h"
#include "enp/view/MarkdownView.h"

#include "imgui_internal.h" // ImRect
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace enp::view {

class EditorView {
public:
    EditorView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    // Find bar (when visible) plus the text area, filling `size`.
    void render(ImVec2 size);
    void focus() { focusRequested_ = true; }

private:
    // One visual row: a whole line, a wrapped piece of it, or the summary of a folded region.
    struct Row {
        int line;
        int begin; // byte range within the line
        int end;
        int fold = -1; // summary row: start line of the folded region (`line` is its last line)
    };
    struct Metrics {
        float charW;
        float lineH;
        float gutterW;
        float markerW;
        float foldW; // fold arrows, at the right edge of the gutter
        float blameW = 0.0f;  // git blame column (after the markers), when shown
        float changeW = 0.0f; // git change bars (before the fold arrows), in a repository
        ImVec2 origin;  // screen position of row 0, column 0 (scrolling applied)
        ImVec2 viewMin; // visible area of the editor child window, in screen space
        ImVec2 viewMax;
        int visibleRows;
    };

    void renderFindBar();
    void renderDiskBar(model::Document& doc);
    // Side bar beside the text: mode tabs (Layers / Minimap), hide button, resizable width.
    void renderSideBar(const model::Document& doc, ImVec2 size);
    void renderMinimap(const model::Document& doc, ImVec2 size);
    int rowAt(ImVec2 mouse, const Metrics& m) const;
    void drawSummaryRow(const model::Document& doc, const Row& row, const Metrics& m, float y);
    void renderEmptyState(ImVec2 size);
    // A Markdown document shown formatted (read-only); double-click or the Edit button for the text.
    void renderMarkdown(model::Document& doc, ImVec2 size);
    // The Rendered / Edit switch at the top right of a Markdown document; returns its rectangle.
    void markdownSwitch(model::Document& doc, ImVec2 topRight);
    static ImRect markdownSwitchRect(ImVec2 topRight, float scale);
    void buildRows(const model::Document& doc, int wrapColumns);
    int rowOf(model::TextPos pos) const;
    model::TextPos hitTest(const model::Document& doc, ImVec2 mouse, const Metrics& m) const;
    float rowX(const model::Document& doc, const Row& row, int column, const Metrics& m) const;
    int columnAtX(const model::Document& doc, const Row& row, float vcol) const;

    void handleMouse(const model::Document& doc, const Metrics& m, bool hovered);
    void handleKeyboard(const model::Document& doc, const Metrics& m);
    void moveVertical(const model::Document& doc, int rows, bool extend);
    void drawRows(const model::Document& doc, const Metrics& m, int firstRow, int lastRow);
    void drawGutter(const model::Document& doc, const Metrics& m, int firstRow, int lastRow);
    void drawHoverTooltip(const model::Document& doc, const Metrics& m);
    void revealCaret(const model::Document& doc, const Metrics& m);
    // Git: merge-conflict blocks (tints, accept buttons, a bar with next / previous), the change
    // peek opened from a change bar, and blame.
    void updateConflicts(const model::Document& doc);
    void renderConflictBar(const model::Document& doc);
    void drawConflictBackgrounds(const Metrics& m, int firstRow, int lastRow);
    void drawConflictActions(const model::Document& doc, const Metrics& m, int firstRow, int lastRow);
    void resolveConflict(int block, int resolution); // model::git::Resolution
    void jumpToConflict(const model::Document& doc, int direction);
    void renderChangePeek(const model::Document& doc);
    void drawBlame(const model::Document& doc, const Metrics& m, int firstRow, int lastRow);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;

    std::vector<Row> rows_;
    std::vector<int> firstRowOfLine_;
    int maxColumns_ = 0;

    LayersView layers_;
public:
    float& sideBarWidth() { return sideBarWidth_; } // remembered with the layout
    float& blameColumns() { return blameColumns_; } // the blame column's width in characters, remembered too
private:
    float sideBarWidth_ = 0.0f;   // 0: default on first use
    struct RenderedMarkdown {
        MarkdownView view;
        uint64_t revision = UINT64_MAX;
        std::string text;
        bool shown = false; // shown last frame (entering it scrolls to the caret's line)
    };
    std::map<int, RenderedMarkdown> markdown_; // by document id
    float blameColumns_ = 30.0f;  // drag its right edge: 9 (hash only) to 60
    int pendingScrollLine_ = -1;  // minimap: centre this line on the next frame
    int firstVisibleLine_ = 0;    // for the minimap's viewport frame
    int lastVisibleLine_ = 0;
    bool focused_ = false;
    bool revealCaret_ = true;
    bool focusRequested_ = true;
    bool focusFind_ = false;
    bool dragging_ = false;
    float preferredVcol_ = -1.0f; // kept across vertical moves
    double lastActivity_ = 0.0;   // resets the caret blink
    std::string findBuffer_;
    std::string replaceBuffer_;
    std::vector<viewmodel::Subscription> subscriptions_;
    std::vector<model::git::ConflictBlock> conflicts_;
    std::vector<std::string> conflictLines_; // the document's lines while it has conflicts
    int conflictsDoc_ = -1;
    uint64_t conflictsVersion_ = ~0ull;
    int peekLine_ = -1;
    bool openPeek_ = false;
    uint64_t blameRequested_ = ~0ull;
};

} // namespace enp::view
