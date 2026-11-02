// Top-level ImGui view bound to NotepadViewModel: title bar with document tabs, ribbon,
// the Agent or Code workspace layout, status bar and dialogs.
#pragma once

#include "enp/view/AgentCursorView.h"
#include "enp/view/AgentPanel.h"
#include "enp/view/DocumentListView.h"
#include "enp/view/EngineLabView.h"
#include "enp/view/EditorView.h"
#include "enp/view/EmojiCache.h"
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
    // Android Back: closes a full-screen page (the Engine lab); false if there was none.
    bool navigateBack();
    // Edges of the window covered by the system (status and navigation bars, a notch, the
    // on-screen keyboard), in pixels: the UI keeps inside them.
    void setSafeArea(float left, float top, float right, float bottom) { safeArea_ = ImVec4(left, top, right, bottom); }

    // The arrangement, chosen every frame from the window's size in dp: the desktop layout when
    // there is room; on a phone one screen at a time, with a bottom bar when upright and a rail
    // on the left when turned sideways (so the editor keeps its height).
    enum class Layout { Desktop, Portrait, Landscape };
    static Layout chooseLayout(float widthDp, float heightDp);
    Layout layout() const { return layout_; }
    const EditorView& editor() const { return editor_; }
    const EmojiCache& emoji() const { return emoji_; }
    // After a close (Application's memory trim): caches let go of what is no longer on screen.
    void compactCaches() { emoji_.releaseUnused(); }

private:
    void handleShortcuts();
    // Phone layouts: every dialog, popup and floating window kept inside the visible screen, and
    // shrunk to it when larger (they are laid out for a desktop).
    void keepWindowsOnScreen();
    void renderTopBar();
    // Phone layouts: one screen at a time.
    enum class Screen { Files, Editor, Agent, Tools };
    void renderCompactTopBar();
    void renderCompactScreen(ImVec2 size);
    void renderNavigation(bool rail, ImVec2 size);
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
    // Explorer rows: icon and name start at the row's left edge plus this pad (no arrow gap), and
    // names start one icon column further, so folder and file names line up.
    float explorerRowPad() const { return 4.0f * s_; }
    float explorerIconColumn() const { return ImGui::GetFontSize() * 1.2f + 4.0f * s_; }
    void renderPreview(ImVec2 size);
    void renderStatusBar();
    void renderGoToLinePopup();
    void renderCommentPopup();
    void renderPreferencesPopup();
    void renderSyntaxColorsPopup();
    void renderAboutPopup();

    // `onClose`: an X button in the header, which calls it.
    bool beginPanel(const char* id, const char* title, ImVec2 size, const char* detail = nullptr,
                    std::function<void()> onClose = nullptr, const char* closeTooltip = nullptr);
    void endPanel();
    void verticalSplitter(const char* id, float& value, float direction, float height, float minValue, float maxValue);
    void horizontalSplitter(const char* id, float& value, float width, float minValue, float maxValue);
    bool flatButton(const char* label, const char* tooltip, bool enabled = true, bool active = false);
    // A square icon-only flat button `size` wide, the icon centred.
    bool flatIconButton(const char* label, const char* tooltip, float size, bool enabled = true);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    float s_;
    IWindowFrame* frame_;
    std::vector<ScreenRect> dragRegions_;
    Palette palette_;
    EmojiCache emoji_; // emoji pictures for rendered Markdown, shared by the editor and the agent chat
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
    EngineLabView engineLab_;

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

    // Developer mode: the process's resource readout in the status bar, refreshed once a second.
    std::string resourceText_; // compact, in the bar
    std::string resourceTip_;  // the full readout, on hover
    double resourceTextAt_ = -1.0;

    // Explorer: one tree; the open documents are marked in it.
    std::string explorerFilter_;
    std::string explorerFilterKey_; // explorerVisible_ was computed for this filter
    std::vector<std::string> explorerVisible_; // sorted paths matching the filter (and their folders)
    int explorerRevealSerial_ = -1;
    int explorerTab_ = 0;         // 0 = Files, 1 = Open, 2 = Git
    int explorerTabRequest_ = -1; // switch on the next frame (agents, "Reveal in Files")
    bool explorerCollapse_ = false;
    std::string explorerToggle_; // a folder an agent clicked: toggled on the next frame
    struct ExplorerPrompt {
        std::string title, hint, value, button;
        bool confirm = false; // a yes/no question (hint is its text), else a name to type
        std::function<void(const std::string&)> onValue;
        std::function<void()> onConfirm;
    } explorerPrompt_;
    bool openExplorerPrompt_ = false;

    std::string rendererInfo_;
    ImVec4 safeArea_{0.0f, 0.0f, 0.0f, 0.0f};
    Layout layout_ = Layout::Desktop;
    Screen screen_ = Screen::Editor;
    bool compactRibbon_ = false;  // the ribbon shown in a phone layout (the menu button)
    int lastActiveDocument_ = -1; // a different document became active: show the editor
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
    int versionTaps_ = 0; // About: seven taps on the version turn on developer mode
    bool openHelpMenu_ = false; // requested by an agent
    bool quitRequested_ = false;
    std::vector<bool> panelHasHeader_; // beginPanel() nesting
    std::vector<viewmodel::Subscription> subscriptions_;
};

} // namespace enp::view
