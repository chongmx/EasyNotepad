#include "enp/view/MainView.h"

#include "enp/view/PanelFrame.h"

#include "enp/os/Platform.h"
#include "enp/model/Highlighter.h"
#include "enp/view/Icons.h"
#include "enp/view/SyntaxColors.h"
#include "enp/view/UiAutomation.h"
#include "enp/viewmodel/DummyData.h"

#include "IconsFontAwesome6.h"
#include "imgui_internal.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace enp::view {

using viewmodel::CommandId;
using viewmodel::WorkspaceMode;

namespace {

constexpr float kTopBarHeight = 36.0f;
constexpr float kStatusBarHeight = 24.0f;
constexpr float kGap = 6.0f;
constexpr float kPanelHeader = 28.0f;
constexpr float kRounding = 6.0f;

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImU32 withAlpha(ImU32 color, int alpha)
{
    return (color & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT);
}

const char* fileIcon(const std::string& name)
{
    switch (model::languageFromPath(name)) {
    case model::Language::Markdown: return ICON_FA_FILE_PEN;
    case model::Language::PlainText: return ICON_FA_FILE_LINES;
    default: return ICON_FA_FILE_CODE;
    }
}

} // namespace

MainView::MainView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, float uiScale, IWindowFrame* frame)
    : vm_(vm), fonts_(fonts), s_(uiScale), frame_(frame && frame->customTitleBar() ? frame : nullptr), palette_(applyTheme(vm.darkTheme.get())),
      ribbon_(vm, fonts, palette_, uiScale), editor_(vm, fonts, palette_, uiScale),
      map_(vm, fonts, palette_, uiScale), fileDialogView_(vm.fileDialog(), fonts, palette_, uiScale),
      searchPanel_(vm, fonts, palette_, uiScale),
      gitView_(vm, fonts, palette_, uiScale),
      documentList_(vm, palette_, uiScale),
      agentPanel_(vm, fonts, palette_, uiScale), reviewPanel_(vm, fonts, palette_, uiScale),
      terminalView_(vm.terminal(), fonts, palette_, uiScale), agentCursor_(vm, fonts, palette_, uiScale),
      explorerWidth_(230.0f * uiScale), agentSideWidth_(360.0f * uiScale), panelHeight_(240.0f * uiScale),
      planWidth_(270.0f * uiScale), conversationWidth_(430.0f * uiScale)
{
    // Ctrl+Tab cycles documents here, not ImGui's window list.
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyNext = 0;
    ImGui::GetCurrentContext()->ConfigNavWindowingKeyPrev = 0;
    if (frame)
        frame->setDarkTheme(vm_.darkTheme.get());
    subscriptions_.push_back(vm_.darkTheme.subscribe([this, frame](const bool& dark) {
        palette_ = applyTheme(dark);
        if (frame)
            frame->setDarkTheme(dark);
    }));
    subscriptions_.push_back(vm_.clipboardRequested.subscribe([this](const viewmodel::ClipboardAction& action) {
        switch (action) {
        case viewmodel::ClipboardAction::Copy:
            ImGui::SetClipboardText(vm_.selectedText().c_str());
            break;
        case viewmodel::ClipboardAction::Cut:
            ImGui::SetClipboardText(vm_.selectedText().c_str());
            vm_.deleteSelection();
            break;
        case viewmodel::ClipboardAction::Paste:
            if (const char* text = ImGui::GetClipboardText())
                vm_.insertText(text);
            break;
        }
    }));
    subscriptions_.push_back(vm_.focusPromptRequested.subscribe([this]() { agentPanel_.focusPrompt(); }));
    subscriptions_.push_back(vm_.goToLineRequested.subscribe([this]() {
        const model::Document* doc = vm_.activeDocument();
        goToLine_ = doc ? doc->caret.line + 1 : 1;
        openGoToLine_ = true;
    }));
    subscriptions_.push_back(vm_.addCommentRequested.subscribe([this]() { openComment_ = true; }));
    subscriptions_.push_back(vm_.preferencesRequested.subscribe([this]() { openPreferences_ = true; }));
    subscriptions_.push_back(vm_.syntaxColorsRequested.subscribe([this]() { openSyntaxColors_ = true; }));
    subscriptions_.push_back(vm_.languagePickerRequested.subscribe([this]() { openLanguagePicker_ = true; }));
    subscriptions_.push_back(vm_.gitTabRequested.subscribe([this]() { explorerTabRequest_ = 2; }));
    documentList_.onReveal = [this](int index) {
        vm_.showExplorer.set(true);
        vm_.activate(index);
        explorerTabRequest_ = 0; // the Files tab
        explorerRevealSerial_ = -1;
    };
    // Panel sizes are remembered (in unscaled pixels, so they survive a DPI change).
    vm_.settings().section(
        "layout",
        [this](const model::Json& j) {
            auto load = [&](const char* key, float& value) {
                if (const model::Json* v = j.find(key); v && v->isNumber() && v->asNumber() > 0.0)
                    value = float(v->asNumber()) * s_;
            };
            load("explorerWidth", explorerWidth_);
            load("agentSideWidth", agentSideWidth_);
            load("panelHeight", panelHeight_);
            load("planWidth", planWidth_);
            load("conversationWidth", conversationWidth_);
            load("sideBarWidth", editor_.sideBarWidth());
            if (const model::Json* v = j.find("blameColumns"); v && v->isNumber())
                editor_.blameColumns() = float(v->asNumber());
        },
        [this]() {
            return model::Json::object()
                .set("explorerWidth", explorerWidth_ / s_)
                .set("agentSideWidth", agentSideWidth_ / s_)
                .set("panelHeight", panelHeight_ / s_)
                .set("planWidth", planWidth_ / s_)
                .set("conversationWidth", conversationWidth_ / s_)
                .set("sideBarWidth", editor_.sideBarWidth() / s_)
                .set("blameColumns", double(editor_.blameColumns()));
        });
    subscriptions_.push_back(vm_.aboutRequested.subscribe([this]() { openAbout_ = true; }));
    subscriptions_.push_back(vm_.quitRequested.subscribe([this]() { quitRequested_ = true; }));
    subscriptions_.push_back(vm_.editorRequested.subscribe([this]() { previewTabRequest_ = 1; }));
    subscriptions_.push_back(vm_.workspace.subscribe([this](const WorkspaceMode& mode) {
        vm_.statusText.set(mode == WorkspaceMode::Agent ? "Agent workspace: describe the goal, review the changes"
                                                        : "Code workspace: manual editing with the agent on the side");
        editor_.focus();
    }));
}

// ---- Frame ------------------------------------------------------------------------------------

MainView::~MainView()
{
    vm_.settings().removeSection("layout");
}

void MainView::render()
{
    UiAutomation::instance().beginFrame();
    terminalView_.setBlinkCursor(vm_.blinkCaret.get());
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col(palette_.titleBarBg));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##main", nullptr, flags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(3);

    handleShortcuts();
    renderTopBar();
    ribbon_.render();

    const float statusH = kStatusBarHeight * s_;
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(kGap * s_, kGap * s_));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(palette_.titleBarBg));
    ImGui::BeginChild("##body", ImVec2(avail.x, std::max(1.0f, avail.y - statusH)), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (vm_.workspace.get() == WorkspaceMode::Agent)
        renderAgentWorkspace();
    else
        renderCodeWorkspace();
    ImGui::EndChild();
    renderStatusBar();

    renderGoToLinePopup();
    renderCommentPopup();
    renderTerminalWindow();
    renderLanguagePicker();
    renderHashResult();
    gitView_.renderPopups();
    documentList_.renderMenu();
    renderSaveChangesPrompt();
    renderQuitPrompt();
    fileDialogView_.render();
    renderPreferencesPopup();
    renderSyntaxColorsPopup();
    renderAboutPopup();
    handleCaptionMouse();
    ImGui::End();
    UiAutomation::instance().endFrame();
    agentCursor_.render(); // on top of everything, using this frame's element positions
}

// ---- Input --------------------------------------------------------------------------------------

void MainView::handleShortcuts()
{
    // Editing: only while neither a text field nor the terminal has focus.
    // Global: everywhere except the terminal, whose keys go to the shell (as in VS Code).
    // Always: also while the terminal has focus.
    enum Scope { Editing, Global, Always };
    struct Binding {
        ImGuiKeyChord chord;
        CommandId id;
        Scope scope;
    };
    using C = CommandId;
    static const Binding bindings[] = {
        {ImGuiMod_Ctrl | ImGuiKey_N, C::NewFile, Global},
        {ImGuiMod_Ctrl | ImGuiKey_O, C::OpenFile, Global},
        {ImGuiMod_Ctrl | ImGuiKey_S, C::Save, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, C::SaveAll, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_S, C::SaveAs, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_O, C::OpenFolder, Global},
        {ImGuiMod_Ctrl | ImGuiKey_W, C::CloseFile, Global},
        {ImGuiMod_Ctrl | ImGuiKey_Tab, C::NextDocument, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Tab, C::PreviousDocument, Global},
        {ImGuiMod_Ctrl | ImGuiKey_PageDown, C::NextDocument, Global},
        {ImGuiMod_Ctrl | ImGuiKey_PageUp, C::PreviousDocument, Global},
        {ImGuiMod_Ctrl | ImGuiKey_X, C::Cut, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_C, C::Copy, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_V, C::Paste, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_Z, C::Undo, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_Y, C::Redo, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, C::Redo, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_A, C::SelectAll, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_D, C::DuplicateLine, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, C::DeleteLine, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_UpArrow, C::MoveLineUp, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_DownArrow, C::MoveLineDown, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_U, C::UpperCase, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_U, C::LowerCase, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_Q, C::ToggleComment, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_F, C::Find, Global},
        {ImGuiMod_Ctrl | ImGuiKey_H, C::Replace, Global},
        {ImGuiKey_F3, C::FindNext, Global},
        {ImGuiMod_Shift | ImGuiKey_F3, C::FindPrevious, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, C::FindInFiles, Global},
        {ImGuiMod_Ctrl | ImGuiKey_G, C::GoToLine, Global},
        {ImGuiMod_Ctrl | ImGuiKey_F2, C::ToggleBookmark, Global},
        {ImGuiKey_F2, C::NextBookmark, Global},
        {ImGuiMod_Shift | ImGuiKey_F2, C::PreviousBookmark, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, C::ToggleExplorer, Global},
        {ImGuiMod_Ctrl | ImGuiKey_J, C::TogglePanel, Always},
        {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_B, C::ToggleLayersMap, Global},
        {ImGuiMod_Ctrl | ImGuiKey_GraveAccent, C::ToggleTerminal, Always},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_GraveAccent, C::NewTerminal, Always},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I, C::ToggleAgentPanel, Global},
        {ImGuiMod_Alt | ImGuiKey_Z, C::ToggleWordWrap, Global},
        {ImGuiMod_Ctrl | ImGuiKey_Equal, C::ZoomIn, Always},
        {ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, C::ZoomIn, Always},
        {ImGuiMod_Ctrl | ImGuiKey_Minus, C::ZoomOut, Global},
        {ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, C::ZoomOut, Always},
        {ImGuiMod_Ctrl | ImGuiKey_0, C::ZoomReset, Always},
        {ImGuiKey_F7, C::RunLint, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F7, C::RunAllReviews, Global},
        {ImGuiKey_F8, C::NextFinding, Global},
        {ImGuiMod_Shift | ImGuiKey_F8, C::PreviousFinding, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_M, C::AddReviewComment, Global},
        {ImGuiMod_Ctrl | ImGuiKey_I, C::AskAgent, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M, C::ToggleWorkspaceMode, Always},
    };
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool terminal = terminalView_.focused();
    for (const Binding& b : bindings) {
        if ((b.scope == Editing && (typing || terminal)) || (b.scope == Global && terminal))
            continue;
        if (ImGui::Shortcut(b.chord, ImGuiInputFlags_RouteGlobal | (b.scope == Editing ? 0 : ImGuiInputFlags_Repeat)))
            vm_.execute(b.id);
    }
    if (terminal)
        return;
    const int count = int(vm_.documents().size());
    if (count > 0 && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageDown, ImGuiInputFlags_RouteGlobal))
        vm_.activate((vm_.activeIndex.get() + 1) % count);
    if (count > 0 && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_PageUp, ImGuiInputFlags_RouteGlobal))
        vm_.activate((vm_.activeIndex.get() + count - 1) % count);
}

// ---- Top bar ------------------------------------------------------------------------------------

bool MainView::flatButton(const char* label, const char* tooltip, bool enabled, bool active)
{
    ImGui::PushStyleColor(ImGuiCol_Button, active ? col(palette_.accentSoft) : ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(palette_.hover));
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    if (tooltip)
        ImGui::SetItemTooltip("%s", tooltip);
    return clicked;
}

