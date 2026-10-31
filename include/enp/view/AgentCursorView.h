// The agent's glowing cursor: glides to the UI element named by NotepadViewModel::agentPointer,
// highlights it, plays a click ripple when asked, and shows the agent's message in a bubble.
// Drawn on the foreground layer and never takes input, so it cannot get in the user's way.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

namespace enp::view {

class AgentCursorView {
public:
    AgentCursorView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    // Call after the whole UI has been built (element rectangles are from this frame).
    void render();

private:
    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;

    int serial_ = -1;
    double moveStart_ = 0.0;
    ImVec2 from_{-1.0f, -1.0f};
    ImVec2 pos_{-1.0f, -1.0f};
    float fade_ = 0.0f; // 0 hidden .. 1 shown
};

} // namespace enp::view
