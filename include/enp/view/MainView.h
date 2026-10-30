// Top-level ImGui view bound to NotepadViewModel: title bar with document tabs, ribbon,
// the Agent or Code workspace layout, status bar and dialogs.
#pragma once

#include "enp/view/AgentPanel.h"
#include "enp/view/EditorView.h"
#include "enp/view/Fonts.h"
#include "enp/view/IWindowFrame.h"
#include "enp/view/ReviewPanel.h"
#include "enp/view/RibbonView.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <string>
#include <vector>

namespace enp::view {

class MainView {
public:
    // `frame` is null when the OS draws the title bar (Android, web) or no frame is available.
    MainView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, float uiScale, IWindowFrame* frame = nullptr);

    // Builds the whole UI for one frame (between ImGui::NewFrame() and ImGui::Render()).
    void render();

    ImVec4 clearColor() const { return palette_.clearColor; }
    bool quitRequested() const { return quitRequested_; }
    // Extra text shown in the About box (e.g. the GL renderer string).
    void setRendererInfo(std::string info) { rendererInfo_ = std::move(info); }

private:
    void handleShortcuts();
    void renderTopBar();
    // Returns the right edge of the last tab (including the "+" button).
    float renderDocumentTabs(float x0, float x1, float y, float height);
    float renderWindowButtons(float right, float y, float height); // returns the left edge
    void handleCaptionMouse();
    void renderCodeWorkspace();
    void renderAgentWorkspace();
    void renderReviewArea(float width);
    void renderExplorer();
    void renderExplorerItem(const viewmodel::ExplorerItem& item);
    void renderPreview(ImVec2 size);
    void renderStatusBar();
    void renderGoToLinePopup();
    void renderCommentPopup();
    void renderPreferencesPopup();
    void renderAboutPopup();

    bool beginPanel(const char* id, const char* title, ImVec2 size, const char* detail = nullptr);
    void endPanel();
    void verticalSplitter(const char* id, float& value, float direction, float height, float minValue, float maxValue);
    void horizontalSplitter(const char* id, float& value, float width, float minValue, float maxValue);
    bool flatButton(const char* label, const char* tooltip, bool enabled = true, bool active = false);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    float s_;
    IWindowFrame* frame_;
    std::vector<ScreenRect> dragRegions_;
    Palette palette_;
    RibbonView ribbon_;
    EditorView editor_;
    AgentPanel agentPanel_;
    ReviewPanel reviewPanel_;

    // Layout (unscaled pixels are multiplied by s_ at use).
    float explorerWidth_;
    float agentSideWidth_;
    float reviewHeight_;
    float planWidth_;
    float conversationWidth_;
    int previewTab_ = 0;        // Agent workspace: 0 = proposed change, 1 = editor
    int previewTabRequest_ = -1;

    std::string rendererInfo_;
    std::string commentDraft_;
    int goToLine_ = 1;
    bool openGoToLine_ = false;
    bool openComment_ = false;
    bool openPreferences_ = false;
    bool openAbout_ = false;
    bool quitRequested_ = false;
    std::vector<bool> panelHasHeader_; // beginPanel() nesting
    std::vector<viewmodel::Subscription> subscriptions_;
};

} // namespace enp::view