void MainView::renderTopBar()
{
    // Each region of the shared main window gets its own ID scope: the title bar, ribbon and
    // status bar all number their items from 0 (tabs, buttons) and would otherwise collide.
    ImGui::PushID("titleBar");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = kTopBarHeight * s_;
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), palette_.titleBarBg);

    const float frameH = ImGui::GetFrameHeight();
    const float y = origin.y + (h - frameH) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 10.0f * s_, y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(palette_.accent), ICON_FA_PEN_TO_SQUARE);
    ImGui::SameLine(0.0f, 6.0f * s_);
    ImGui::TextUnformatted("EasyNotepad");
    ImGui::SameLine(0.0f, 14.0f * s_);
    dragRegions_.clear();
    dragRegions_.push_back({origin, ImVec2(ImGui::GetCursorScreenPos().x - 4.0f * s_, origin.y + h)}); // logo and name

    if (flatButton("File " ICON_FA_CARET_DOWN, nullptr))
        ImGui::OpenPopup("##fileMenu");
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMin().x, origin.y + h));
    if (ImGui::BeginPopup("##fileMenu")) {
        const CommandId items[] = {CommandId::NewFile, CommandId::OpenFile, CommandId::OpenFolder, CommandId::Save,
                                   CommandId::SaveAs, CommandId::SaveAll, CommandId::CloseFile};
        for (CommandId id : items) {
            const std::string label = std::string(commandIcon(id)) + "  " + commandInfo(id).tooltip;
            if (ImGui::MenuItem(label.c_str(), commandInfo(id).shortcut, false, vm_.command(id).canExecute()))
                vm_.execute(id);
        }
        ImGui::Separator();
        // Recent files (shown workspace-relative when inside the workspace) and folders.
        std::string chosen;
        if (ImGui::BeginMenu(ICON_FA_CLOCK_ROTATE_LEFT "  Recent files", !vm_.recentFiles().empty())) {
            for (const std::string& path : vm_.recentFiles())
                if (ImGui::MenuItem(vm_.workspaceRelative(path).c_str()))
                    chosen = path;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(ICON_FA_FOLDER_OPEN "  Recent folders", !vm_.recentFolders().empty())) {
            for (const std::string& path : vm_.recentFolders())
                if (ImGui::MenuItem(path.c_str()))
                    chosen = path;
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(ICON_FA_BROOM "  Clear recent", nullptr, false, !vm_.recentFiles().empty() || !vm_.recentFolders().empty()))
            vm_.clearRecent();
        if (!chosen.empty())
            vm_.openRecent(chosen);
        if (ImGui::BeginMenu(ICON_FA_ARROWS_ROTATE "  Workspace")) {
            for (CommandId id : {CommandId::SwitchToAgentMode, CommandId::SwitchToCodeMode})
                if (ImGui::MenuItem(commandInfo(id).label, nullptr, vm_.isChecked(id)))
                    vm_.execute(id);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        for (CommandId id : {CommandId::Preferences, CommandId::About, CommandId::Exit}) {
            const std::string label = std::string(commandIcon(id)) + "  " + commandInfo(id).tooltip;
            if (ImGui::MenuItem(label.c_str(), commandInfo(id).shortcut))
                vm_.execute(id);
        }
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 2.0f * s_);
    for (CommandId id : {CommandId::Save, CommandId::Undo, CommandId::Redo}) {
        ImGui::PushID(int(id));
        const std::string tip = std::string(commandInfo(id).tooltip) + " (" + commandInfo(id).shortcut + ")";
        if (flatButton(commandIcon(id), tip.c_str(), vm_.command(id).canExecute()))
            vm_.execute(id);
        ImGui::PopID();
        ImGui::SameLine(0.0f, 2.0f * s_);
    }
    const float tabsX0 = ImGui::GetCursorScreenPos().x + 10.0f * s_;

    // Right-aligned: workspace switch, theme, settings, help, version.
    const char* version = "v" ENP_VERSION;
    const float modeW = ImGui::CalcTextSize(ICON_FA_ROBOT " Agent").x + ImGui::CalcTextSize(ICON_FA_CODE " Code").x +
                        ImGui::GetStyle().FramePadding.x * 4.0f + 2.0f * s_;
    const float iconW = frameH * 1.1f;
    const float rightW = modeW + 3.0f * iconW + ImGui::CalcTextSize(version).x + 40.0f * s_;
    const float buttonsX = frame_ ? renderWindowButtons(origin.x + width, origin.y, h) : origin.x + width;
    const float rightX = buttonsX - rightW;

    const float tabsX1 = renderDocumentTabs(tabsX0, rightX - 12.0f * s_, origin.y, h);
    // Empty caption space: the strip above the tabs and everything between the tabs and the
    // right-hand controls.
    dragRegions_.push_back({ImVec2(tabsX0, origin.y), ImVec2(tabsX1, origin.y + 5.0f * s_)});
    dragRegions_.push_back({ImVec2(tabsX1 + 4.0f * s_, origin.y), ImVec2(rightX - 4.0f * s_, origin.y + h)});
    if (frame_)
    {
        const ImVec2 o = ImGui::GetMainViewport()->Pos;
        std::vector<ScreenRect> local = dragRegions_;
        for (ScreenRect& r : local) {
            r.min = ImVec2(r.min.x - o.x, r.min.y - o.y);
            r.max = ImVec2(r.max.x - o.x, r.max.y - o.y);
        }
        frame_->setDragRegions(local);
    }

    ImGui::SetCursorScreenPos(ImVec2(rightX, y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * s_);
    const bool agent = vm_.workspace.get() == WorkspaceMode::Agent;
    ImGui::PushStyleColor(ImGuiCol_Text, agent ? col(palette_.modeAgent) : col(palette_.textDim));
    if (flatButton(ICON_FA_ROBOT " Agent", "Agent workspace: describe the goal, review the changes", true, agent))
        vm_.execute(CommandId::SwitchToAgentMode);
    registerLastItem("command:SwitchToAgentMode", "button", "Agent workspace", [this]() { vm_.execute(CommandId::SwitchToAgentMode); });
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 2.0f * s_);
    ImGui::PushStyleColor(ImGuiCol_Text, !agent ? col(palette_.modeCode) : col(palette_.textDim));
    if (flatButton(ICON_FA_CODE " Code", "Code workspace: manual editing", true, !agent))
        vm_.execute(CommandId::SwitchToCodeMode);
    registerLastItem("command:SwitchToCodeMode", "button", "Code workspace", [this]() { vm_.execute(CommandId::SwitchToCodeMode); });
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::SameLine(0.0f, 12.0f * s_);
    if (flatButton(vm_.darkTheme.get() ? ICON_FA_SUN : ICON_FA_MOON, "Toggle theme"))
        vm_.execute(CommandId::ToggleTheme);
    ImGui::SameLine(0.0f, 2.0f * s_);
    if (flatButton(ICON_FA_GEAR, "Preferences"))
        vm_.execute(CommandId::Preferences);
    ImGui::SameLine(0.0f, 2.0f * s_);
    // Help: the tour, the git guide and About.
    if (flatButton(ICON_FA_CIRCLE_QUESTION, "Help") || openHelpMenu_)
        ImGui::OpenPopup("##helpMenu");
    openHelpMenu_ = false;
    registerLastItem("help-menu", "button", "Help", [this]() { openHelpMenu_ = true; });
    if (ImGui::BeginPopup("##helpMenu")) {
        if (ImGui::MenuItem(ICON_FA_HAND_POINTER "  Take the tour"))
            vm_.execute(CommandId::StartTour);
        if (ImGui::MenuItem(ICON_FA_GRADUATION_CAP "  Learn Git"))
            vm_.execute(CommandId::GitGuide);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO "  About EasyNotepad"))
            vm_.execute(CommandId::About);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 8.0f * s_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", version);

    // Claim the bar's area with an item: moving the cursor past the content without submitting
    // one is an ImGui error ("SetCursorPos() to extend window boundaries").
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopID();
}

