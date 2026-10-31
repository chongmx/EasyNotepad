// Integrated terminal: renders the active TerminalScreen (colours, cursor, scrollback, selection),
// turns keystrokes into terminal input while focused, and lists the terminals when there are several.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/TerminalViewModel.h"

#include <cstdint>
#include <vector>

namespace enp::view {

class TerminalView {
public:
    TerminalView(viewmodel::TerminalViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);
    void setBlinkCursor(bool blink) { blinkCursor_ = blink; }

    void render(ImVec2 size, float zoom);
    // True while the terminal has keyboard focus: application shortcuts then step aside, as in VS Code.
    bool focused() const { return focused_; }
    void focus() { focusRequested_ = true; }

private:
    struct Selection {
        bool active = false;
        int anchorLine = 0, anchorColumn = 0;
        int line = 0, column = 0;
    };

    void renderList(float width, float height);
    void renderScreen(viewmodel::TerminalInstance& terminal, ImVec2 size, float zoom);
    void renderPlaceholder(ImVec2 size);
    void handleKeyboard(const viewmodel::TerminalInstance& terminal, float pageHeight);
    void renderContextMenu(const viewmodel::TerminalInstance& terminal);
    void copySelection(const viewmodel::TerminalInstance& terminal);
    void paste();
    void send(const std::string& bytes);
    ImU32 resolve(uint32_t color, bool foreground, uint8_t flags) const;
    bool selected(int line, int column) const;

    viewmodel::TerminalViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;

    // Actions that destroy or reshape the terminal being drawn run after the frame's drawing.
    enum class Deferred { None, Close, Clear };
    Deferred deferred_ = Deferred::None;

    bool focused_ = false;
    bool focusRequested_ = false;
    bool stickToBottom_ = true;
    bool dragging_ = false;
    int shownId_ = -1;
    uint64_t shownRevision_ = 0;
    double lastActivity_ = 0.0;
    bool blinkCursor_ = true;
    Selection selection_;
    std::vector<viewmodel::Subscription> subscriptions_;
};

} // namespace enp::view
