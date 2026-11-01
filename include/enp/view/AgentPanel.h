// Agent UI pieces: conversation with prompt composer, plan + proposed changes, and the
// change preview with accept/reject.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/MarkdownView.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <deque>
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
    void bubble(const model::ChatMessage& message, size_t index, float width);
    void openLink(const std::string& url); // a web link in the browser, a file path in the editor
    // Install / log in / settings for the agent: shown when it is missing or not logged in, or on request.
    void renderSetup(float width);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    std::string prompt_;
    bool focusPrompt_ = false;
    size_t lastMessageCount_ = 0;
    std::string commandEdit_; // the set-up card's command field while it is being edited
    bool editingCommand_ = false;
    std::deque<MarkdownView> markdown_; // per message (deque: views keep their address as it grows)
};

} // namespace enp::view