float MainView::renderDocumentTabs(float x0, float x1, float y, float height)
{
    const auto& docs = vm_.documents();
    const int count = int(docs.size());
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    const float buttonW = ImGui::GetFrameHeight();
    const float tabTop = y + 5.0f * s_;
    const float tabH = height - 5.0f * s_;
    const float closeSize = ImGui::GetTextLineHeight();
    const float pad = 10.0f * s_;
    const float minTabW = 104.0f * s_, maxTabW = 230.0f * s_;

    // Natural widths: icon, name, the folder hint for namesakes, close button.
    std::vector<float> widths(static_cast<size_t>(count), 0.0f);
    std::vector<std::string> hints(static_cast<size_t>(count));
    float total = 0.0f;
    for (int i = 0; i < count; ++i) {
        const model::Document& doc = *docs[size_t(i)];
        hints[size_t(i)] = vm_.documentHint(i);
        float w = pad + ImGui::CalcTextSize(fileIcon(doc.title)).x + 6.0f * s_ + ImGui::CalcTextSize(doc.title.c_str()).x +
                  closeSize + 14.0f * s_;
        if (!hints[size_t(i)].empty())
            w += 6.0f * s_ + ImGui::CalcTextSize(hints[size_t(i)].c_str()).x;
        widths[size_t(i)] = std::clamp(w, minTabW, maxTabW);
        total += widths[size_t(i)];
    }
    const bool overflow = total > x1 - x0 - buttonW - 4.0f * s_;
    const std::string listLabel = std::string(ICON_FA_CHEVRON_DOWN " ") + std::to_string(count);
    const float listW = overflow ? ImGui::CalcTextSize(listLabel.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f : 0.0f;
    const float stripX1 = overflow ? x1 - buttonW - listW - 6.0f * s_ : x1 - buttonW - 4.0f * s_;
    const float stripW = std::max(0.0f, stripX1 - x0);
    const float maxScroll = std::max(0.0f, total - stripW);

    // Keep the active tab in view when it changes (opening, switching, Ctrl+Tab).
    if (vm_.revealSerial() != tabRevealSerial_ && vm_.activeIndex.get() >= 0 && vm_.activeIndex.get() < count) {
        tabRevealSerial_ = vm_.revealSerial();
        float left = 0.0f;
        for (int i = 0; i < vm_.activeIndex.get(); ++i)
            left += widths[size_t(i)];
        const float right = left + widths[size_t(vm_.activeIndex.get())];
        if (left < tabScroll_)
            tabScroll_ = left - (vm_.activeIndex.get() > 0 ? 24.0f * s_ : 0.0f); // a peek of the previous tab
        else if (right > tabScroll_ + stripW)
            tabScroll_ = right - stripW + (vm_.activeIndex.get() + 1 < count ? 24.0f * s_ : 0.0f);
    }
    // The wheel scrolls the strip.
    const bool overStrip = ImGui::IsMouseHoveringRect(ImVec2(x0, y), ImVec2(stripX1, y + height)) && ImGui::IsWindowHovered();
    if (overStrip && maxScroll > 0.0f)
        tabScroll_ -= (io.MouseWheel + io.MouseWheelH) * 80.0f * s_;
    tabScroll_ = std::clamp(tabScroll_, 0.0f, maxScroll);
    tabScrollShown_ = std::clamp(tabScrollShown_, 0.0f, maxScroll); // tabs closed: no stale offset
    tabScrollShown_ += (tabScroll_ - tabScrollShown_) * std::min(1.0f, io.DeltaTime * 16.0f);
    if (std::fabs(tabScroll_ - tabScrollShown_) < 0.5f)
        tabScrollShown_ = tabScroll_;

    int closeIndex = -1;
    int moveFrom = -1, moveTo = -1;
    ImGui::PushID("documentTabs");
    ImGui::PushClipRect(ImVec2(x0, y), ImVec2(stripX1, y + height), true); // clips hovering too
    float tx = x0 - tabScrollShown_;
    for (int i = 0; i < count; ++i) {
        const model::Document& doc = *docs[size_t(i)];
        const float w = widths[size_t(i)];
        const bool active = i == vm_.activeIndex.get();
        const ImVec2 min(tx, tabTop), max(tx + w - 2.0f * s_, tabTop + tabH);
        tx += w;
        const std::string automationId = "document:" + (doc.path.empty() ? doc.title : doc.path);
        auto activateTab = [this, i]() {
            vm_.activate(i);
            previewTabRequest_ = 1;
        };
        if (max.x <= x0 || min.x >= stripX1) {
            // Scrolled out of view: still known to agents (activating scrolls it in).
            UiAutomation::instance().add({automationId, "tab", doc.title, ImVec2(x0, tabTop), ImVec2(x0, tabTop), true, activateTab});
            continue;
        }
        ImGui::PushID(doc.id);
        ImGui::SetCursorScreenPos(min);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##tab", ImVec2(std::max(1.0f, max.x - min.x), tabH)))
            activateTab();
        registerLastItem(automationId, "tab", doc.title, activateTab);
        const bool hovered = ImGui::IsItemHovered();
        const bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 4.0f * s_);
        if (dragging) {
            // Live reordering: swap with a neighbour once the mouse is well over it (no flicker
            // between tabs of different widths).
            if (i > 0 && io.MousePos.x < min.x - widths[size_t(i - 1)] + std::min(w, widths[size_t(i - 1)]) * 0.5f) {
                moveFrom = i;
                moveTo = i - 1;
            } else if (i + 1 < count && io.MousePos.x > min.x + w + widths[size_t(i + 1)] - std::min(w, widths[size_t(i + 1)]) * 0.5f) {
                moveFrom = i;
                moveTo = i + 1;
            }
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        }
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            closeIndex = i;
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            documentList_.requestMenu(i);
        }
        if (hovered && !dragging)
            ImGui::SetItemTooltip("%s%s", doc.path.empty() ? doc.title.c_str() : doc.path.c_str(),
                                  doc.modified ? "\n(unsaved changes)" : "");
        if (active || dragging) {
            dl->AddRectFilled(min, max, palette_.ribbonBg, kRounding * s_, ImDrawFlags_RoundTop);
            dl->AddLine(ImVec2(min.x + kRounding * s_, min.y + 1.0f), ImVec2(max.x - kRounding * s_, min.y + 1.0f),
                        palette_.accent, 2.0f * s_);
        } else if (hovered) {
            dl->AddRectFilled(min, max, palette_.hover, kRounding * s_, ImDrawFlags_RoundTop);
        } else if (i + 1 != vm_.activeIndex.get() && i + 1 < count) {
            // Hairline separators between inactive tabs.
            dl->AddLine(ImVec2(max.x + 1.0f * s_, min.y + tabH * 0.28f), ImVec2(max.x + 1.0f * s_, max.y - tabH * 0.28f), palette_.border);
        }

        const float textY = min.y + (tabH - ImGui::GetTextLineHeight()) * 0.5f;
        const char* icon = fileIcon(doc.title);
        dl->AddText(ImVec2(min.x + pad, textY), active ? palette_.accent : palette_.textFaint, icon);
        const float textX = min.x + pad + ImGui::CalcTextSize(icon).x + 6.0f * s_;
        const float textEnd = max.x - closeSize - 8.0f * s_;
        dl->PushClipRect(ImVec2(textX, min.y), ImVec2(textEnd, max.y), true);
        // Git: a changed file's title takes its status colour (as in the Files tree).
        const char gitStatus = doc.onDisk ? vm_.git().statusOf(doc.path) : 0;
        const ImU32 titleColor = gitStatus == 'A' || gitStatus == 'U' ? palette_.diffAddedText
                                 : gitStatus == '!'                   ? palette_.iconOrange
                                 : gitStatus == 'M' || gitStatus == 'R' ? palette_.severityWarning
                                 : active                             ? palette_.text
                                                                      : palette_.textDim;
        dl->AddText(ImVec2(textX, textY), titleColor, doc.title.c_str());
        if (!hints[size_t(i)].empty())
            dl->AddText(ImVec2(textX + ImGui::CalcTextSize(doc.title.c_str()).x + 6.0f * s_, textY), palette_.textFaint,
                        hints[size_t(i)].c_str());
        dl->PopClipRect();
        if (textX + ImGui::CalcTextSize(doc.title.c_str()).x > textEnd) // cut: fade into the tab
            dl->AddRectFilledMultiColor(ImVec2(textEnd - 18.0f * s_, min.y + 3.0f * s_), ImVec2(textEnd, max.y - 1.0f),
                                        withAlpha(active ? palette_.ribbonBg : palette_.titleBarBg, 0),
                                        active ? palette_.ribbonBg : palette_.titleBarBg,
                                        active ? palette_.ribbonBg : palette_.titleBarBg,
                                        withAlpha(active ? palette_.ribbonBg : palette_.titleBarBg, 0));

        // Close button, or the "modified" dot while not hovered.
        const ImVec2 closeMin(max.x - closeSize - 6.0f * s_, min.y + (tabH - closeSize) * 0.5f);
        ImGui::SetCursorScreenPos(closeMin);
        if (ImGui::InvisibleButton("##close", ImVec2(closeSize, closeSize)))
            closeIndex = i;
        const bool closeHovered = ImGui::IsItemHovered();
        if (closeHovered)
            dl->AddRectFilled(closeMin, ImVec2(closeMin.x + closeSize, closeMin.y + closeSize), palette_.hover, 3.0f * s_);
        const ImVec2 center(closeMin.x + closeSize * 0.5f, closeMin.y + closeSize * 0.5f);
        if (doc.modified && !hovered && !closeHovered) {
            dl->AddCircleFilled(center, 3.5f * s_, active ? palette_.accent : palette_.textDim);
        } else if (hovered || closeHovered || active) {
            const float r = closeSize * 0.22f;
            const ImU32 c = closeHovered ? palette_.text : palette_.textFaint;
            dl->AddLine(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r), c, 1.5f * s_);
            dl->AddLine(ImVec2(center.x - r, center.y + r), ImVec2(center.x + r, center.y - r), c, 1.5f * s_);
        }
        ImGui::PopID();
    }
    ImGui::PopClipRect();
    // Fades where tabs continue out of view.
    const float fade = 28.0f * s_;
    const ImU32 bg = palette_.titleBarBg, clear = withAlpha(palette_.titleBarBg, 0);
    if (tabScrollShown_ > 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(x0, y), ImVec2(x0 + fade, y + height), bg, clear, clear, bg);
    if (tabScrollShown_ < maxScroll - 0.5f)
        dl->AddRectFilledMultiColor(ImVec2(stripX1 - fade, y), ImVec2(stripX1, y + height), clear, bg, bg, clear);

    const float buttonY = y + (height - ImGui::GetFrameHeight()) * 0.5f;
    // Right after the last tab, never left of the strip (the smoothed scroll lags when tabs close).
    ImGui::SetCursorScreenPos(ImVec2(std::clamp(x0 + total - tabScrollShown_, x0, stripX1) + 2.0f * s_, buttonY));
    if (flatButton(ICON_FA_PLUS "##newTab", "New document (Ctrl+N)"))
        vm_.execute(CommandId::NewFile);
    float end = ImGui::GetItemRectMax().x;
    if (overflow) {
        ImGui::SameLine(0.0f, 2.0f * s_);
        if (flatButton((listLabel + "##allTabs").c_str(), "All open documents"))
            ImGui::OpenPopup("##allTabs");
        registerLastItem("all-documents", "button", "All open documents", [this]() { openAllTabs_ = true; });
        if (openAllTabs_) {
            openAllTabs_ = false;
            ImGui::OpenPopup("##allTabs");
        }
        end = ImGui::GetItemRectMax().x;
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetItemRectMax().x, y + height), ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        renderAllTabsPopup();
    }
    ImGui::PopID();
    if (moveFrom >= 0)
        vm_.moveDocument(moveFrom, moveTo);
    if (closeIndex >= 0)
        vm_.requestCloseDocument(closeIndex);
    return end;
}

void MainView::renderTerminalWindow()
{
    if (!vm_.terminalPoppedOut.get())
        return;
    // Its own OS window (multi-viewport: never merged back into the main window), with an
    // ImGui-drawn title bar in the theme's colours. Closing it puts the terminal back in the panel.
    ImGuiWindowClass windowClass;
    windowClass.ViewportFlagsOverrideSet = ImGuiViewportFlags_NoAutoMerge;
    ImGui::SetNextWindowClass(&windowClass);
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x * 0.2f, main->Pos.y + main->Size.y * 0.25f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(main->Size.x * 0.6f, main->Size.y * 0.5f), ImGuiCond_FirstUseEver);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, col(palette_.panelBg));
    bool open = true;
    const viewmodel::TerminalInstance* active = vm_.terminal().active();
    const std::string title = std::string(ICON_FA_TERMINAL "  ") + (active ? (active->screen.title().empty() ? active->name : active->screen.title()) : "Terminal") +
                              "###terminalWindow";
    const bool visible = ImGui::Begin(title.c_str(), &open, ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    if (visible) {
        ImGui::PushID("terminalWindow");
        terminalView_.render(ImGui::GetContentRegionAvail(), vm_.zoom.get());
        ImGui::PopID();
    }
    ImGui::End();
    if (!open)
        vm_.terminalPoppedOut.set(false);
}

void MainView::renderHashResult()
{
    if (!vm_.hashResult.get().empty() && !ImGui::IsPopupOpen("SHA-256"))
        ImGui::OpenPopup("SHA-256");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("SHA-256", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (vm_.hashResult.get().empty()) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const model::Document* doc = vm_.activeDocument();
    ImGui::TextDisabled("Of the %s", doc && doc->hasSelection() ? "selection" : "whole document");
    std::string value = vm_.hashResult.get();
    ImGui::PushFont(fonts_.mono, Fonts::kEditorSize);
    ImGui::SetNextItemWidth(ImGui::CalcTextSize(value.c_str()).x + 16.0f * s_);
    ImGui::InputText("##hash", &value, ImGuiInputTextFlags_ReadOnly);
    ImGui::PopFont();
    if (ImGui::Button(ICON_FA_COPY "  Copy"))
        ImGui::SetClipboardText(vm_.hashResult.get().c_str());
    ImGui::SameLine();
    if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        vm_.hashResult.set({});
        ImGui::CloseCurrentPopup();
    }
    registerLastItem("dialog:hash:close", "button", "Close", [this]() { vm_.hashResult.set({}); });
    ImGui::EndPopup();
}

void MainView::renderLanguagePicker()
{
    // Every language of the table, filtered as you type; Enter takes the first match.
    if (openLanguagePicker_) {
        ImGui::OpenPopup("##languagePicker");
        openLanguagePicker_ = false;
        languageFilter_.clear();
    }
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x * 0.5f, main->Pos.y + 120.0f * s_), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(320.0f * s_, 0.0f));
    if (!ImGui::BeginPopup("##languagePicker"))
        return;
    model::Document* doc = vm_.activeDocument();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Language or extension", &languageFilter_,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    registerLastItem("language-picker:filter", "textbox", "Language", nullptr);
    std::string needle = languageFilter_;
    for (char& c : needle)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    auto matches = [&](const model::LanguageSpec& spec) {
        if (needle.empty())
            return true;
        std::string name = spec.name;
        for (char& c : name)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        if (name.find(needle) != std::string::npos || std::string(spec.key).find(needle) != std::string::npos)
            return true;
        for (const std::string& ext : spec.extensions)
            if (ext == needle || ext == "." + needle)
                return true;
        return false;
    };
    const model::LanguageSpec* first = nullptr;
    ImGui::BeginChild("##languages", ImVec2(0.0f, std::min(420.0f * s_, float(model::languages().size()) * ImGui::GetTextLineHeightWithSpacing() + 8.0f * s_)));
    for (const model::LanguageSpec& spec : model::languages()) {
        if (!matches(spec))
            continue;
        if (!first)
            first = &spec;
        const bool current = doc && doc->language == spec.id;
        const std::string extensions = spec.extensions.empty() ? std::string() : spec.extensions.front();
        if (ImGui::MenuItem(spec.name, extensions.c_str(), current)) {
            vm_.setDocumentLanguage(spec.id);
            ImGui::CloseCurrentPopup();
        }
        registerLastItem(std::string("language-picker:") + spec.key, "button", spec.name,
                         [this, id = spec.id]() { vm_.setDocumentLanguage(id); });
    }
    if (!first)
        ImGui::TextDisabled("No language matches");
    ImGui::EndChild();
    if (enter && first) {
        vm_.setDocumentLanguage(first->id);
        ImGui::CloseCurrentPopup();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_PALETTE "  Customize colours..."))
        openSyntaxColors_ = true;
    ImGui::EndPopup();
}

