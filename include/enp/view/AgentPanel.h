// Agent UI pieces: conversation with prompt composer, plan + proposed changes, and the
// change preview with accept/reject.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <string>

namespace enp::view {

class AgentPanel {
public:
    AgentPanel(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    // `compact` is the Code-workspace side panel: narrower header, same features.
    void renderConversation(bool compact);
    void renderPlan();
    void renderChangePreview(ImVec2 size);
    void focusPrompt() { focusPrompt_ = true; }

private:
    void renderHeader(bool compact);
    void renderMessages(float height);
    void renderComposer();
    void bubble(const model::ChatMessage& message, float width);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    std::string prompt_;
    bool focusPrompt_ = false;
    size_t lastMessageCount_ = 0;
};

} // namespace enp::view
