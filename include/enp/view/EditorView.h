// Custom-drawn code editor for the active document: gutter (line numbers, bookmarks, review
// markers, fold arrows), syntax colouring, selection, find highlights, word wrap, folded regions
// shown as AI summaries, the layers map, and keyboard/mouse editing.
#pragma once

#include "enp/model/Git.h"
#include "enp/view/Fonts.h"
#include "enp/view/LayersView.h"
#include "enp/view/DataViews.h"
#include "enp/view/LibraryView.h"
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

    // Find bar (when visible) plus the text area, filling `size`. With `shown` (the column of two
    // that does not have the keyboard): that document, to look at and scroll; no input, no bars.
    void render(ImVec2 size, model::Document* shown = nullptr);
    void focus() { focusRequested_ = true; }
    void setEmoji(EmojiCache* emoji) { emoji_ = emoji; } // pictures for emoji in rendered Markdown
    const DataViews& dataViews() const { return data_; }
    const LibraryView& libraryView() const { return library_; }
    // A phone layout: no side bar (layers, minimap); the text gets the whole width.
    void setCompact(bool compact) { compact_ = compact; }
    // Rendered-Markdown states held (one per open Markdown document; tests check that closed
    // documents release theirs).
    size_t renderedMarkdownCount() const { return markdown_.size(); }
    // The text cell width the last frame used for the caret, selections and columns (tests).
    float cellWidth() const { return cellWidth_; }
    float cellFontSize() const { return cellFontSize_; } // the font size it was measured at
    // Image textures alive or awaiting the backend's destruction (0 once every Markdown document closed).
    size_t imageTextureCount() const { return images_.liveCount() + images_.pendingCount(); }
    size_t imageDocumentCount() const { return fileImages_.size(); }

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
    // Image and binary documents: the image (fit or actual size), or a notice with "Open as text".
    void renderFileView(model::Document& doc, ImVec2 size);
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
    // Column (box) selection: Shift+Alt+drag or a middle-button drag; one caret per line.
    void updateColumnSelection(const model::Document& doc, const Metrics& m);
    float mouseVcol(const model::Document& doc, ImVec2 mouse, const Metrics& m, model::TextPos pos) const;
    void drawRows(const model::Document& doc, const Metrics& m, int firstRow, int lastRow);
    void drawGutter(const model::Document& doc, const Metrics& m, int firstRow, int lastRow);
    void drawHoverTooltip(const model::Document& doc, const Metrics& m);
    // IntelliSense: the suggestion list at the caret (one window for every document), and the
    // declaration of the name under the mouse.
    void renderCompletion(const model::Document& doc, const Metrics& m);
    void drawSymbolTooltip(const model::Document& doc, const Metrics& m);
    // The name under the mouse: its row and byte range in the line; false between words, in the
    // gutter or on a folded summary.
    bool nameAt(const model::Document& doc, const Metrics& m, ImVec2 mouse, int& row, int& begin, int& end) const;
    // Ctrl (Cmd on macOS) held over a name: underlined with a hand cursor when the index knows it;
    // a click goes to its definition. Returns true when the click was taken.
    bool handleDefinitionLink(const model::Document& doc, const Metrics& m, bool hovered);
    void drawDefinitionLink(const model::Document& doc, const Metrics& m);
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
    float cellWidth_ = 0.0f;
    float cellFontSize_ = 0.0f;

    std::vector<Row> rows_;
    std::vector<int> firstRowOfLine_;
    int maxColumns_ = 0;

    LayersView layers_;
public:
    float& sideBarWidth() { return sideBarWidth_; } // remembered with the layout
    float& blameColumns() { return blameColumns_; } // the blame column's width in characters, remembered too
    float& markdownColumn() { return markdownColumn_; } // rendered Markdown's text width, remembered too