void MainView::renderSaveChangesPrompt()
{
    using Choice = viewmodel::NotepadViewModel::CloseChoice;
    // The popup follows the view-model: answered elsewhere (an agent, a test), it closes too.
    const int index = vm_.documentIndexById(vm_.pendingClose.get());
    if (index >= 0 && !ImGui::IsPopupOpen("Save changes?"))
        ImGui::OpenPopup("Save changes?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Save changes?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (index < 0) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const model::Document& doc = *vm_.documents()[size_t(index)];
    ImGui::Text(ICON_FA_CIRCLE_EXCLAMATION "  \"%s\" has unsaved changes.", doc.title.c_str());
    ImGui::TextDisabled("Save them before closing?");
    ImGui::Spacing();
    const bool inPlace = vm_.canSaveInPlace(doc);
    std::optional<Choice> choice;
    if (ImGui::Button(inPlace ? ICON_FA_FLOPPY_DISK "  Save" : ICON_FA_FLOPPY_DISK "  Save As...", ImVec2(110.0f * s_, 0.0f)) ||
        ImGui::IsKeyPressed(ImGuiKey_Enter))
        choice = Choice::Save;
    if (!inPlace)
        ImGui::SetItemTooltip("Untitled: choose where to save it; it closes once saved");
    registerLastItem("dialog:save-changes:save", "button", "Save", [this]() { vm_.resolvePendingClose(Choice::Save); });
    ImGui::SameLine();
    if (ImGui::Button("Don't Save", ImVec2(110.0f * s_, 0.0f)))
        choice = Choice::Discard;
    registerLastItem("dialog:save-changes:discard", "button", "Don't Save", [this]() { vm_.resolvePendingClose(Choice::Discard); });
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        choice = Choice::Cancel;
    registerLastItem("dialog:save-changes:cancel", "button", "Cancel", [this]() { vm_.resolvePendingClose(Choice::Cancel); });
    if (choice) {
        ImGui::CloseCurrentPopup();
        vm_.resolvePendingClose(*choice);
    }
    ImGui::EndPopup();
}

void MainView::renderQuitPrompt()
{
    using Choice = viewmodel::NotepadViewModel::CloseChoice;
    const bool pending = vm_.quitConfirmPending.get();
    if (pending && !ImGui::IsPopupOpen("Quit EasyNotepad?"))
        ImGui::OpenPopup("Quit EasyNotepad?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Quit EasyNotepad?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (!pending) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    const auto unsaved = vm_.unsavedDocuments();
    ImGui::Text(ICON_FA_CIRCLE_EXCLAMATION "  %d document(s) have unsaved changes:", int(unsaved.size()));
    bool allSavable = true;
    for (const model::Document* doc : unsaved) {
        const bool savable = vm_.canSaveInPlace(*doc);
        allSavable = allSavable && savable;
        ImGui::BulletText("%s%s", doc->path.empty() ? doc->title.c_str() : doc->path.c_str(), savable ? "" : "  (untitled)");
    }
    ImGui::Spacing();
    std::optional<Choice> choice;
    ImGui::BeginDisabled(!allSavable);
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save All and Quit"))
        choice = Choice::Save;
    ImGui::EndDisabled();
    if (!allSavable)
        ImGui::SetItemTooltip("Untitled documents need Save As first");
    registerLastItem("dialog:quit:save", "button", "Save All and Quit", [this]() { vm_.resolveQuit(Choice::Save); }, allSavable);
    ImGui::SameLine();
    if (ImGui::Button("Quit Without Saving"))
        choice = Choice::Discard;
    registerLastItem("dialog:quit:discard", "button", "Quit Without Saving", [this]() { vm_.resolveQuit(Choice::Discard); });
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        choice = Choice::Cancel;
    registerLastItem("dialog:quit:cancel", "button", "Cancel", [this]() { vm_.resolveQuit(Choice::Cancel); });
    if (choice) {
        ImGui::CloseCurrentPopup();
        vm_.resolveQuit(*choice);
    }
    ImGui::EndPopup();
}

void MainView::renderAllTabsPopup()
{
    if (!ImGui::BeginPopup("##allTabs"))
        return;
    if (ImGui::IsWindowAppearing()) {
        allTabsFilter_.clear();
        ImGui::SetKeyboardFocusHere();
    }
    ImGui::SetNextItemWidth(340.0f * s_);
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Filter open documents", &allTabsFilter_);
    ImGui::Separator();
    const float rowsH = float(vm_.documents().size()) * ImGui::GetFrameHeight() + 8.0f * s_;
    ImGui::BeginChild("##list", ImVec2(340.0f * s_, std::min(420.0f * s_, rowsH)));
    if (documentList_.render("all-documents", allTabsFilter_, {false}, true) >= 0)
        ImGui::CloseCurrentPopup();
    ImGui::EndChild();
    ImGui::Separator();
    if (ImGui::Button(ICON_FA_CIRCLE_CHECK "  Close Saved"))
        vm_.closeSaved();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_BROOM "  Close All"))
        vm_.closeAll();
    ImGui::EndPopup();
}

float MainView::renderWindowButtons(float right, float y, float height)
{
    // Minimize, maximize/restore and close, drawn as thin line glyphs like the OS caption buttons.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float w = 46.0f * s_;
    const float g = 5.0f * s_; // glyph half-size
    const float thickness = std::max(1.0f, s_);
    const bool maximized = frame_->isMaximized();
    float x = right - 3.0f * w;
    const float left = x;
    ImGui::PushID("windowButtons");
    for (int i = 0; i < 3; ++i, x += w) {
        ImGui::SetCursorScreenPos(ImVec2(x, y));
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##windowButton", ImVec2(w, height));
        ImGui::PopID();
        const bool hovered = ImGui::IsItemHovered();
        const bool close = i == 2;
        if (hovered)
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + height),
                              close ? IM_COL32(196, 43, 28, 255) : palette_.hover);
        const ImU32 c = hovered && close ? IM_COL32(255, 255, 255, 255) : palette_.text;
        const ImVec2 m(std::floor(x + w * 0.5f) + 0.5f, std::floor(y + height * 0.5f) + 0.5f);
        if (i == 0) {
            dl->AddLine(ImVec2(m.x - g, m.y), ImVec2(m.x + g, m.y), c, thickness);
            ImGui::SetItemTooltip("Minimize");
            if (clicked)
                frame_->minimize();
        } else if (i == 1) {
            if (maximized) {
                const float o = 2.0f * s_;
                dl->AddRect(ImVec2(m.x - g, m.y - g + o), ImVec2(m.x + g - o, m.y + g), c, 0.0f, thickness);
                dl->AddLine(ImVec2(m.x - g + o, m.y - g + o), ImVec2(m.x - g + o, m.y - g), c, thickness);
                dl->AddLine(ImVec2(m.x - g + o, m.y - g), ImVec2(m.x + g, m.y - g), c, thickness);
                dl->AddLine(ImVec2(m.x + g, m.y - g), ImVec2(m.x + g, m.y + g - o), c, thickness);
                dl->AddLine(ImVec2(m.x + g, m.y + g - o), ImVec2(m.x + g - o, m.y + g - o), c, thickness);
            } else {
                dl->AddRect(ImVec2(m.x - g, m.y - g), ImVec2(m.x + g, m.y + g), c, 0.0f, thickness);
            }
            ImGui::SetItemTooltip(maximized ? "Restore" : "Maximize");
            if (clicked)
                frame_->toggleMaximize();
        } else {
            dl->AddLine(ImVec2(m.x - g, m.y - g), ImVec2(m.x + g, m.y + g), c, thickness);
            dl->AddLine(ImVec2(m.x - g, m.y + g), ImVec2(m.x + g, m.y - g), c, thickness);
            ImGui::SetItemTooltip("Close");
            if (clicked)
                vm_.requestQuit(); // unsaved documents are asked about first
        }
    }
    ImGui::PopID();
    return left;
}

void MainView::handleCaptionMouse()
{
    if (!frame_)
        return;
    const ImGuiMouseCursor cursor = frame_->resizeCursor();
    if (cursor != ImGuiMouseCursor_None) {
        ImGui::SetMouseCursor(cursor);
        return;
    }
    // Only reached where the OS does not hit-test the caption itself (see IWindowFrame).
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsAnyItemHovered())
        return;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    for (const ScreenRect& r : dragRegions_) {
        if (!r.contains(mouse))
            continue;
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
            frame_->toggleMaximize();
        else
            frame_->beginDrag();
        return;
    }
}

// ---- Panels and splitters -----------------------------------------------------------------------

bool MainView::beginPanel(const char* id, const char* title, ImVec2 size, const char* detail)
{
    // The border is drawn when the child begins, under its content: the 1px padding keeps the
    // content inside it, and panel::fill rounds content backgrounds that reach the corners.
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kRounding * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(title ? palette_.panelBg : palette_.editorBg));
    ImGui::PushStyleColor(ImGuiCol_Border, col(palette_.border));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    const ImVec2 outer = ImGui::GetWindowPos();
    const ImVec2 inner(outer.x + 1.0f, outer.y + 1.0f);
    const ImVec2 innerMax(outer.x + ImGui::GetWindowWidth() - 1.0f, outer.y + ImGui::GetWindowHeight() - 1.0f);
    panel::push(inner, innerMax, std::max(0.0f, kRounding * s_ - 1.0f));
    panelHasHeader_.push_back(title != nullptr);
    if (!title)
        return true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = inner;
    const float w = innerMax.x - inner.x;
    const float h = kPanelHeader * s_;
    panel::fill(dl, p, ImVec2(p.x + w, p.y + h), palette_.panelHeaderBg);
    dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h), palette_.border);
    const float textY = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(p.x + 10.0f * s_, textY), palette_.text, title);
    if (detail) {
        const float dw = ImGui::CalcTextSize(detail).x;
        dl->AddText(ImVec2(p.x + w - dw - 10.0f * s_, textY), palette_.textFaint, detail);
    }
    ImGui::SetCursorPos(ImVec2(1.0f, h + 2.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f * s_, 6.0f * s_));
    ImGui::BeginChild("##content", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    return true;
}

void MainView::endPanel()
{
    if (panelHasHeader_.back())
        ImGui::EndChild(); // content
    panelHasHeader_.pop_back();

    panel::pop();
    ImGui::EndChild();
}

void MainView::verticalSplitter(const char* id, float& value, float direction, float height, float minValue, float maxValue)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = kGap * s_;
    ImGui::InvisibleButton(id, ImVec2(w, std::max(1.0f, height)));
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + w * 0.5f, p.y + 4.0f * s_), ImVec2(p.x + w * 0.5f, p.y + height - 4.0f * s_),
                                            palette_.accent, 2.0f * s_);
    }
    if (active)
        value = std::clamp(value + ImGui::GetIO().MouseDelta.x * direction, minValue, std::max(minValue, maxValue));
    if (ImGui::IsItemDeactivated())
        vm_.settings().markDirty(); // remembered with the layout
}

void MainView::horizontalSplitter(const char* id, float& value, float width, float minValue, float maxValue)
{
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = kGap * s_;
    ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, width), h));
    const bool active = ImGui::IsItemActive();
    if (ImGui::IsItemHovered() || active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 4.0f * s_, p.y + h * 0.5f), ImVec2(p.x + width - 4.0f * s_, p.y + h * 0.5f),
                                            palette_.accent, 2.0f * s_);
    }
    if (active)
        value = std::clamp(value - ImGui::GetIO().MouseDelta.y, minValue, std::max(minValue, maxValue));
    if (ImGui::IsItemDeactivated())
        vm_.settings().markDirty();
}

// ---- Workspaces ---------------------------------------------------------------------------------

void MainView::renderCodeWorkspace()
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos(); // rows are placed explicitly: no item spacing
    const float gap = kGap * s_;
    const bool panel = vm_.showPanel.get();
    if (panel && vm_.panelMaximized.get()) {
        renderBottomPanel(avail.x, avail.y);
        return;
    }
    panelHeight_ = std::clamp(panelHeight_, 90.0f * s_, std::max(90.0f * s_, avail.y - 160.0f * s_));
    const float topH = panel ? avail.y - panelHeight_ - gap : avail.y;

    if (vm_.showExplorer.get()) {
        beginPanel("##explorer", ICON_FA_SITEMAP "  Explorer", ImVec2(explorerWidth_, topH), vm_.explorer().name.c_str());
        renderExplorer();
        endPanel();
        ImGui::SameLine(0.0f, 0.0f);
        verticalSplitter("##splitExplorer", explorerWidth_, 1.0f, topH, 140.0f * s_, avail.x * 0.4f);
        ImGui::SameLine(0.0f, 0.0f);
    }
    const bool side = vm_.showAgentPanel.get();
    const float sideW = side ? agentSideWidth_ + gap : 0.0f;
    const float editorW = std::max(100.0f * s_, ImGui::GetContentRegionAvail().x - sideW);
    beginPanel("##editorPanel", nullptr, ImVec2(editorW, topH));
    if (vm_.editorMode.get() == viewmodel::EditorMode::Map)
        map_.render(ImGui::GetContentRegionAvail());
    else
        editor_.render(ImGui::GetContentRegionAvail());
    endPanel();
    if (side) {
        ImGui::SameLine(0.0f, 0.0f);
        verticalSplitter("##splitAgent", agentSideWidth_, -1.0f, topH, 260.0f * s_, avail.x * 0.5f);
        ImGui::SameLine(0.0f, 0.0f);
        const char* busy = vm_.agent().busy.get() ? "working..." : nullptr;
        beginPanel("##agentSide", ICON_FA_ROBOT "  Agent", ImVec2(agentSideWidth_, topH), busy);
        agentPanel_.renderConversation(true);
        endPanel();
    }
    if (panel) {
        // topH + gap + panelHeight_ == avail.y exactly (ImGui's item spacing would push the
        // bottom panel's lower edge under the status bar).
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + topH));
        horizontalSplitter("##splitPanel", panelHeight_, avail.x, 90.0f * s_, avail.y - 160.0f * s_);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + topH + gap));
        renderBottomPanel(avail.x, panelHeight_);
    }
}

void MainView::renderAgentWorkspace()
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const ImVec2 origin = ImGui::GetCursorScreenPos(); // rows are placed explicitly: no item spacing
    const float gap = kGap * s_;
    const bool panel = vm_.showPanel.get();
    if (panel && vm_.panelMaximized.get()) {
        renderBottomPanel(avail.x, avail.y);
        return;
    }
    panelHeight_ = std::clamp(panelHeight_, 90.0f * s_, std::max(90.0f * s_, avail.y - 160.0f * s_));
    const float topH = panel ? avail.y - panelHeight_ - gap : avail.y;

    if (vm_.showExplorer.get()) {
        const std::string detail = std::to_string(vm_.agent().pendingChangeCount()) + " pending";
        beginPanel("##plan", ICON_FA_LIST_CHECK "  Plan & Changes", ImVec2(planWidth_, topH), detail.c_str());
        agentPanel_.renderPlan();
        endPanel();
        ImGui::SameLine(0.0f, 0.0f);
        verticalSplitter("##splitPlan", planWidth_, 1.0f, topH, 200.0f * s_, avail.x * 0.35f);
        ImGui::SameLine(0.0f, 0.0f);
    }
    const char* busy = vm_.agent().busy.get() ? "working..." : nullptr;
    beginPanel("##conversation", ICON_FA_ROBOT "  Agent", ImVec2(conversationWidth_, topH), busy);
    agentPanel_.renderConversation(false);
    endPanel();
    ImGui::SameLine(0.0f, 0.0f);
    verticalSplitter("##splitConversation", conversationWidth_, 1.0f, topH, 300.0f * s_, avail.x * 0.6f);
    ImGui::SameLine(0.0f, 0.0f);
    const float previewW = std::max(100.0f * s_, ImGui::GetContentRegionAvail().x);
    beginPanel("##preview", ICON_FA_CODE_COMPARE "  Preview", ImVec2(previewW, topH));
    renderPreview(ImGui::GetContentRegionAvail());
    endPanel();

    if (panel) {
        // topH + gap + panelHeight_ == avail.y exactly (ImGui's item spacing would push the
        // bottom panel's lower edge under the status bar).
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + topH));
        horizontalSplitter("##splitPanel", panelHeight_, avail.x, 90.0f * s_, avail.y - 160.0f * s_);
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + topH + gap));
        renderBottomPanel(avail.x, panelHeight_);
    }
}

