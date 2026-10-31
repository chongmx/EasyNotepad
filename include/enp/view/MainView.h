// Top-level ImGui view bound to NotepadViewModel: title bar with document tabs, ribbon,
// the Agent or Code workspace layout, status bar and dialogs.
#pragma once

#include "enp/view/AgentCursorView.h"
#include "enp/view/AgentPanel.h"
#include "enp/view/DocumentListView.h"
#include "enp/view/EditorView.h"
#include "enp/view/FileDialogView.h"
#include "enp/view/GitView.h"
#include "enp/view/SearchPanel.h"
#include "enp/view/Fonts.h"
#include "enp/view/IWindowFrame.h"
#include "enp/view/MapView.h"
#include "enp/view/ReviewPanel.h"
#include "enp/view/RibbonView.h"
#include "enp/view/TerminalView.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <functional>
#include <string>
#include <vector>

namespace enp::view {

class MainView {
public:
    // `frame` is null when the OS draws the title bar (Android, web) or no frame is available.
    MainView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, float uiScale, IWindowFrame* frame = nullptr);
    ~MainView();

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
    // VS Code-style bottom panel: review tabs and the integrated terminal.
    void renderBottomPanel(float width, float height);
    void renderPanelActions(float left, float right, float y, float height);
    void renderTerminalWindow(); // the popped-out terminal
    void renderLanguagePicker();
    void renderHashResult();
    void renderSaveChangesPrompt();
    void renderQuitPrompt();
    void renderCloseFolderPrompt();
    void renderAllTabsPopup();
    void renderExplorer();
    void renderOpenDocuments(); // the explorer's "Open" tab
    void renderExplorerItem(const viewmodel::ExplorerItem& item);
    // Right-click menu of a file, a folder, or (root) the opened folder; and its questions.
    void explorerMenu(const viewmodel::ExplorerItem& item, bool root);
    void explorerAsk(std::string title, std::string hint, std::string value, std::string button,
                     std::function<void(const std::string&)> action);
    void explorerConfirm(std::string title, std::string text, std::string button, std::function<void()> action);
    void renderExplorerPrompt();
    bool explorerVisible(const viewmodel::ExplorerItem& item); // under the filter
    void renderPreview(ImVec2 size);
    void renderStatusBar();
    void renderGoToLinePopup();
    void renderCommentPopup();
    void renderPreferencesPopup();
    void renderSyntaxColorsPopup();
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
    MapView map_;
    FileDialogView fileDialogView_;
    SearchPanel searchPanel_;
    GitView gitView_;
    DocumentListView documentList_;
    AgentPanel agentPanel_;
    ReviewPanel reviewPanel_;
    TerminalView terminalView_;
    AgentCursorView agentCursor_;

    // Layout (unscaled pixels are multiplied by s_ at use).
    float explorerWidth_;
    float agentSideWidth_;
    float panelHeight_;
    float planWidth_;
    float conversationWidth_;
    int previewTab_ = 0;        // Agent workspace: 0 = proposed change, 1 = editor
    int previewTabRequest_ = -1;

    // Document tabs: a strip that scrolls when the tabs do not fit.
    float tabScroll_ = 0.0f;
    float tabScrollShown_ = 0.0f;
    int tabRevealSerial_ = -1;
    std::string allTabsFilter_;
    bool openAllTabs_ = false; // requested by an agent

    // Explorer: one tree; the open documents are marked in it.
    std::string explorerFilter_;
    std::string explorerFilterKey_; // explorerVisible_ was computed for this filter
    std::vector<std::string> explorerVisible_; // sorted paths matching the filter (and their folders)
    int explorerRevealSerial_ = -1;
    int explorerTab_ = 0;         // 0 = Files, 1 = Open, 2 = Git
    int explorerTabRequest_ = -1; // switch on the next frame (agents, "Reveal in Files")
    bool explorerCollapse_ = false;
    struct ExplorerPrompt {
        std::string title, hint, value, button;
        bool confirm = false; // a yes/no question (hint is its text), else a name to type
        std::function<void(const std::string&)> onValue;
        std::function<void()> onConfirm;
    } explorerPrompt_;
    bool openExplorerPrompt_ = false;

    std::string rendererInfo_;
    std::string commentDraft_;
    int goToLine_ = 1;
    bool openGoToLine_ = false;
    bool openComment_ = false;
    bool openPreferences_ = false;
    bool openSyntaxColors_ = false;
    bool openLanguagePicker_ = false;
    std::string languageFilter_;
    int syntaxColorsLanguage_ = -1; // index into model::languages(); -1 = all languages
    bool openAbout_ = false;
    bool openHelpMenu_ = false; // requested by an agent
    bool quitRequested_ = false;
    std::vector<bool> panelHasHeader_; // beginPanel() nesting
    std::vector<viewmodel::Subscription> subscriptions_;
};

} // namespace enp::view
