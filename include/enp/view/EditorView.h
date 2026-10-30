// Custom-drawn code editor for the active document: gutter (line numbers, bookmarks, review
// markers), syntax colouring, selection, find highlights, word wrap and keyboard/mouse editing.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

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
    // One visual row: a whole line, or a wrapped piece of it.
    struct Row {
        int line;
        int begin; // byte range within the line
        int end;
    };
    struct Metrics {
        float charW;
        float lineH;
        float gutterW;
        float markerW;
        ImVec2 origin;  // screen position of row 0, column 0 (scrolling applied)
        ImVec2 viewMin; // visible area of the editor child window, in screen space
        ImVec2 viewMax;
        int visibleRows;
    };

    void renderFindBar();
    void renderEmptyState(ImVec2 size);
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

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;

    std::vector<Row> rows_;
    std::vector<int> firstRowOfLine_;
    int maxColumns_ = 0;

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
};

} // namespace enp::view