void MainView::renderBottomPanel(float width, float height)
{
    using viewmodel::PanelTab;
    ImGui::PushID("bottomPanel");
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kRounding * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(1.0f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(palette_.panelBg));
    ImGui::PushStyleColor(ImGuiCol_Border, col(palette_.border));
    ImGui::BeginChild("##panel", ImVec2(width, height), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 outer = ImGui::GetWindowPos();
    const ImVec2 p(outer.x + 1.0f, outer.y + 1.0f);
    const float w = ImGui::GetWindowWidth() - 2.0f;
    panel::push(p, ImVec2(p.x + w, outer.y + ImGui::GetWindowHeight() - 1.0f), std::max(0.0f, kRounding * s_ - 1.0f));
    const float headerH = kPanelHeader * s_ + 2.0f * s_;
    panel::fill(dl, p, ImVec2(p.x + w, p.y + headerH), palette_.panelHeaderBg);
    dl->AddLine(ImVec2(p.x, p.y + headerH), ImVec2(p.x + w, p.y + headerH), palette_.border);

    // Tabs: upper-case captions with counts, the active one underlined (VS Code's panel style).
    const viewmodel::ReviewViewModel& review = vm_.review();
    int openComments = 0, done = 0;
    for (const model::ReviewComment& c : review.comments())
        openComments += c.resolved ? 0 : 1;
    for (const model::ChecklistItem& item : review.checklist())
        done += item.done ? 1 : 0;
    const int problems = review.count(model::Severity::Error) + review.count(model::Severity::Warning) +
                         review.count(model::Severity::Info);
    const int terminals = int(vm_.terminal().terminals().size());
    struct TabInfo {
        PanelTab tab;
        const char* label;
        std::string badge;
    };
    const TabInfo tabs[] = {
        {PanelTab::Problems, "PROBLEMS", std::to_string(problems)},
        {PanelTab::Diff, "DIFF", {}},
        {PanelTab::Comments, "COMMENTS", openComments ? std::to_string(openComments) : std::string()},
        {PanelTab::Checklist, "CHECKLIST", std::to_string(done) + "/" + std::to_string(review.checklist().size())},
        {PanelTab::Terminal, "TERMINAL", terminals > 1 ? std::to_string(terminals) : std::string()},
        {PanelTab::Search, "SEARCH", vm_.findInFiles().hitCount() ? std::to_string(vm_.findInFiles().hitCount()) : std::string()},
        {PanelTab::History, "HISTORY", {}},
    };
    const PanelTab current = vm_.panelTab.get();
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    float x = p.x + 8.0f * s_;
    const float textY = p.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f;
    for (const TabInfo& t : tabs) {
        const float labelW = ImGui::CalcTextSize(t.label).x;
        const float badgeW = t.badge.empty() ? 0.0f : ImGui::CalcTextSize(t.badge.c_str()).x + 12.0f * s_;
        const float tabW = labelW + badgeW + 20.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(int(t.tab));
        if (ImGui::InvisibleButton("##panelTab", ImVec2(tabW, headerH)))
            vm_.showPanelTab(t.tab);
        {
            std::string name = t.label;
            for (size_t k = 1; k < name.size(); ++k)
                name[k] = char(std::tolower(static_cast<unsigned char>(name[k])));
            const PanelTab tab = t.tab;
            registerLastItem("panel-tab:" + name, "tab", name, [this, tab]() { vm_.showPanelTab(tab); });
        }
        ImGui::PopID();
        const bool active = t.tab == current;
        const bool hovered = ImGui::IsItemHovered();
        dl->AddText(ImVec2(x + 10.0f * s_, textY), active || hovered ? palette_.text : palette_.textDim, t.label);
        if (!t.badge.empty()) {
            const ImVec2 b0(x + 10.0f * s_ + labelW + 6.0f * s_, textY - 1.0f * s_);
            const ImVec2 b1(b0.x + badgeW - 6.0f * s_, textY + ImGui::GetTextLineHeight() + 1.0f * s_);
            dl->AddRectFilled(b0, b1, palette_.accentSoft, 8.0f * s_);
            dl->AddText(ImVec2(b0.x + 3.0f * s_, textY), palette_.text, t.badge.c_str());
        }
        if (active)
            dl->AddRectFilled(ImVec2(x + 8.0f * s_, p.y + headerH - 2.0f * s_), ImVec2(x + tabW - 8.0f * s_, p.y + headerH),
                              palette_.accent);
        x += tabW;
    }
    ImGui::PopFont();
    renderPanelActions(x + 8.0f * s_, p.x + w - 6.0f * s_, p.y, headerH);

    // Content.
    ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + headerH + 1.0f));
    const bool terminalTab = current == PanelTab::Terminal;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, terminalTab ? ImVec2(0.0f, 0.0f) : ImVec2(8.0f * s_, 6.0f * s_));
    ImGui::BeginChild("##panelContent", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AlwaysUseWindowPadding,
                      terminalTab ? ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse : 0);
    ImGui::PopStyleVar();
    if (terminalTab && vm_.terminalPoppedOut.get()) {
        ImGui::SetCursorPos(ImVec2(12.0f * s_, 12.0f * s_));
        ImGui::TextDisabled(ICON_FA_UP_RIGHT_FROM_SQUARE "  The terminal is in its own window.");
        ImGui::SetCursorPosX(12.0f * s_);
        if (ImGui::Button(ICON_FA_DOWN_LEFT_AND_UP_RIGHT_TO_CENTER "  Bring it back"))
            vm_.execute(CommandId::PopOutTerminal);
        registerLastItem("terminal-dock", "button", "Bring the terminal back", [this]() { vm_.execute(CommandId::PopOutTerminal); });
    } else if (terminalTab)
        terminalView_.render(ImGui::GetContentRegionAvail(), vm_.zoom.get());
    else if (current == PanelTab::Search)
        searchPanel_.render();
    else if (current == PanelTab::History)
        gitView_.renderHistory();
    else
        reviewPanel_.renderTab(current);
    ImGui::EndChild();
    panel::pop();
    ImGui::EndChild();
    ImGui::PopID();
}

void MainView::renderPanelActions(float left, float right, float y, float height)
{
    const float buttonY = y + (height - ImGui::GetFrameHeight()) * 0.5f;
    float x = right;
    auto action = [&](const char* label, const char* tooltip, bool enabled = true) {
        x -= ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f + 2.0f * s_;
        if (x < left)
            return false;
        ImGui::SetCursorScreenPos(ImVec2(x, buttonY));
        return flatButton(label, tooltip, enabled);
    };
    if (action(ICON_FA_XMARK "##closePanel", "Close panel (Ctrl+J)")) {
        vm_.showPanel.set(false);
        vm_.panelMaximized.set(false);
    }
    const bool maximized = vm_.panelMaximized.get();
    if (action(maximized ? ICON_FA_CHEVRON_DOWN "##restorePanel" : ICON_FA_CHEVRON_UP "##maximizePanel",
               maximized ? "Restore panel size" : "Maximize panel"))
        vm_.execute(CommandId::ToggleMaximizePanel);
    if (vm_.panelTab.get() != viewmodel::PanelTab::Terminal)
        return;

    viewmodel::TerminalViewModel& terminal = vm_.terminal();
    x -= 8.0f * s_;
    if (action(ICON_FA_UP_RIGHT_FROM_SQUARE "##popOutTerminal", "Pop out: the terminal in its own window", terminal.supported()))
        vm_.execute(CommandId::PopOutTerminal);
    registerLastItem("command:PopOutTerminal", "button", "Pop out terminal", [this]() { vm_.execute(CommandId::PopOutTerminal); });
    if (action(ICON_FA_TRASH_CAN "##killTerminal", "Kill terminal", terminal.active() != nullptr))
        vm_.execute(CommandId::KillTerminal);
    if (action(ICON_FA_ERASER "##clearTerminal", "Clear terminal", terminal.active() != nullptr))
        vm_.execute(CommandId::ClearTerminal);
    if (action(ICON_FA_CARET_DOWN "##shellMenu", "Choose a shell for a new terminal", terminal.supported()))
        ImGui::OpenPopup("##shells");
    ImGui::SetNextWindowPos(ImVec2(x, y + height), ImGuiCond_Appearing, ImVec2(0.0f, 0.0f));
    if (ImGui::BeginPopup("##shells")) {
        ImGui::TextDisabled("NEW TERMINAL");
        const auto& shells = terminal.shells();
        for (int i = 0; i < int(shells.size()); ++i) {
            ImGui::PushID(i);
            const std::string label = std::string(ICON_FA_TERMINAL "  ") + shells[size_t(i)].name;
            if (ImGui::MenuItem(label.c_str())) {
                vm_.showPanelTab(viewmodel::PanelTab::Terminal);
                terminal.newTerminal(i);
            }
            ImGui::SetItemTooltip("%s", shells[size_t(i)].command.c_str());
            ImGui::PopID();
        }
        ImGui::EndPopup();
    }
    x += 2.0f * s_; // the "+" and its drop-down read as one split button
    if (action(ICON_FA_PLUS "##newTerminal", "New terminal (Ctrl+Shift+`)", terminal.supported()))
        vm_.execute(CommandId::NewTerminal);
}