private:
    float sideBarWidth_ = 0.0f;   // 0: default on first use
    float markdownColumn_ = 0.0f; // 0: the standard width (set by dragging the ruler's ends)
    float rulerDragStart_ = 0.0f, rulerDragMouse_ = 0.0f;
    bool rulerHovered_ = false;   // the mouse is on the ruler: no link click, no double-click to edit
    bool passive_ = false;        // this frame shows a document other than the active one (render's `shown`)
    void markdownRuler(ImVec2 winPos, float viewW, float column, float widest);
    struct RenderedMarkdown {
        MarkdownView view;
        uint64_t revision = UINT64_MAX;
        std::string text;
        bool shown = false; // shown last frame (entering it scrolls to the caret's line)
        // Images this document acquired from images_ (released when it closes or its text changes).
        struct HeldImage {
            std::string key; // the cache's key (a resolved path, or the URL)
            const MarkdownImage* image = nullptr;
        };
        std::map<std::string, HeldImage> images; // by source as written
    };
    std::map<int, RenderedMarkdown> markdown_; // by document id (open documents only)
    MarkdownImages images_;                     // shared by the documents, destroyed when unused
    EmojiCache* emoji_ = nullptr;               // owned by MainView
    DataViews data_;                            // CSV table, JSON/YAML tree and accordion
    LibraryView library_;                       // what a .so, .dll, .a or .lib contains
    std::map<int, RenderedMarkdown::HeldImage> fileImages_; // image documents' images, by document id
    bool imageFit_ = true;                      // image documents: fit the window, else actual size
    void releaseImages(RenderedMarkdown& md);
    const MarkdownImage* loadImage(RenderedMarkdown& md, const model::Document& doc, const std::string& source);
    // The text and the rendered Markdown are drawn in one child window each, shared by all
    // documents (Dear ImGui never frees a window: one per document would accumulate for as long
    // as the app runs). A document's scroll is kept here and restored when it is shown again.
    struct DocumentScroll {
        ImVec2 text{0.0f, 0.0f};
        ImVec2 markdown{0.0f, 0.0f};
    };
    std::map<int, DocumentScroll> scroll_; // by document id (open documents only)
    int shownTextDoc_ = -1;     // document the text window showed last
    int shownMarkdownDoc_ = -1; // document the Markdown window showed last
    void pruneClosed();         // drops the states of closed documents
    float blameColumns_ = 30.0f;  // drag its right edge: 9 (hash only) to 60
    int pendingScrollLine_ = -1;  // minimap: centre this line on the next frame
    int firstVisibleLine_ = 0;    // for the minimap's viewport frame
    int lastVisibleLine_ = 0;
    bool focused_ = false;
    bool compact_ = false;
    bool tapPending_ = false; // a finger went down on the text: a tap places the caret on release
    bool revealCaret_ = true;
    bool focusRequested_ = true;
    bool focusFind_ = false;
    bool dragging_ = false;
    float preferredVcol_ = -1.0f; // kept across vertical moves
    std::vector<float> preferredVcols_; // the same for every caret, with several
    std::vector<model::Selection> drawSelections_; // drawRows' carets, reused frame to frame
    bool dragKeepsCursors_ = false;     // a drag after Alt+click extends the new caret only
    int columnButton_ = -1;             // the mouse button of a column selection in progress
    model::TextPos columnAnchor_;
    float columnAnchorVcol_ = 0.0f;
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
    bool completionHovered_ = false; // the suggestion list, last frame
    bool completionFocused_ = false;
    int completionSerial_ = -1;      // the list's selection last scrolled into view
    int completionSession_ = 0;      // the last list seen opening (the editor takes the keyboard for it)
    ImVec2 hoverPos_{-1.0f, -1.0f};  // the mouse resting for a symbol's tooltip
    int linkRow_ = -1;               // Ctrl+hover: the name under the mouse, this frame
    int linkBegin_ = 0;
    int linkEnd_ = 0;
    double hoverSince_ = 0.0;
};

} // namespace enp::view