void MainView::renderPreview(ImVec2 size)
{
    const float startY = ImGui::GetCursorPosY();
    if (ImGui::BeginTabBar("##previewTabs")) {
        const model::Document* doc = vm_.activeDocument();
        const std::string editorLabel = std::string(ICON_FA_PEN_TO_SQUARE "  ") + (doc ? doc->title : "Editor") + "###editorTab";
        const int request = previewTabRequest_;
        previewTabRequest_ = -1;
        if (ImGui::BeginTabItem(ICON_FA_CODE_BRANCH "  Proposed change", nullptr, request == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            previewTab_ = 0;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem(editorLabel.c_str(), nullptr, request == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            previewTab_ = 1;
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    const ImVec2 rest(size.x, std::max(1.0f, size.y - (ImGui::GetCursorPosY() - startY)));
    if (previewTab_ == 0)
        agentPanel_.renderChangePreview(rest);
    else if (vm_.editorMode.get() == viewmodel::EditorMode::Map)
        map_.render(rest);
    else
        editor_.render(rest);
}

bool MainView::explorerVisible(const viewmodel::ExplorerItem& item)
{
    if (explorerFilter_.empty())
        return true;
    if (explorerFilterKey_ != explorerFilter_) {
        // Recomputed only when the filter changes: matching files plus the folders leading to them.
        explorerFilterKey_ = explorerFilter_;
        explorerVisible_.clear();
        std::string needle = explorerFilter_;
        std::transform(needle.begin(), needle.end(), needle.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        auto walk = [&](auto&& self, const viewmodel::ExplorerItem& node) -> bool {
            bool any = false;
            if (node.isDirectory) {
                for (const viewmodel::ExplorerItem& child : node.children)
                    any = self(self, child) || any;
            } else {
                std::string name = node.name;
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                any = name.find(needle) != std::string::npos;
            }
            if (any)
                explorerVisible_.push_back(node.path);
            return any;
        };
        walk(walk, vm_.explorer());
        std::sort(explorerVisible_.begin(), explorerVisible_.end());
    }
    return std::binary_search(explorerVisible_.begin(), explorerVisible_.end(), item.path);
}

void MainView::renderExplorerItem(const viewmodel::ExplorerItem& item)
{
    if (!explorerVisible(item))
        return;
    const model::Document* doc = vm_.activeDocument();
    const std::string activePath = doc ? doc->path : std::string();
    const bool reveal = explorerRevealSerial_ != vm_.revealSerial() && !activePath.empty();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (item.isDirectory) {
        const bool root = &item == &vm_.explorer();
        const std::string prefix = item.path.empty() ? std::string() : item.path + "/";
        const bool holdsActive = !activePath.empty() && (root || activePath.compare(0, prefix.size(), prefix) == 0);
        int openInside = 0;
        for (const auto& d : vm_.documents())
            openInside += !d->path.empty() && (root || d->path.compare(0, prefix.size(), prefix) == 0);
        if (!explorerFilter_.empty() || (reveal && holdsActive))
            ImGui::SetNextItemOpen(true);
        else if (explorerCollapse_ && !root)
            ImGui::SetNextItemOpen(false);
        const std::string label = std::string(ICON_FA_FOLDER "  ") + item.name + "##" + item.path;
        ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.text));
        const bool open = ImGui::TreeNodeEx(label.c_str(), (root ? ImGuiTreeNodeFlags_DefaultOpen : 0) |
                                                               ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        registerLastItem("folder:" + item.path, "button", item.name, nullptr);
        if (!open && openInside > 0) {
            // A collapsed folder with open documents: how many, and an accent when the active one is inside.
            const std::string badge = std::to_string(openInside);
            const ImVec2 ts = ImGui::CalcTextSize(badge.c_str());
            const float x = ImGui::GetItemRectMax().x - ts.x - 10.0f * s_;
            const float yc = (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f;
            dl->AddRectFilled(ImVec2(x - 5.0f * s_, yc - ts.y * 0.5f - 1.0f), ImVec2(x + ts.x + 5.0f * s_, yc + ts.y * 0.5f + 1.0f),
                              holdsActive ? withAlpha(palette_.accent, 70) : palette_.hover, ts.y);
            dl->AddText(ImVec2(x, yc - ts.y * 0.5f), holdsActive ? palette_.text : palette_.textDim, badge.c_str());
        }
        if (!root && vm_.git().folderChanged(item.path)) {
            // Git: something inside changed since the last commit.
            const float x = ImGui::GetItemRectMax().x - (!open && openInside > 0 ? 34.0f : 10.0f) * s_;
            dl->AddCircleFilled(ImVec2(x, (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f), 3.0f * s_,
                                withAlpha(palette_.severityWarning, 200));
        }
        if (open) {
            for (const viewmodel::ExplorerItem& child : item.children)
                renderExplorerItem(child);
            ImGui::TreePop();
        }
        return;
    }
    const int openIndex = vm_.documentIndex(item.path);
    const model::Document* openDoc = openIndex >= 0 ? vm_.documents()[size_t(openIndex)].get() : nullptr;
    const bool selected = doc && doc->path == item.path;
    // The row is a label-less tree node; icon and name are drawn here so they can end in an
    // ellipsis before the markers on the right.
    const std::string id = "##" + item.path;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::SetNextItemAllowOverlap();
    ImGui::TreeNodeEx(id.c_str(), flags);
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen())
        vm_.openFile(item.path);
    registerLastItem("file:" + item.path, "file", item.name, [this, path = item.path]() { vm_.openFile(path); });
    const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
    const bool rowHovered = ImGui::IsItemHovered();
    if (selected && reveal)
        ImGui::SetScrollHereY(0.4f);
    float right = rowMax.x - 6.0f * s_;
    const float yc = (rowMin.y + rowMax.y) * 0.5f;
    const auto findings = vm_.review().findingsForFile(item.path);
    // Git: the file's status letter at the far right, and its name tinted to match.
    const char gitStatus = vm_.git().statusOf(item.path);
    ImU32 gitColor = 0;
    if (gitStatus) {
        gitColor = gitStatus == 'A' || gitStatus == 'U' ? palette_.diffAddedText : gitStatus == 'D' ? palette_.diffRemovedText
                   : gitStatus == '!'                   ? palette_.iconOrange
                                                        : palette_.severityWarning;
        const char letter[2] = {gitStatus == '!' ? 'C' : gitStatus, 0};
        const float w = ImGui::CalcTextSize(letter).x;
        dl->AddText(ImVec2(right - w, yc - ImGui::GetTextLineHeight() * 0.5f), gitColor, letter);
        if (rowHovered)
            ImGui::SetTooltip("%s\n%s", item.path.c_str(), gitStatus == 'U' ? "Untracked: new, not in git yet" : gitStatus == 'A' ? "Added"
                                                       : gitStatus == 'D' ? "Deleted" : gitStatus == '!' ? "Merge conflict" : "Modified since the last commit");
        right -= w + 6.0f * s_;
    }
    {
        // Open documents read brighter than the rest of the tree.
        const float reserved = (openDoc ? ImGui::GetTextLineHeight() + 4.0f * s_ : 0.0f) +
                               (findings.empty() ? 0.0f : ImGui::CalcTextSize("00").x + 4.0f * s_);
        const char* icon = fileIcon(item.name);
        const float textY = yc - ImGui::GetTextLineHeight() * 0.5f;
        const float iconX = rowMin.x + ImGui::GetTreeNodeToLabelSpacing() - ImGui::GetFontSize() * 0.5f;
        dl->AddText(ImVec2(iconX, textY), selected ? palette_.accent : openDoc ? palette_.textDim : palette_.textFaint, icon);
        const float nameX = iconX + ImGui::CalcTextSize(icon).x + 7.0f * s_;
        const float limit = right - reserved - 2.0f * s_;
        std::string name = item.name;
        if (nameX + ImGui::CalcTextSize(name.c_str()).x > limit) {
            const float dots = ImGui::CalcTextSize("...").x;
            while (!name.empty() && nameX + ImGui::CalcTextSize(name.c_str()).x + dots > limit)
                name.pop_back();
            name += "...";
        }
        dl->AddText(ImVec2(nameX, textY), gitColor && !selected ? gitColor : openDoc || selected ? palette_.text : palette_.textDim,
                    name.c_str());
        if (name != item.name && rowHovered)
            ImGui::SetTooltip("%s", item.path.c_str());
    }
    if (openDoc) {
        // On the right: a close button on hover, else the unsaved dot.
        const float size = ImGui::GetTextLineHeight();
        const ImVec2 closeMin(right - size, yc - size * 0.5f);
        ImGui::SetCursorScreenPos(closeMin);
        ImGui::PushID(item.path.c_str());
        const bool closeClicked = ImGui::InvisibleButton("##closeOpen", ImVec2(size, size));
        ImGui::PopID();
        const bool closeHovered = ImGui::IsItemHovered();
        if (closeHovered)
            dl->AddRectFilled(closeMin, ImVec2(closeMin.x + size, closeMin.y + size), palette_.hover, 3.0f * s_);
        const ImVec2 c(closeMin.x + size * 0.5f, yc);
        if (rowHovered || closeHovered) {
            const float r = size * 0.22f;
            const ImU32 color = closeHovered ? palette_.text : palette_.textFaint;
            dl->AddLine(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), color, 1.5f * s_);
            dl->AddLine(ImVec2(c.x - r, c.y + r), ImVec2(c.x + r, c.y - r), color, 1.5f * s_);
            ImGui::SetItemTooltip("Close");
        } else if (openDoc->modified) {
            dl->AddCircleFilled(c, 3.5f * s_, palette_.accent);
        }
        if (closeClicked)
            vm_.requestCloseDocument(openIndex);
        right -= size + 4.0f * s_;
    }
    // Finding count badge.
    if (!findings.empty()) {
        const model::Severity worst = (*std::max_element(findings.begin(), findings.end(), [](auto a, auto b) {
                                          return a->severity < b->severity;
                                      }))->severity;
        const ImU32 c = worst == model::Severity::Error     ? palette_.severityError
                        : worst == model::Severity::Warning ? palette_.severityWarning
                                                            : palette_.severityInfo;
        const std::string badge = std::to_string(findings.size());
        const ImVec2 ts = ImGui::CalcTextSize(badge.c_str());
        dl->AddText(ImVec2(right - ts.x, yc - ts.y * 0.5f), c, badge.c_str());
    }
}

void MainView::renderExplorer()
{
    ImGui::PushID("explorer");
    // Two views of the workspace: the folder tree, and the documents that are open.
    const int count = int(vm_.documents().size());
    const int request = explorerTabRequest_;
    explorerTabRequest_ = -1;
    // Three tabs share a narrow panel: tighter padding than the default.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f * s_, ImGui::GetStyle().FramePadding.y));
    if (ImGui::BeginTabBar("##explorerTabs")) {
        if (ImGui::BeginTabItem(ICON_FA_FOLDER_TREE " Files###files", nullptr, request == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            explorerTab_ = 0;
            ImGui::EndTabItem();
        }
        registerLastItem("explorer-tab:Files", "tab", "Files", [this]() { explorerTabRequest_ = 0; });
        const std::string openLabel = std::string(ICON_FA_LAYER_GROUP " Open ") + std::to_string(count) + "###open";
        if (ImGui::BeginTabItem(openLabel.c_str(), nullptr, request == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
            explorerTab_ = 1;
            ImGui::EndTabItem();
        }
        registerLastItem("explorer-tab:Open", "tab", "Open documents", [this]() { explorerTabRequest_ = 1; });
        const int changes = gitView_.changeCount();
        const std::string gitLabel = std::string(ICON_FA_CODE_BRANCH " Git") + (changes ? " " + std::to_string(changes) : "") + "###git";
        if (ImGui::BeginTabItem(gitLabel.c_str(), nullptr, request == 2 ? ImGuiTabItemFlags_SetSelected : 0)) {
            explorerTab_ = 2;
            ImGui::EndTabItem();
        }
        registerLastItem("explorer-tab:Git", "tab", "Git", [this]() { explorerTabRequest_ = 2; });
        ImGui::EndTabBar();
    }
    ImGui::PopStyleVar();
    if (explorerTab_ == 2) {
        gitView_.renderChanges();
        ImGui::PopID();
        return;
    }

    // Toolbar: filter (both views); reveal and collapse for the tree.
    const bool files = explorerTab_ == 0;
    const float button = ImGui::GetFrameHeight();
    const float spacing = 2.0f * s_;
    ImGui::SetNextItemWidth(std::max(40.0f * s_, ImGui::GetContentRegionAvail().x - (files ? 2.0f * (button + spacing) : 0.0f)));
    ImGui::InputTextWithHint("##filter", files ? ICON_FA_MAGNIFYING_GLASS "  Filter files" : ICON_FA_MAGNIFYING_GLASS "  Filter open documents",
                             &explorerFilter_);
    registerLastItem("explorer-filter", "textbox", "Filter", nullptr);
    if (!explorerFilter_.empty() && ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Escape))
        explorerFilter_.clear();
    if (files) {
        ImGui::SameLine(0.0f, spacing);
        if (flatButton(ICON_FA_CROSSHAIRS "##reveal", "Reveal the active file", vm_.activeDocument() != nullptr))
            explorerRevealSerial_ = -1;
        registerLastItem("explorer-reveal", "button", "Reveal the active file", [this]() { explorerRevealSerial_ = -1; });
        ImGui::SameLine(0.0f, spacing);
        if (flatButton(ICON_FA_FOLDER_MINUS "##collapse", "Collapse all folders"))
            explorerCollapse_ = true;
        registerLastItem("explorer-collapse", "button", "Collapse all folders", [this]() { explorerCollapse_ = true; });
    }

    ImGui::BeginChild("##explorerBody", ImVec2(0.0f, 0.0f));
    if (files) {
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 14.0f * s_);
        renderExplorerItem(vm_.explorer());
        ImGui::PopStyleVar();
        if (!explorerFilter_.empty() && !explorerVisible(vm_.explorer()))
            ImGui::TextDisabled("No files match \"%s\"", explorerFilter_.c_str());
        explorerRevealSerial_ = vm_.revealSerial();
        explorerCollapse_ = false;
    } else {
        renderOpenDocuments();
    }
    ImGui::EndChild();
    ImGui::PopID();
}

void MainView::renderOpenDocuments()
{
    // The open documents in tab order (drag to reorder), the same rows as the tabs' overflow list.
    if (documentList_.render("open-document", explorerFilter_, {}) >= 0)
        previewTabRequest_ = 1;
    if (!vm_.documents().empty()) {
        ImGui::Spacing();
        if (ImGui::SmallButton(ICON_FA_CIRCLE_CHECK "  Close Saved"))
            vm_.closeSaved();
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_BROOM "  Close All"))
            vm_.closeAll();
    }
}

// ---- Status bar ---------------------------------------------------------------------------------

void MainView::renderStatusBar()
{
    ImGui::PushID("statusBar");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = kStatusBarHeight * s_;
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), palette_.statusBarBg);
    dl->AddLine(origin, ImVec2(origin.x + width, origin.y), palette_.border);
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.87f);
    const float textY = origin.y + (h - ImGui::GetTextLineHeight()) * 0.5f;

    // Workspace badge (click to switch).
    const bool agent = vm_.workspace.get() == WorkspaceMode::Agent;
    const char* badge = agent ? ICON_FA_ROBOT " AGENT" : ICON_FA_CODE " CODE";
    const ImU32 modeColor = agent ? palette_.modeAgent : palette_.modeCode;
    const ImVec2 badgeSize = ImGui::CalcTextSize(badge);
    const ImVec2 bMin(origin.x + 6.0f * s_, origin.y + 3.0f * s_);
    const ImVec2 bMax(bMin.x + badgeSize.x + 14.0f * s_, origin.y + h - 3.0f * s_);
    ImGui::SetCursorScreenPos(bMin);
    if (ImGui::InvisibleButton("##modeBadge", ImVec2(bMax.x - bMin.x, bMax.y - bMin.y)))
        vm_.execute(CommandId::ToggleWorkspaceMode);
    ImGui::SetItemTooltip("Switch workspace (%s)", commandInfo(CommandId::ToggleWorkspaceMode).shortcut);
    dl->AddRectFilled(bMin, bMax, withAlpha(modeColor, ImGui::IsItemHovered() ? 70 : 40), 4.0f * s_);
    dl->AddText(ImVec2(bMin.x + 7.0f * s_, textY), modeColor, badge);

    // Git branch chip: branch, ahead/behind, a dot when there are changes; click for the branch picker.
    float messageX = bMax.x;
    if (gitView_.repository()) {
        std::string branchText = std::string(ICON_FA_CODE_BRANCH " ") + gitView_.branch();
        if (gitView_.merging())
            branchText += "  MERGING";
        if (gitView_.ahead() || gitView_.behind())
            branchText += "  " ICON_FA_ARROW_UP + std::to_string(gitView_.ahead()) + " " ICON_FA_ARROW_DOWN + std::to_string(gitView_.behind());
        const float dotW = gitView_.changeCount() ? 14.0f * s_ : 0.0f;
        const ImVec2 gMin(bMax.x + 6.0f * s_, origin.y + 1.0f);
        const ImVec2 gMax(gMin.x + ImGui::CalcTextSize(branchText.c_str()).x + dotW + 16.0f * s_, origin.y + h);
        ImGui::SetCursorScreenPos(gMin);
        if (ImGui::InvisibleButton("##branchChip", ImVec2(gMax.x - gMin.x, gMax.y - gMin.y)))
            gitView_.openBranchPicker(ImVec2(gMin.x, origin.y), true);
        ImGui::SetItemTooltip("Branches: switch, create, merge");
        registerLastItem("status:branch", "button", "Branches", [this]() { vm_.execute(CommandId::GitBranches); });
        if (ImGui::IsItemHovered())
            dl->AddRectFilled(gMin, gMax, palette_.hover);
        dl->AddText(ImVec2(gMin.x + 8.0f * s_, textY), gitView_.merging() ? palette_.severityWarning : palette_.text, branchText.c_str());
        if (dotW > 0.0f) // uncommitted changes, in the modified colour
            dl->AddCircleFilled(ImVec2(gMax.x - 8.0f * s_ - dotW * 0.5f, origin.y + h * 0.5f), 3.5f * s_, palette_.severityWarning);
        dl->AddLine(ImVec2(gMax.x, origin.y + 5.0f * s_), ImVec2(gMax.x, origin.y + h - 5.0f * s_), palette_.border);
        messageX = gMax.x;
    }

    // Demo data (the guide, or no folder open): say so plainly.
    if (gitView_.demo()) {
        const bool learning = gitView_.guideRunning();
        const char* demoText = learning ? ICON_FA_GRADUATION_CAP " LEARNING GIT · DEMO DATA" : ICON_FA_FLASK " DEMO DATA";
        const ImU32 demoColor = learning ? palette_.iconPurple : palette_.iconOrange;
        const ImVec2 dMin(messageX + 6.0f * s_, origin.y + 3.0f * s_);
        const ImVec2 dMax(dMin.x + ImGui::CalcTextSize(demoText).x + 14.0f * s_, origin.y + h - 3.0f * s_);
        ImGui::SetCursorScreenPos(dMin);
        if (ImGui::InvisibleButton("##demoBadge", ImVec2(dMax.x - dMin.x, dMax.y - dMin.y)))
            vm_.execute(CommandId::GitGuide);
        ImGui::SetItemTooltip(learning ? "Learn Git is running on made-up files and branches; your real project is not touched."
                                       : "No folder is open, so the Git views show made-up data. Click to learn git with it.");
        registerLastItem("status:demo", "button", "Demo data", [this]() { vm_.execute(CommandId::GitGuide); });
        dl->AddRectFilled(dMin, dMax, withAlpha(demoColor, ImGui::IsItemHovered() ? 70 : 40), 4.0f * s_);
        dl->AddText(ImVec2(dMin.x + 7.0f * s_, textY), demoColor, demoText);
        messageX = dMax.x;
    }

    // Right-hand segments, Notepad++ style.
    struct Segment {
        std::string text;
        const char* popup;
    };
    std::vector<Segment> segments;
    if (vm_.agent().busy.get())
        segments.push_back({ICON_FA_CIRCLE_NOTCH " Agent working", nullptr});
    if (vm_.recording.get())
        segments.push_back({ICON_FA_CIRCLE_DOT " REC", nullptr});
    const model::Document* doc = vm_.activeDocument();
    if (doc) {
        const int eolLength = doc->eol == model::EolMode::CrLf ? 2 : 1;
        const std::string& line = doc->buffer.line(doc->caret.line);
        const int column = model::codePointCount(std::string_view(line).substr(0, size_t(doc->caret.column))) + 1;
        const std::string selected = vm_.selectedText();
        const int selLines = doc->hasSelection() ? doc->selectionEnd().line - doc->selectionStart().line + 1 : 0;
        segments.push_back({model::languageName(doc->language), "##language"});
        segments.push_back({"length : " + std::to_string(doc->buffer.characterCount(eolLength)) +
                                "    lines : " + std::to_string(doc->buffer.lineCount()),
                            nullptr});
        segments.push_back({"Ln : " + std::to_string(doc->caret.line + 1) + "    Col : " + std::to_string(column) +
                                "    Sel : " + std::to_string(model::codePointCount(selected)) + " | " + std::to_string(selLines),
                            nullptr});
        segments.push_back({std::string(doc->indent.tabs ? "Tab Size: " : "Spaces: ") + std::to_string(doc->indent.width), "##indent"});
        segments.push_back({model::eolName(doc->eol), "##eol"});
        segments.push_back({model::encodingName(doc->encoding), "##encoding"});
    }
    segments.push_back({std::to_string(int(std::lround(vm_.zoom.get() * 100.0f))) + "%", nullptr});
    segments.push_back({"INS", nullptr});

    float x = origin.x + width - 8.0f * s_;
    const float pad = 10.0f * s_;
    const char* openPopup = nullptr;
    ImVec2 popupPos;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        const float w = ImGui::CalcTextSize(it->text.c_str()).x;
        x -= w + pad * 2.0f;
        const ImVec2 min(x, origin.y + 1.0f), max(x + w + pad * 2.0f, origin.y + h);
        bool hovered = false;
        if (it->popup) {
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(it->popup);
            if (ImGui::InvisibleButton("##segment", ImVec2(max.x - min.x, max.y - min.y))) {
                openPopup = it->popup;
                popupPos = ImVec2(min.x, origin.y);
            }
            hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hovered)
                dl->AddRectFilled(min, max, palette_.hover);
        }
        const bool agentSegment = it->text.rfind(ICON_FA_CIRCLE_NOTCH, 0) == 0;
        dl->AddText(ImVec2(x + pad, textY), agentSegment ? palette_.modeAgent : hovered ? palette_.text : palette_.textDim,
                    it->text.c_str());
        dl->AddLine(ImVec2(x, origin.y + 5.0f * s_), ImVec2(x, origin.y + h - 5.0f * s_), palette_.border);
    }

    // Status message between the badge and the segments.
    dl->PushClipRect(ImVec2(messageX + 8.0f * s_, origin.y), ImVec2(x - 4.0f * s_, origin.y + h), true);
    dl->AddText(ImVec2(messageX + 10.0f * s_, textY), palette_.textDim, vm_.statusText.get().c_str());
    dl->PopClipRect();
    ImGui::PopFont();

    if (openPopup && std::strcmp(openPopup, "##language") == 0) {
        vm_.execute(CommandId::ChooseLanguage); // the one language picker
        openPopup = nullptr;
    }
    if (openPopup)
        ImGui::OpenPopup(openPopup);
    auto menu = [&](const char* id, std::initializer_list<CommandId> ids) {
        ImGui::SetNextWindowPos(popupPos, ImGuiCond_Appearing, ImVec2(0.0f, 1.0f));
        if (ImGui::BeginPopup(id)) {
            for (CommandId c : ids)
                if (ImGui::MenuItem(commandInfo(c).tooltip, nullptr, vm_.isChecked(c)))
                    vm_.execute(c);
            ImGui::EndPopup();
        }
    };
    menu("##eol", {CommandId::EolCrLf, CommandId::EolLf, CommandId::EolCr});
    menu("##indent", {CommandId::IndentWithSpaces, CommandId::IndentWithTabs, CommandId::IndentWidth2, CommandId::IndentWidth4,
                      CommandId::IndentWidth8, CommandId::DetectIndentation, CommandId::ConvertIndentation,
                      CommandId::ToggleSmartIndent});
    menu("##encoding", {CommandId::EncodingUtf8, CommandId::EncodingUtf8Bom, CommandId::EncodingUtf16Le,
                        CommandId::EncodingUtf16Be, CommandId::EncodingAnsi});
    ImGui::SetCursorScreenPos(origin); // see renderTopBar(): end the region with an item
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopID();
}

// ---- Dialogs ------------------------------------------------------------------------------------

void MainView::renderGoToLinePopup()
{
    if (openGoToLine_) {
        ImGui::OpenPopup("Go To Line");
        openGoToLine_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Go To Line", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const model::Document* doc = vm_.activeDocument();
    const int lines = doc ? doc->buffer.lineCount() : 1;
    ImGui::Text("Line number (1 - %d):", lines);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(200.0f * s_);
    const bool enter = ImGui::InputInt("##line", &goToLine_, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue);
    goToLine_ = std::clamp(goToLine_, 1, lines);
    if (ImGui::Button("Go", ImVec2(96.0f * s_, 0.0f)) || enter) {
        vm_.goToLine(goToLine_ - 1);
        editor_.focus();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MainView::renderCommentPopup()
{
    if (openComment_) {
        ImGui::OpenPopup("Add Review Comment");
        openComment_ = false;
        commentDraft_.clear();
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Add Review Comment", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    const model::Document* doc = vm_.activeDocument();
    const std::string file = doc ? (doc->path.empty() ? doc->title : doc->path) : std::string();
    const int line = doc ? doc->caret.line : 0;
    ImGui::Text("%s  %s:%d", ICON_FA_COMMENT_DOTS, file.c_str(), line + 1);
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::InputTextMultiline("##comment", &commentDraft_, ImVec2(420.0f * s_, ImGui::GetTextLineHeight() * 5.0f));
    ImGui::BeginDisabled(commentDraft_.empty() || !doc);
    if (ImGui::Button("Add comment", ImVec2(120.0f * s_, 0.0f))) {
        vm_.review().addComment(file, line, commentDraft_);
        vm_.panelTab.set(viewmodel::PanelTab::Comments);
        vm_.showPanel.set(true);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MainView::renderPreferencesPopup()
{
    if (openPreferences_) {
        ImGui::OpenPopup("Preferences");
        openPreferences_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::BeginPopupModal("Preferences", &open, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    auto toggle = [](const char* label, viewmodel::Property<bool>& property) {
        bool value = property.get();
        if (ImGui::Checkbox(label, &value))
            property.set(value);
    };
    ImGui::TextDisabled("APPEARANCE");
    toggle("Dark theme", vm_.darkTheme);
    float zoom = vm_.zoom.get() * 100.0f;
    ImGui::SetNextItemWidth(220.0f * s_);
    if (ImGui::SliderFloat("Editor zoom", &zoom, 50.0f, 300.0f, "%.0f%%"))
        vm_.zoom.set(std::round(zoom / 10.0f) / 10.0f);
    ImGui::Spacing();
    ImGui::TextDisabled("EDITOR");
    toggle("Line numbers", vm_.showLineNumbers);
    toggle("Indent guides", vm_.showIndentGuides);
    toggle("Show whitespace", vm_.showWhitespace);
    toggle("Word wrap", vm_.wordWrap);
    toggle("Smart indent", vm_.smartIndent);
    {
        // Default indentation for new and not-yet-indented files (indented files keep theirs).
        const auto& preferred = vm_.preferredIndent();
        const char* const labels[] = {"Language default", "2 spaces", "4 spaces", "Tabs"};
        int current = !preferred ? 0 : preferred->tabs ? 3 : preferred->width == 2 ? 1 : 2;
        ImGui::SetNextItemWidth(160.0f * s_);
        if (ImGui::Combo("Default indentation", &current, labels, 4)) {
            switch (current) {
            case 1: vm_.setPreferredIndent(model::IndentStyle{false, 2}); break;
            case 2: vm_.setPreferredIndent(model::IndentStyle{false, 4}); break;
            case 3: vm_.setPreferredIndent(model::IndentStyle{true, 4}); break;
            default: vm_.setPreferredIndent(std::nullopt); break;
            }
        }
        ImGui::SetItemTooltip("For new files and files with nothing indented yet; indented files keep their own style");
    }
    ImGui::Spacing();
    ImGui::TextDisabled("PERFORMANCE");
    {
        // The app redraws only when something changes; this caps how often it does while it does.
        const int options[] = {15, 30, 60, 0};
        const char* const labels[] = {"15 fps (saves the most power)", "30 fps", "60 fps", "Uncapped (display rate)"};
        int current = 1;
        for (int i = 0; i < 4; ++i)
            if (options[i] == vm_.maxFramesPerSecond.get())
                current = i;
        ImGui::SetNextItemWidth(220.0f * s_);
        if (ImGui::Combo("Max redraw rate", &current, labels, 4))
            vm_.maxFramesPerSecond.set(options[current]);
        ImGui::SetItemTooltip("While something moves (typing, scrolling, a terminal program). When nothing changes the app does not redraw at all.");
    }
    toggle("Blinking caret (off: an idle app does not redraw at all)", vm_.blinkCaret);
    ImGui::Spacing();
    ImGui::TextDisabled("WORKSPACE");
    int mode = vm_.workspace.get() == WorkspaceMode::Agent ? 0 : 1;
    if (ImGui::RadioButton("Agent", mode == 0))
        vm_.workspace.set(WorkspaceMode::Agent);
    ImGui::SameLine();
    if (ImGui::RadioButton("Code", mode == 1))
        vm_.workspace.set(WorkspaceMode::Code);
    if (ImGui::Button(ICON_FA_PALETTE "  Syntax colours..."))
        openSyntaxColors_ = true;
    ImGui::Spacing();
    ImGui::TextDisabled(vm_.settingsPath().empty() ? "Settings last for this session." : "Settings are saved automatically.");
    if (ImGui::Button("Close", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

namespace {

// A few lines of every language for the colour preview.
const char* colorSample(model::Language language)
{
    using L = model::Language;
    switch (language) {
    case L::Cpp: return "#include <vector>\n// Sum of the values.\nint sum(const std::vector<int>& v)\n{\n    int total = 0; /* running */\n    for (int x : v) total += x * 2;\n    return total; // \"done\"\n}";
    case L::CSharp: return "using System;\n#region Demo\npublic class Greeter {\n    // Says hello.\n    public string Hello(string name) => $\"Hi {name}\";\n    private int count = 42;\n}\n#endregion";
    case L::Java: return "@Override\npublic String toString() {\n    // Debug text.\n    int n = 3;\n    return \"Point(\" + x + \")\";\n}";
    case L::JavaScript: return "// Fetch and render.\nexport async function load(url) {\n    const res = await fetch(url);\n    return `items: ${res.status}`;\n}";
    case L::TypeScript: return "interface User { name: string; age: number }\n@Component\nexport class View {\n    // Greets.\n    greet(u: User): string { return `hi ${u.name}`; }\n}";
    case L::Python: return "@dataclass\nclass Point:\n    \"\"\"A 2D point.\"\"\"\n    x: int = 0\n\n    def scale(self, k):  # in place\n        self.x *= k\n        return self";
    case L::Rust: return "#[derive(Debug)]\nstruct Point { x: i32 }\n// Moves right.\nfn step(p: &mut Point) -> i32 {\n    p.x += 1;\n    println!(\"{}\", p.x);\n    p.x\n}";
    case L::Go: return "package main\n\nimport \"fmt\"\n\n// main prints.\nfunc main() {\n    n := 42\n    fmt.Println(\"n =\", n)\n}";
    case L::Lua: return "-- Counts down.\nlocal function count(n)\n    for i = n, 1, -1 do\n        print(\"t-\" .. i)\n    end\n    return nil\nend";
    case L::Shell: return "#!/bin/bash\n# Build all.\nfor f in src/*.cpp; do\n    if [ -f \"$f\" ]; then\n        echo \"building ${f}\"\n    fi\ndone\nexit $?";
    case L::PowerShell: return "<# Lists files #>\nfunction Show-Files($Path) {\n    # newest first\n    Get-ChildItem -Path $Path | Sort-Object LastWriteTime\n    if ($null -eq $Path) { return 0 }\n}";
    case L::Html: return "<!DOCTYPE html>\n<html lang=\"en\">\n  <!-- page -->\n  <body class=\"main\">\n    <p>Fish &amp; chips</p>\n  </body>\n</html>";
    case L::Xml: return "<?xml version=\"1.0\"?>\n<project name=\"demo\">\n  <!-- settings -->\n  <item id=\"1\" enabled=\"true\"/>\n</project>";
    case L::Css: return "/* Layout */\n@media (max-width: 600px) {\n  .card:hover {\n    color: #ff8800;\n    margin: 4px 0;\n  }\n}";
    case L::Json: return "{\n  \"name\": \"easynotepad\",\n  \"version\": 2,\n  \"tags\": [\"editor\", \"agent\"],\n  \"private\": true,\n  \"license\": null\n}";
    case L::Yaml: return "# CI\nname: build\non: [push]\njobs:\n  test:\n    runs-on: ubuntu-latest\n    steps:\n      - run: make test\n        timeout: 10";
    case L::Toml: return "# Package\n[package]\nname = \"demo\"\nversion = \"0.1.0\"\n\n[dependencies]\nserde = { version = \"1\", features = [\"derive\"] }\nfast = true";
    case L::Csv: return "id,name,city,score,active,joined\n1,Ana,Lisbon,92.5,true,2024-01-03\n2,Bo,\"Oslo, NO\",88,false,2023-11-20\n3,Chen,Taipei,95,true,2022-07-14";
    case L::Sql: return "-- Top customers\nSELECT name, SUM(total) AS spent\nFROM orders\nWHERE created_at > '2024-01-01'\nGROUP BY name\nORDER BY spent DESC\nLIMIT 10;";
    case L::CMake: return "# Library\ncmake_minimum_required(VERSION 3.20)\nadd_library(core STATIC src/a.cpp)\nif(WIN32)\n    target_link_libraries(core PRIVATE ws2_32)\nendif()\nset(CMAKE_CXX_STANDARD 17)\nmessage(STATUS \"dir: ${CMAKE_SOURCE_DIR}\")";
    case L::Markdown: return "# Title\n\nSome **bold** and *em* text with `code`.\n\n- see [the docs](https://example.com)\n> a quote\n\n```cpp\nint x = 1;\n```";
    default: return "Plain text has no syntax colours,\nonly the text colour.";
    }
}

} // namespace

void MainView::renderSyntaxColorsPopup()
{
    if (openSyntaxColors_) {
        ImGui::OpenPopup("Syntax Colours");
        openSyntaxColors_ = false;
        if (const model::Document* doc = vm_.activeDocument())
            for (size_t i = 0; i < model::languages().size(); ++i)
                if (model::languages()[i].id == doc->language && doc->language != model::Language::PlainText)
                    syntaxColorsLanguage_ = int(i);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(820.0f * s_, 560.0f * s_), ImGuiCond_Appearing);
    bool open = true;
    if (!ImGui::BeginPopupModal("Syntax Colours", &open))
        return;
    const bool dark = vm_.darkTheme.get();
    const auto& all = model::languages();
    const bool allLanguages = syntaxColorsLanguage_ < 0 || syntaxColorsLanguage_ >= int(all.size());
    const model::LanguageSpec* spec = allLanguages ? nullptr : &all[size_t(syntaxColorsLanguage_)];
    const std::string key = spec ? spec->key : viewmodel::NotepadViewModel::kAllLanguages;
    const float footerH = ImGui::GetFrameHeightWithSpacing() + 8.0f * s_;

    // Languages.
    ImGui::BeginChild("##languages", ImVec2(190.0f * s_, -footerH), ImGuiChildFlags_Borders);
    auto languageRow = [&](int index, const char* label, const std::string& languageKey) {
        const bool custom = vm_.hasSyntaxColors(dark, languageKey);
        // No "##" suffix: a name like "C#" would turn it into "###".
        const std::string text = std::string(label) + (custom ? "  " ICON_FA_CIRCLE : "");
        ImGui::PushID(index);
        if (ImGui::Selectable(text.c_str(), syntaxColorsLanguage_ == index || (index < 0 && allLanguages)))
            syntaxColorsLanguage_ = index;
        ImGui::PopID();
        if (custom)
            ImGui::SetItemTooltip("Has custom colours");
    };
    languageRow(-1, ICON_FA_LAYER_GROUP "  All languages", viewmodel::NotepadViewModel::kAllLanguages);
    ImGui::Separator();
    for (int i = 0; i < int(all.size()); ++i)
        languageRow(i, all[size_t(i)].name, all[size_t(i)].key);
    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginGroup();
    ImGui::Text("%s", spec ? spec->name : "All languages");
    ImGui::SameLine();
    ImGui::TextDisabled("%s theme  |  %s", dark ? "dark" : "light",
                        spec ? "overrides the all-languages colours" : "applies wherever a language has no colour of its own");

    // Token colours.
    std::vector<model::Token> tokens;
    if (spec) {
        tokens = model::languageTokens(spec->id);
    } else {
        for (int t = 0; t < int(model::Token::Count); ++t)
            tokens.push_back(model::Token(t));
    }
    const SyntaxColors base = resolveSyntaxColors(palette_, vm_, model::Language::PlainText); // theme + all-languages
    const SyntaxColors effective = spec ? resolveSyntaxColors(palette_, vm_, spec->id) : base;
    const float previewH = 190.0f * s_;
    ImGui::BeginChild("##tokens", ImVec2(0.0f, -footerH - previewH - 8.0f * s_), ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("##tokenTable", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed, 36.0f * s_);
        ImGui::TableSetupColumn("Token", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthFixed, 200.0f * s_);
        for (model::Token token : tokens) {
            ImGui::TableNextRow();
            ImGui::PushID(int(token));
            const size_t t = size_t(token);
            const std::optional<uint32_t> own = vm_.syntaxColor(dark, key, token);
            ImVec4 colour = ImGui::ColorConvertU32ToFloat4(effective[t]);
            ImGui::TableSetColumnIndex(0);
            if (ImGui::ColorEdit3("##colour", &colour.x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel))
                vm_.setSyntaxColor(dark, key, token, u32ToRgb(ImGui::ColorConvertFloat4ToU32(colour)));
            registerLastItem("syntax-colour:" + key + ":" + model::tokenKey(token), "button", model::tokenName(token), nullptr);
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(colour, "%s", model::tokenName(token));
            ImGui::TableSetColumnIndex(2);
            if (own) {
                ImGui::TextUnformatted("custom");
                ImGui::SameLine();
                if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT " reset"))
                    vm_.setSyntaxColor(dark, key, token, std::nullopt);
            } else {
                ImGui::TextDisabled("%s", spec && base[t] != palette_.syntax[t] ? "all languages" : "theme default");
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    // Live preview with the colours above.
    const model::Language previewLanguage = spec ? spec->id : model::Language::Cpp;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, palette_.editorBg);
    ImGui::BeginChild("##preview", ImVec2(0.0f, previewH), ImGuiChildFlags_Borders);
    ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * 0.95f);
    {
        const SyntaxColors colors = resolveSyntaxColors(palette_, vm_, previewLanguage);
        const model::LanguageSpec& previewSpec = model::languageSpec(previewLanguage);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const std::string sample = colorSample(previewLanguage);
        int state = 0;
        std::vector<model::Token> toks;
        ImVec2 pos = ImVec2(ImGui::GetCursorScreenPos().x + 6.0f * s_, ImGui::GetCursorScreenPos().y + 4.0f * s_);
        const float lineH = ImGui::GetTextLineHeight() * 1.3f;
        size_t start = 0;
        int lines = 0;
        while (start <= sample.size()) {
            const size_t nl = sample.find('\n', start);
            const std::string line = sample.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
            model::highlightLine(line, previewSpec, state, toks);
            float x = pos.x;
            for (size_t b = 0; b < line.size();) {
                size_t e = b + 1;
                while (e < line.size() && toks[e] == toks[b])
                    ++e;
                const std::string run = line.substr(b, e - b);
                dl->AddText(ImVec2(x, pos.y), colors[size_t(toks[b])], run.c_str());
                x += ImGui::CalcTextSize(run.c_str()).x;
                b = e;
            }
            pos.y += lineH;
            ++lines;
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
        ImGui::Dummy(ImVec2(1.0f, float(lines) * lineH + 8.0f * s_));
    }
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::EndGroup();

    // Footer.
    ImGui::Spacing();
    if (ImGui::Button(spec ? ICON_FA_ROTATE_LEFT "  Reset this language" : ICON_FA_ROTATE_LEFT "  Reset all-languages colours"))
        vm_.resetSyntaxColors(dark, key);
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TRASH_CAN "  Reset everything"))
        vm_.resetSyntaxColors(dark, "");
    ImGui::SameLine();
    ImGui::TextDisabled(vm_.settingsPath().empty() ? "(not saved: no settings file)" : "Saved automatically.");
    ImGui::SameLine();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - 96.0f * s_));
    if (ImGui::Button("Close", ImVec2(96.0f * s_, 0.0f)) || (ImGui::IsKeyPressed(ImGuiKey_Escape) && !ImGui::IsAnyItemActive()))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MainView::renderAboutPopup()
{
    if (openAbout_) {
        ImGui::OpenPopup("About EasyNotepad");
        openAbout_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    if (!ImGui::BeginPopupModal("About EasyNotepad", &open, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    ImGui::TextColored(col(palette_.accent), ICON_FA_PEN_TO_SQUARE);
    ImGui::SameLine();
    ImGui::Text("EasyNotepad %s", ENP_VERSION);
    ImGui::TextDisabled("Native text editor with Agent and Code workspaces.");
    ImGui::Separator();
    ImGui::Text("Platform: %s %s", os::osName(), os::archName());
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    if (!rendererInfo_.empty())
        ImGui::TextWrapped("Renderer: %s", rendererInfo_.c_str());
    ImGui::Spacing();
    ImGui::TextDisabled("UI preview build: running on dummy data, no services connected.");
    if (ImGui::Button("OK", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace enp::view
