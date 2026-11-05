#include "enp/view/MainView.h"
#include "enp/Version.h" // generated from the VERSION file (cmake/Version.cmake)
#include "enp/os/Memory.h"
#include "enp/view/Fit.h"

#include "enp/view/PanelFrame.h"

#include "enp/os/Os.h"
#include "enp/os/Platform.h"
#include "enp/view/TouchScroll.h"
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
      engineLab_(vm, fonts, palette_, uiScale),
      explorerWidth_(230.0f * uiScale), agentSideWidth_(360.0f * uiScale), panelHeight_(240.0f * uiScale),
      planWidth_(270.0f * uiScale), conversationWidth_(430.0f * uiScale)
{
    editor_.setEmoji(&emoji_);
    agentPanel_.setEmoji(&emoji_);
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
                vm_.paste(text); // one line per caret when the counts match
            break;
        }
    }));
    subscriptions_.push_back(vm_.localModelRequested.subscribe([this]() { screen_ = Screen::Agent; }));
    subscriptions_.push_back(vm_.focusPromptRequested.subscribe([this]() {
        agentPanel_.focusPrompt();
        screen_ = Screen::Agent;
    }));
    // A phone shows one screen at a time: commands that show a part of the UI switch to it.
    subscriptions_.push_back(vm_.showPanel.subscribe([this](const bool& shown) {
        if (shown)
            screen_ = Screen::Tools;
    }));
    subscriptions_.push_back(vm_.panelTab.subscribe([this](const viewmodel::PanelTab&) { screen_ = Screen::Tools; }));
    subscriptions_.push_back(vm_.showExplorer.subscribe([this](const bool& shown) {
        if (shown)
            screen_ = Screen::Files;
    }));
    subscriptions_.push_back(vm_.showAgentPanel.subscribe([this](const bool& shown) {
        if (shown)
            screen_ = Screen::Agent;
    }));
    subscriptions_.push_back(vm_.goToLineRequested.subscribe([this]() {
        const model::Document* doc = vm_.activeDocument();
        goToLine_ = doc ? doc->caret.line + 1 : 1;
        openGoToLine_ = true;
    }));
    subscriptions_.push_back(vm_.addCommentRequested.subscribe([this]() { openComment_ = true; }));
    subscriptions_.push_back(vm_.preferencesRequested.subscribe([this]() { openPreferences_ = true; }));
    subscriptions_.push_back(vm_.syntaxColorsRequested.subscribe([this]() { openSyntaxColors_ = true; }));
    subscriptions_.push_back(vm_.intellisenseSettingsRequested.subscribe([this]() { openIntellisenseSettings_ = true; }));
    subscriptions_.push_back(vm_.languagePickerRequested.subscribe([this]() { openLanguagePicker_ = true; }));
    subscriptions_.push_back(vm_.recentPickerRequested.subscribe([this]() {
        if (ImGui::IsPopupOpen("##recentPicker"))
            recentStep_ = +1; // Ctrl+R again: the next entry, as in VS Code
        else
            openRecentPicker_ = true;
    }));
    // Ctrl+E: into the terminal; from the terminal, back to the editor.
    subscriptions_.push_back(vm_.terminalFocusRequested.subscribe([this]() {
        if (terminalView_.focused()) {
            vm_.editorRequested.emit();
            focusedEditor().focus();
        } else {
            vm_.showPanelTab(viewmodel::PanelTab::Terminal); // starts a shell if there is none
        }
    }));
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
            load("markdownColumn", editor_.markdownColumn());
            if (const model::Json* v = j.find("splitRatio"); v && v->isNumber())
                splitRatio_ = std::clamp(float(v->asNumber()), 0.1f, 0.9f);
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
                .set("markdownColumn", editor_.markdownColumn() / s_) // 0: the standard width
                .set("splitRatio", double(splitRatio_))
                .set("blameColumns", double(editor_.blameColumns()));
        });
    subscriptions_.push_back(vm_.aboutRequested.subscribe([this]() { openAbout_ = true; }));
    subscriptions_.push_back(vm_.shortcutsRequested.subscribe([this]() { openShortcuts_ = true; }));
    subscriptions_.push_back(vm_.engineLabRequested.subscribe([this]() { engineLab_.open(); }));
    subscriptions_.push_back(vm_.quitRequested.subscribe([this]() { quitRequested_ = true; }));
    subscriptions_.push_back(vm_.editorRequested.subscribe([this]() { previewTabRequest_ = 1; }));
    subscriptions_.push_back(vm_.workspace.subscribe([this](const WorkspaceMode& mode) {
        vm_.statusText.set(mode == WorkspaceMode::Agent ? "Agent workspace: describe the goal, review the changes"
                                                        : "Code workspace: manual editing with the agent on the side");
        focusedEditor().focus();
    }));
    subscriptions_.push_back(vm_.otherColumnRequested.subscribe([this]() { focusColumn(1 - focusedColumn_); }));
}

// ---- Frame ------------------------------------------------------------------------------------

MainView::~MainView()
{
    vm_.settings().removeSection("layout");
}

void MainView::render()
{
    UiAutomation::instance().beginFrame();
    emoji_.frame(ImGui::GetTime());
    touch::update(s_); // a finger drags content (before the UI: it may cancel the press)
    terminalView_.setBlinkCursor(vm_.blinkCaret.get());
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    // Inside the safe area (system bars, notch, on-screen keyboard).
    const ImVec2 pos(viewport->WorkPos.x + safeArea_.x, viewport->WorkPos.y + safeArea_.y);
    const ImVec2 size(std::max(1.0f, viewport->WorkSize.x - safeArea_.x - safeArea_.z),
                      std::max(1.0f, viewport->WorkSize.y - safeArea_.y - safeArea_.w));
    layout_ = chooseLayout(size.x / s_, size.y / s_);
    editor_.setCompact(layout_ != Layout::Desktop);
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
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
    if (layout_ != Layout::Desktop) {
        renderCompactTopBar();
        if (compactRibbon_)
            ribbon_.render();
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        renderCompactScreen(avail);
    } else {
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
    map_.releaseIfMapGone(); // whichever view is shown: the closed folder's map layout goes with it
    ImGui::EndChild();
    renderStatusBar();
    }

    renderGoToLinePopup();
    renderCommentPopup();
    renderTerminalWindow();
    renderLanguagePicker();
    renderRecentPicker();
    renderHashResult();
    gitView_.renderPopups();
    renderExplorerPrompt();
    renderCloseFolderPrompt();
    documentList_.renderMenu();
    renderSaveChangesPrompt();
    renderQuitPrompt();
    fileDialogView_.render();
    renderPreferencesPopup();
    renderSyntaxColorsPopup();
    renderIntellisenseSettingsPopup();
    renderAboutPopup();
    renderShortcutsDialog();
    engineLab_.render(layout_ != Layout::Desktop, pos, ImVec2(pos.x + size.x, pos.y + size.y));
    handleCaptionMouse();
    ImGui::End();
    UiAutomation::instance().endFrame();
    agentCursor_.render(); // on top of everything, using this frame's element positions
    if (layout_ != Layout::Desktop)
        keepWindowsOnScreen();
}

// ---- Input --------------------------------------------------------------------------------------

namespace {

// Editing: only while neither a text field nor the terminal has focus.
// Global: everywhere except the terminal, whose keys go to the shell (as in VS Code).
// Always: also while the terminal has focus.
// EditingRepeat: as Editing, repeating while the keys are held (moving lines, adding carets).
enum Scope { Editing, EditingRepeat, Global, Always };
struct Binding {
    ImGuiKeyChord chord;
    CommandId id;
    Scope scope;
};

// Every shortcut key; the Keyboard Shortcuts list shows this table.
const std::vector<Binding>& keyBindings()
{
    using C = CommandId;
    static const std::vector<Binding> bindings = {
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
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V, C::ToggleMarkdownView, Global},
        {ImGuiMod_Ctrl | ImGuiKey_V, C::Paste, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_Z, C::Undo, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_Y, C::Redo, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, C::Redo, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_A, C::SelectAll, Editing},
        // VS Code's editing keys: carets, occurrences, line moves and copies.
        {ImGuiMod_Ctrl | ImGuiKey_D, C::AddNextOccurrence, EditingRepeat},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, C::SelectAllOccurrences, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_UpArrow, C::AddCursorAbove, EditingRepeat},
        {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_DownArrow, C::AddCursorBelow, EditingRepeat},
        {ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_I, C::CursorsAtLineEnds, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_K, C::DeleteLine, EditingRepeat},
        {ImGuiMod_Alt | ImGuiKey_UpArrow, C::MoveLineUp, EditingRepeat},
        {ImGuiMod_Alt | ImGuiKey_DownArrow, C::MoveLineDown, EditingRepeat},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_UpArrow, C::MoveLineUp, EditingRepeat},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_DownArrow, C::MoveLineDown, EditingRepeat},
        {ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_UpArrow, C::CopyLineUp, EditingRepeat},
        {ImGuiMod_Shift | ImGuiMod_Alt | ImGuiKey_DownArrow, C::DuplicateLine, EditingRepeat},
        {ImGuiMod_Ctrl | ImGuiKey_R, C::OpenRecent, Global},
        {ImGuiMod_Ctrl | ImGuiKey_E, C::FocusTerminal, Always},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_U, C::UpperCase, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_U, C::LowerCase, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_Slash, C::ToggleComment, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Slash, C::ToggleComment, Editing}, // Ctrl+? (/ with Shift)
        {ImGuiMod_Ctrl | ImGuiKey_F, C::Find, Global},
        {ImGuiMod_Ctrl | ImGuiKey_H, C::Replace, Global},
        {ImGuiKey_F3, C::FindNext, Global},
        {ImGuiMod_Shift | ImGuiKey_F3, C::FindPrevious, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, C::FindInFiles, Global},
        {ImGuiMod_Ctrl | ImGuiKey_G, C::GoToLine, Global},
        {ImGuiMod_Ctrl | ImGuiKey_Space, C::TriggerCompletion, Editing},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Space, C::ImplementComment, Editing},
        {ImGuiKey_F12, C::GoToDefinition, Editing},
        {ImGuiMod_Alt | ImGuiKey_LeftArrow, C::NavigateBack, Editing},
        {ImGuiMod_Alt | ImGuiKey_RightArrow, C::NavigateForward, Editing},
        {ImGuiMod_Ctrl | ImGuiKey_F2, C::ToggleBookmark, Global},
        {ImGuiKey_F2, C::NextBookmark, Global},
        {ImGuiMod_Shift | ImGuiKey_F2, C::PreviousBookmark, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, C::ToggleExplorer, Global},
        {ImGuiMod_Ctrl | ImGuiKey_J, C::TogglePanel, Always},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_J, C::ToggleMaximizePanel, Always},
        {ImGuiMod_Ctrl | ImGuiKey_Backslash, C::ToggleSplitView, Global},
        {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_Backslash, C::FocusOtherColumn, Global},
        {ImGuiKey_F1, C::KeyboardShortcuts, Global},
        {ImGuiMod_Ctrl | ImGuiKey_F1, C::ToggleSlimRibbon, Always},
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
    return bindings;
}

// "Ctrl+Shift+/" rather than ImGui's "Ctrl+Shift+Slash".
std::string chordLabel(ImGuiKeyChord chord)
{
    std::string s;
    if (chord & ImGuiMod_Ctrl)
        s += ImGui::GetIO().ConfigMacOSXBehaviors ? "Cmd+" : "Ctrl+";
    if (chord & ImGuiMod_Shift)
        s += "Shift+";
    if (chord & ImGuiMod_Alt)
        s += ImGui::GetIO().ConfigMacOSXBehaviors ? "Option+" : "Alt+";
    const ImGuiKey key = ImGuiKey(chord & ~ImGuiMod_Mask_);
    static const std::pair<ImGuiKey, const char*> symbols[] = {
        {ImGuiKey_Slash, "/"},          {ImGuiKey_Backslash, "\\"},     {ImGuiKey_GraveAccent, "`"},
        {ImGuiKey_Equal, "="},          {ImGuiKey_Minus, "-"},          {ImGuiKey_KeypadAdd, "Num +"},
        {ImGuiKey_KeypadSubtract, "Num -"}, {ImGuiKey_UpArrow, "Up"},   {ImGuiKey_DownArrow, "Down"},
        {ImGuiKey_LeftArrow, "Left"},   {ImGuiKey_RightArrow, "Right"}, {ImGuiKey_PageUp, "PgUp"},
        {ImGuiKey_PageDown, "PgDn"},
    };
    for (const auto& [k, name] : symbols)
        if (k == key)
            return s + name;
    return s + ImGui::GetKeyName(key);
}

} // namespace

void MainView::handleShortcuts()
{
    const bool typing = ImGui::GetIO().WantTextInput;
    const bool terminal = terminalView_.focused();
    for (const Binding& b : keyBindings()) {
        const bool editing = b.scope == Editing || b.scope == EditingRepeat;
        if ((editing && (typing || terminal)) || (b.scope == Global && terminal))
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

bool MainView::flatIconButton(const char* label, const char* tooltip, float size, bool enabled)
{
    // No side padding: with it an icon wider than size - 2 * padding is pushed off centre.
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(palette_.hover));
    ImGui::BeginDisabled(!enabled);
    const bool clicked = ImGui::Button(label, ImVec2(size, size));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar();
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s_, 8.0f * s_)); // the ribbon's menus' padding
    const bool fileMenu = ImGui::BeginPopup("##fileMenu");
    ImGui::PopStyleVar();
    if (fileMenu) {
        const CommandId items[] = {CommandId::NewFile, CommandId::OpenFile, CommandId::OpenFolder, CommandId::OpenWorkspace,
                                   CommandId::OpenRecent, CommandId::Save,
                                   CommandId::SaveAs, CommandId::SaveAll, CommandId::CloseFile, CommandId::CloseFolder};
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
    // A build from anything but the release's tag says so: "v1.1.0-dev" (the commit is in the tooltip).
    const char* version = ENP_RELEASE ? "v" ENP_VERSION : "v" ENP_VERSION "-dev";
    const float modeW = frameH + workspaceChevronWidth();
    const float iconW = frameH * 1.1f;
    const float rightW = layoutTogglesWidth() + modeW + 3.0f * iconW + ImGui::CalcTextSize(version).x + 40.0f * s_;
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
    renderLayoutToggles();
    ImGui::SameLine(0.0f, 12.0f * s_);
    renderWorkspaceSwitch();
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
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s_, 8.0f * s_));
    const bool helpMenu = ImGui::BeginPopup("##helpMenu");
    ImGui::PopStyleVar();
    if (helpMenu) {
        if (ImGui::MenuItem(ICON_FA_HAND_POINTER "  Take the tour"))
            vm_.execute(CommandId::StartTour);
        if (ImGui::MenuItem(ICON_FA_KEYBOARD "  Keyboard shortcuts", "F1"))
            vm_.execute(CommandId::KeyboardShortcuts);
        if (ImGui::MenuItem(ICON_FA_GRADUATION_CAP "  Learn Git"))
            vm_.execute(CommandId::GitGuide);
        if (ImGui::MenuItem(ICON_FA_MICROCHIP "  Local model (offline agent)"))
            vm_.execute(CommandId::LocalModel);
        if (vm_.developerMode.get() && ImGui::MenuItem(ICON_FA_FLASK "  Engine Lab"))
            vm_.execute(CommandId::EngineLab);
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CIRCLE_INFO "  About EasyNotepad"))
            vm_.execute(CommandId::About);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 8.0f * s_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", version);
    ImGui::SetItemTooltip(ENP_RELEASE ? "EasyNotepad " ENP_VERSION_FULL " (release, commit " ENP_COMMIT ")"
                                      : "EasyNotepad " ENP_VERSION_FULL " (development build)");
    registerLastItem("version", "text", "EasyNotepad " ENP_VERSION_FULL, nullptr);

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

// ---- Phone layouts ------------------------------------------------------------------------------

void MainView::keepWindowsOnScreen()
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float margin = 6.0f * s_;
    const ImVec2 min(viewport->WorkPos.x + safeArea_.x + margin, viewport->WorkPos.y + safeArea_.y + margin);
    const ImVec2 max(viewport->WorkPos.x + viewport->WorkSize.x - safeArea_.z - margin,
                     viewport->WorkPos.y + viewport->WorkSize.y - safeArea_.w - margin);
    const ImVec2 room(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y));
    for (ImGuiWindow* w : GImGui->Windows) {
        // Root floating windows only: not the app itself, not children, not tooltips (ImGui keeps
        // those on screen itself).
        if (!w->Active || (w->Flags & (ImGuiWindowFlags_ChildWindow | ImGuiWindowFlags_Tooltip)) || std::strcmp(w->Name, "##main") == 0)
            continue;
        ImVec2 size = w->Size;
        if (!(w->Flags & ImGuiWindowFlags_AlwaysAutoResize) && (size.x > room.x || size.y > room.y)) {
            size = ImVec2(std::min(size.x, room.x), std::min(size.y, room.y));
            ImGui::SetWindowSize(w, size, ImGuiCond_Always); // from the next frame
        }
        const ImVec2 pos(std::clamp(w->Pos.x, min.x, std::max(min.x, max.x - size.x)),
                         std::clamp(w->Pos.y, min.y, std::max(min.y, max.y - size.y)));
        if (pos.x != w->Pos.x || pos.y != w->Pos.y)
            ImGui::SetWindowPos(w, pos, ImGuiCond_Always);
    }
}

MainView::Layout MainView::chooseLayout(float widthDp, float heightDp)
{
    if (widthDp < 600.0f) // a phone, or a narrow window
        return widthDp >= heightDp ? Layout::Landscape : Layout::Portrait;
    if (heightDp < 500.0f) // a phone turned sideways
        return Layout::Landscape;
    return Layout::Desktop;
}

void MainView::renderCompactTopBar()
{
    ImGui::PushID("compactBar");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = kTopBarHeight * s_;
    const float button = h - 6.0f * s_;
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), palette_.titleBarBg);
    const float y = origin.y + (h - button) * 0.5f;
    dragRegions_.clear();

    // Left: the ribbon (all commands), behind a menu button.
    ImGui::SetCursorScreenPos(ImVec2(origin.x + 4.0f * s_, y));
    if (flatIconButton(compactRibbon_ ? ICON_FA_XMARK "##ribbon" : ICON_FA_BARS "##ribbon",
                       compactRibbon_ ? "Hide the commands" : "All commands", button))
        compactRibbon_ = !compactRibbon_;
    registerLastItem("compact:ribbon", "button", "All commands", [this]() { compactRibbon_ = !compactRibbon_; });

    // Right: save, undo, more (and the window buttons when the app draws its own title bar).
    const float buttonsX = frame_ ? renderWindowButtons(origin.x + width, origin.y, h) : origin.x + width;
    // Undo and redo only when the document's name keeps some room (they are in the ribbon too).
    const float titleStart = origin.x + 10.0f * s_ + button;
    const bool undoRedo = buttonsX - titleStart - 4.0f * (button + 2.0f * s_) >= 140.0f * s_;
    const int count = undoRedo ? 4 : 2;
    const float rightX = buttonsX - 4.0f * s_ - float(count) * (button + 2.0f * s_);
    ImGui::SetCursorScreenPos(ImVec2(rightX, y));
    for (CommandId id : {CommandId::Save, CommandId::Undo, CommandId::Redo}) {
        if (!undoRedo && id != CommandId::Save)
            continue;
        ImGui::PushID(int(id));
        if (flatIconButton(commandIcon(id), commandInfo(id).tooltip, button, vm_.command(id).canExecute()))
            vm_.execute(id);
        ImGui::PopID();
        ImGui::SameLine(0.0f, 2.0f * s_);
    }
    if (flatIconButton(ICON_FA_ELLIPSIS_VERTICAL "##more", "More", button))
        ImGui::OpenPopup("##compactMenu");
    registerLastItem("compact:more", "button", "More", nullptr);
    if (ImGui::BeginPopup("##compactMenu")) {
        const CommandId items[] = {CommandId::NewFile, CommandId::OpenFile, CommandId::OpenFolder, CommandId::SaveAs,
                                   CommandId::SaveAll, CommandId::CloseFile, CommandId::CloseFolder};
        for (CommandId id : items) {
            const std::string label = std::string(commandIcon(id)) + "  " + commandInfo(id).tooltip;
            if (ImGui::MenuItem(label.c_str(), nullptr, false, vm_.command(id).canExecute()))
                vm_.execute(id);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(vm_.darkTheme.get() ? ICON_FA_SUN "  Light theme" : ICON_FA_MOON "  Dark theme"))
            vm_.execute(CommandId::ToggleTheme);
        for (CommandId id : {CommandId::Preferences, CommandId::LocalModel, CommandId::StartTour, CommandId::GitGuide,
                             CommandId::About}) {
            const std::string label = std::string(commandIcon(id)) + "  " + (id == CommandId::LocalModel
                                                                                ? "Local model (offline agent)"
                                                                                : commandInfo(id).tooltip);
            if (ImGui::MenuItem(label.c_str()))
                vm_.execute(id);
        }
        if (vm_.developerMode.get() && ImGui::MenuItem(ICON_FA_FLASK "  Engine Lab"))
            vm_.execute(CommandId::EngineLab);
        if (os::isDesktop()) {
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_RIGHT_FROM_BRACKET "  Exit"))
                vm_.execute(CommandId::Exit);
        }
        ImGui::EndPopup();
    }

    // Middle: the active document; tap for the list of open documents.
    const float titleX0 = origin.x + 4.0f * s_ + button + 6.0f * s_;
    const float titleX1 = rightX - 6.0f * s_;
    if (titleX1 - titleX0 > 40.0f * s_) {
        const model::Document* doc = vm_.activeDocument();
        std::string title = doc ? doc->title : std::string("No document");
        if (doc && doc->modified)
            title += " " ICON_FA_CIRCLE;
        title += "  " ICON_FA_CARET_DOWN;
        ImGui::SetCursorScreenPos(ImVec2(titleX0, y));
        ImGui::PushStyleVar(ImGuiStyleVar_ButtonTextAlign, ImVec2(0.0f, 0.5f));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(palette_.hover));
        if (ImGui::Button((title + "##documents").c_str(), ImVec2(titleX1 - titleX0, button)))
            ImGui::OpenPopup("##allTabs");
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        registerLastItem("compact:documents", "button", "Open documents", [this]() { openAllTabs_ = true; });
        if (frame_) // the empty part of the bar moves the window
            dragRegions_.push_back({ImVec2(titleX1, origin.y), ImVec2(rightX, origin.y + h)});
    }
    if (openAllTabs_) {
        ImGui::OpenPopup("##allTabs");
        openAllTabs_ = false;
    }
    renderAllTabsPopup(); // (in this ID scope, where it is opened)
    if (frame_) {
        const ImVec2 o = ImGui::GetMainViewport()->Pos;
        std::vector<ScreenRect> local = dragRegions_;
        for (ScreenRect& r : local) {
            r.min = ImVec2(r.min.x - o.x, r.min.y - o.y);
            r.max = ImVec2(r.max.x - o.x, r.max.y - o.y);
        }
        frame_->setDragRegions(local);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopID();
}

void MainView::renderCompactScreen(ImVec2 size)
{
    // A document opened (from the files, a search result, the agent): show it.
    const model::Document* active = vm_.activeDocument();
    const int activeId = active ? active->id : -1;
    if (activeId != lastActiveDocument_ && activeId >= 0 && lastActiveDocument_ != -2)
        screen_ = Screen::Editor;
    lastActiveDocument_ = activeId;

    const bool rail = layout_ == Layout::Landscape;
    const float navW = rail ? 64.0f * s_ : size.x;
    const float navH = rail ? size.y : 58.0f * s_;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 body(rail ? size.x - navW : size.x, rail ? size.y : size.y - navH);

    if (rail) {
        renderNavigation(true, ImVec2(navW, navH));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + navW, origin.y));
    }
    const float gap = 4.0f * s_;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(gap, gap));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(palette_.titleBarBg));
    ImGui::BeginChild("##compactBody", body, ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    const ImVec2 inner = ImGui::GetContentRegionAvail();
    switch (screen_) {
    case Screen::Files:
        beginPanel("##explorer", ICON_FA_SITEMAP "  Files", inner, vm_.explorer().name.c_str());
        renderExplorer();
        endPanel();
        break;
    case Screen::Editor:
        beginPanel("##editorPanel", nullptr, inner);
        if (vm_.editorMode.get() == viewmodel::EditorMode::Map)
            map_.render(ImGui::GetContentRegionAvail());
        else
            editor_.render(ImGui::GetContentRegionAvail());
        endPanel();
        break;
    case Screen::Agent: {
        const char* busy = vm_.agent().busy.get() ? "working..." : nullptr;
        beginPanel("##agentSide", ICON_FA_ROBOT "  Agent", inner, busy);
        agentPanel_.renderConversation(true);
        endPanel();
        break;
    }
    case Screen::Tools:
        renderBottomPanel(inner.x, inner.y);
        break;
    }
    ImGui::EndChild();
    if (!rail) {
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + body.y));
        renderNavigation(false, ImVec2(navW, navH));
    }
}

void MainView::renderNavigation(bool rail, ImVec2 size)
{
    ImGui::PushID("navigation");
    struct Item {
        Screen screen;
        const char* icon;
        const char* label;
        const char* id;
    };
    const Item items[] = {
        {Screen::Files, ICON_FA_FOLDER_TREE, "Files", "nav:Files"},
        {Screen::Editor, ICON_FA_CODE, "Code", "nav:Code"},
        {Screen::Agent, ICON_FA_ROBOT, "Agent", "nav:Agent"},
        {Screen::Tools, ICON_FA_TERMINAL, "Tools", "nav:Tools"},
    };
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), palette_.panelHeaderBg);
    if (rail)
        dl->AddLine(ImVec2(origin.x + size.x - 1.0f, origin.y), ImVec2(origin.x + size.x - 1.0f, origin.y + size.y), palette_.border);
    else
        dl->AddLine(origin, ImVec2(origin.x + size.x, origin.y), palette_.border);
    const int n = int(std::size(items));
    // Upright: the bar's full width, shared. Sideways: square cells from the top.
    const ImVec2 cell = rail ? ImVec2(size.x, std::min(size.y / float(n), 64.0f * s_)) : ImVec2(size.x / float(n), size.y);
    for (int i = 0; i < n; ++i) {
        const Item& item = items[i];
        const ImVec2 min = rail ? ImVec2(origin.x, origin.y + cell.y * float(i)) : ImVec2(origin.x + cell.x * float(i), origin.y);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(i);
        const bool clicked = ImGui::InvisibleButton("##nav", cell);
        ImGui::PopID();
        const bool active = screen_ == item.screen;
        const bool hovered = ImGui::IsItemHovered();
        if (clicked)
            screen_ = item.screen;
        registerLastItem(item.id, "tab", item.label, [this, s = item.screen]() { screen_ = s; });
        const ImU32 color = active ? palette_.accent : hovered ? palette_.text : palette_.textDim;
        // Active: a pill behind the icon (Material style).
        const float iconSize = Fonts::kUiSize * 1.35f * s_;
        const float labelSize = Fonts::kUiSize * 0.78f * s_;
        const float contentH = iconSize + 4.0f * s_ + labelSize;
        const float top = min.y + (cell.y - contentH) * 0.5f;
        const ImVec2 iconDim = fonts_.ui->CalcTextSizeA(iconSize, FLT_MAX, 0.0f, item.icon);
        const float cx = min.x + cell.x * 0.5f;
        if (active)
            dl->AddRectFilled(ImVec2(cx - 26.0f * s_, top - 3.0f * s_), ImVec2(cx + 26.0f * s_, top + iconSize + 3.0f * s_),
                              palette_.accentSoft, (iconSize + 6.0f * s_) * 0.5f);
        dl->AddText(fonts_.ui, iconSize, ImVec2(cx - iconDim.x * 0.5f, top), color, item.icon);
        const ImVec2 labelDim = fonts_.ui->CalcTextSizeA(labelSize, FLT_MAX, 0.0f, item.label);
        dl->AddText(fonts_.ui, labelSize, ImVec2(cx - labelDim.x * 0.5f, top + iconSize + 4.0f * s_), color, item.label);
        // Something waiting: problems in Tools, the agent working.
        const bool badge = (item.screen == Screen::Agent && vm_.agent().busy.get()) ||
                           (item.screen == Screen::Tools && !vm_.review().findings().empty());
        if (badge)
            dl->AddCircleFilled(ImVec2(cx + iconDim.x * 0.5f + 3.0f * s_, top + 2.0f * s_), 3.5f * s_, palette_.iconOrange);
    }
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(size);
    ImGui::PopID();
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
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(main->Size.x * 0.6f, main->Size.y * 0.5f)), ImGuiCond_FirstUseEver);
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
    fit::constrainNextWindow();
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
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(320.0f * s_, 0.0f)));
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

void MainView::renderRecentPicker()
{
    // Ctrl+R: recent folders (the open one left out, so the first entry is the previous folder),
    // then recent files; filtered as you type; Up/Down (or Ctrl+R again) choose, Enter opens.
    if (openRecentPicker_) {
        ImGui::OpenPopup("##recentPicker");
        openRecentPicker_ = false;
        recentFilter_.clear();
        recentSelected_ = 0;
    }
    const ImGuiViewport* main = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(main->Pos.x + main->Size.x * 0.5f, main->Pos.y + 80.0f * s_), ImGuiCond_Appearing, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(560.0f * s_, 0.0f)));
    if (!ImGui::BeginPopup("##recentPicker"))
        return;
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputTextWithHint("##filter", ICON_FA_CLOCK_ROTATE_LEFT "  Open recent folder or file",
                                                &recentFilter_, ImGuiInputTextFlags_EnterReturnsTrue);
    if (ImGui::IsItemEdited())
        recentSelected_ = 0;
    registerLastItem("recent-picker:filter", "textbox", "Recent", nullptr);

    std::string needle = recentFilter_;
    for (char& c : needle)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    auto matches = [&needle](const std::string& path) {
        if (needle.empty())
            return true;
        std::string p = path;
        for (char& c : p)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return p.find(needle) != std::string::npos;
    };
    struct Entry {
        const std::string* path;
        bool folder;
    };
    std::vector<Entry> entries;
    for (const std::string& path : vm_.recentFolders())
        if (path != vm_.workspaceRoot() && matches(path))
            entries.push_back({&path, true});
    for (const std::string& path : vm_.recentFiles())
        if (matches(path))
            entries.push_back({&path, false});

    const int count = int(entries.size());
    const int before = recentSelected_;
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow))
        ++recentSelected_;
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow))
        --recentSelected_;
    recentSelected_ += recentStep_;
    recentStep_ = 0;
    if (count > 0)
        recentSelected_ = (recentSelected_ % count + count) % count;
    const bool moved = recentSelected_ != before;

    std::string chosen;
    const float rowH = ImGui::GetTextLineHeightWithSpacing();
    ImGui::BeginChild("##recent", ImVec2(0.0f, std::min(440.0f * s_, float(std::max(1, count)) * rowH + 8.0f * s_)));
    for (int i = 0; i < count; ++i) {
        const Entry& e = entries[size_t(i)];
        // A folder: its name first (long paths are cut at the right), then where it is.
        const size_t slash = e.path->find_last_of('/');
        const std::string shown = e.folder && slash != std::string::npos && slash + 1 < e.path->size()
                                      ? e.path->substr(slash + 1) + "   " + e.path->substr(0, slash)
                                      : vm_.workspaceRelative(*e.path);
        const std::string label = std::string(e.folder ? ICON_FA_FOLDER "  " : ICON_FA_FILE "  ") + shown + "##recent" + std::to_string(i);
        if (ImGui::Selectable(label.c_str(), i == recentSelected_))
            chosen = *e.path;
        if (i == recentSelected_ && moved)
            ImGui::SetScrollHereY();
        registerLastItem("recent-picker:" + std::to_string(i), "button", *e.path,
                         [this, path = *e.path]() { vm_.openRecent(path); });
    }
    if (count == 0)
        ImGui::TextDisabled(vm_.recentFolders().empty() && vm_.recentFiles().empty() ? "Nothing opened yet" : "Nothing matches");
    ImGui::EndChild();
    ImGui::TextDisabled("Enter: open   Up/Down or Ctrl+R: choose   Esc: cancel");
    if (enter && count > 0)
        chosen = *entries[size_t(recentSelected_)].path;
    if (!chosen.empty()) {
        ImGui::CloseCurrentPopup();
        vm_.openRecent(chosen);
    }
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
    fit::constrainNextWindow();
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

void MainView::renderCloseFolderPrompt()
{
    using Choice = viewmodel::NotepadViewModel::CloseChoice;
    const bool pending = vm_.closeFolderPending.get();
    if (pending && !ImGui::IsPopupOpen("Close the folder?"))
        ImGui::OpenPopup("Close the folder?");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Close the folder?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
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
    if (ImGui::Button(ICON_FA_FLOPPY_DISK "  Save All and Close"))
        choice = Choice::Save;
    ImGui::EndDisabled();
    if (!allSavable)
        ImGui::SetItemTooltip("Untitled documents need Save As first");
    registerLastItem("dialog:close-folder:save", "button", "Save All and Close", [this]() { vm_.resolveCloseFolder(Choice::Save); }, allSavable);
    ImGui::SameLine();
    if (ImGui::Button("Close Without Saving"))
        choice = Choice::Discard;
    registerLastItem("dialog:close-folder:discard", "button", "Close Without Saving", [this]() { vm_.resolveCloseFolder(Choice::Discard); });
    ImGui::SameLine();
    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape))
        choice = Choice::Cancel;
    registerLastItem("dialog:close-folder:cancel", "button", "Cancel", [this]() { vm_.resolveCloseFolder(Choice::Cancel); });
    if (choice) {
        ImGui::CloseCurrentPopup();
        vm_.resolveCloseFolder(*choice);
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
    fit::constrainNextWindow();
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

namespace {

// The workspaces, in the order the switch goes through them (WorkspaceMode, kWorkspaceModeCount).
struct Workspace {
    WorkspaceMode mode;
    CommandId command; // switches to it; its icon and label
    const char* automationId;
};
constexpr Workspace kWorkspaces[] = {{WorkspaceMode::Agent, CommandId::SwitchToAgentMode, "workspace:agent"},
                                     {WorkspaceMode::Code, CommandId::SwitchToCodeMode, "workspace:code"}};
static_assert(std::size(kWorkspaces) == size_t(viewmodel::kWorkspaceModeCount), "a workspace without its switch entry");

} // namespace

float MainView::workspaceChevronWidth() const
{
    return std::round(ImGui::GetFrameHeight() * 0.6f);
}

void MainView::renderWorkspaceSwitch()
{
    const Workspace* current = &kWorkspaces[0];
    for (const Workspace& w : kWorkspaces)
        if (w.mode == vm_.workspace.get())
            current = &w;
    auto modeColor = [this](WorkspaceMode mode) { return mode == WorkspaceMode::Agent ? palette_.modeAgent : palette_.modeCode; };
    const float size = ImGui::GetFrameHeight();
    const float rounding = 5.0f * s_;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID("workspaceSwitch");

    // The icon of the current workspace: a click goes on to the next one.
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const bool next = ImGui::InvisibleButton("##next", ImVec2(size, size));
    const bool iconHovered = ImGui::IsItemHovered();
    registerLastItem("workspace-switch", "button", std::string(commandInfo(current->command).label) + " workspace",
                     [this]() { vm_.execute(CommandId::ToggleWorkspaceMode); });
    if (iconHovered)
        ImGui::SetTooltip("%s workspace: click for the next one (%s)", commandInfo(current->command).label,
                          commandInfo(CommandId::ToggleWorkspaceMode).shortcut);
    // The chevron, right against it: the list of workspaces.
    ImGui::SameLine(0.0f, 0.0f);
    const float chevronW = workspaceChevronWidth();
    const bool open = ImGui::InvisibleButton("##menu", ImVec2(chevronW, size)) || openWorkspaceMenu_;
    const bool chevronHovered = ImGui::IsItemHovered();
    openWorkspaceMenu_ = false;
    registerLastItem("workspace-menu", "button", "Workspaces", [this]() { openWorkspaceMenu_ = true; });
    if (chevronHovered)
        ImGui::SetTooltip("Workspaces");

    // One rounded block, each half lit on its own when hovered.
    const ImVec2 end(at.x + size + chevronW, at.y + size);
    if (iconHovered)
        dl->AddRectFilled(at, ImVec2(at.x + size, end.y), palette_.hover, rounding, ImDrawFlags_RoundLeft);
    if (chevronHovered || ImGui::IsPopupOpen("##workspaceMenu"))
        dl->AddRectFilled(ImVec2(at.x + size, at.y), end, palette_.hover, rounding, ImDrawFlags_RoundRight);
    const char* icon = commandIcon(current->command);
    const ImVec2 iconSize = ImGui::CalcTextSize(icon);
    dl->AddText(ImVec2(std::floor(at.x + (size - iconSize.x) * 0.5f), std::floor(at.y + (size - iconSize.y) * 0.5f)),
                modeColor(current->mode), icon);
    // The chevron is drawn small, like a caret, close to the icon.
    const float c = std::round(3.5f * s_);
    const ImVec2 m(std::floor(at.x + size + chevronW * 0.4f) + 0.5f, std::floor(at.y + size * 0.5f) + 0.5f);
    dl->AddTriangleFilled(ImVec2(m.x - c, m.y - c * 0.5f), ImVec2(m.x + c, m.y - c * 0.5f), ImVec2(m.x, m.y + c * 0.6f), palette_.textDim);

    if (next)
        vm_.execute(CommandId::ToggleWorkspaceMode);
    if (open)
        ImGui::OpenPopup("##workspaceMenu");
    ImGui::SetNextWindowPos(ImVec2(end.x, end.y + 4.0f * s_), ImGuiCond_Always, ImVec2(1.0f, 0.0f)); // under it, right-aligned
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s_, 8.0f * s_)); // the other menus' padding
    const bool menu = ImGui::BeginPopup("##workspaceMenu");
    ImGui::PopStyleVar();
    if (menu) {
        // Each workspace: its icon in its colour, its name, and what it is for.
        const float rowW = 300.0f * s_, rowH = ImGui::GetTextLineHeight() * 2.0f + 10.0f * s_;
        for (const Workspace& w : kWorkspaces) {
            const viewmodel::CommandInfo& info = commandInfo(w.command);
            ImGui::PushID(w.automationId);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::Selectable("##workspace", w.mode == current->mode, ImGuiSelectableFlags_None, ImVec2(rowW, rowH)))
                vm_.execute(w.command);
            registerLastItem(w.automationId, "menuitem", std::string(info.label) + " workspace",
                             [this, id = w.command]() { vm_.execute(id); });
            ImGui::PopID();
            ImDrawList* pdl = ImGui::GetWindowDrawList();
            const char* wIcon = commandIcon(w.command);
            pdl->AddText(ImVec2(p.x + 6.0f * s_, p.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f), modeColor(w.mode), wIcon);
            const float textX = p.x + 6.0f * s_ + ImGui::GetFontSize() * 1.6f;
            pdl->AddText(ImVec2(textX, p.y + 4.0f * s_), palette_.text, info.label);
            const char* colon = std::strchr(info.tooltip, ':'); // "Agent-centric workspace: describe ..." -> "describe ..."
            const char* detail = colon ? colon + 2 : info.tooltip;
            pdl->PushClipRect(p, ImVec2(p.x + rowW, p.y + rowH), true);
            pdl->AddText(ImVec2(textX, p.y + 6.0f * s_ + ImGui::GetTextLineHeight()), palette_.textDim, detail);
            pdl->PopClipRect();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Next workspace: click the icon, or %s", commandInfo(CommandId::ToggleWorkspaceMode).shortcut);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

float MainView::layoutTogglesWidth() const
{
    return 3.0f * ImGui::GetFrameHeight() + 2.0f * 2.0f * s_ + 12.0f * s_; // three buttons, their gaps, the gap after
}

void MainView::renderLayoutToggles()
{
    struct Toggle {
        CommandId id;
        const char* automationId;
        int pane; // 0 left, 1 bottom, 2 right
    };
    static constexpr Toggle toggles[] = {{CommandId::ToggleExplorer, "layout:explorer", 0},
                                         {CommandId::TogglePanel, "layout:panel", 1},
                                         {CommandId::ToggleAgentPanel, "layout:agent-panel", 2}};
    // The agent workspace has no agent side panel: its right-hand side is always the preview.
    const bool codeWorkspace = vm_.workspace.get() == WorkspaceMode::Code;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float size = ImGui::GetFrameHeight();
    const float thickness = std::max(1.0f, s_);
    ImGui::PushID("layoutToggles");
    for (const Toggle& t : toggles) {
        const bool enabled = t.pane != 2 || codeWorkspace;
        const bool on = enabled && vm_.isChecked(t.id);
        if (t.pane > 0)
            ImGui::SameLine(0.0f, 2.0f * s_);
        ImGui::PushID(t.pane);
        ImGui::BeginDisabled(!enabled);
        const bool clicked = ImGui::InvisibleButton("##layoutToggle", ImVec2(size, size));
        ImGui::EndDisabled();
        ImGui::PopID();
        const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            if (enabled)
                dl->AddRectFilled(a, b, palette_.hover, 4.0f * s_);
            const viewmodel::CommandInfo& info = commandInfo(t.id);
            if (enabled)
                ImGui::SetTooltip("%s (%s)", info.tooltip, info.shortcut);
            else
                ImGui::SetTooltip("%s: in the Code workspace", info.tooltip);
        }
        if (clicked && enabled)
            vm_.execute(t.id);
        registerLastItem(t.automationId, "toggle", commandInfo(t.id).label, [this, id = t.id]() { vm_.execute(id); }, enabled);

        // The window: 14 x 12, on whole pixels so its 1 px lines stay sharp.
        const ImU32 c = enabled ? palette_.text : palette_.textDim;
        const float w = std::round(14.0f * s_), h = std::round(12.0f * s_);
        const ImVec2 o(std::floor((a.x + b.x - w) * 0.5f) + 0.5f, std::floor((a.y + b.y - h) * 0.5f) + 0.5f);
        const ImVec2 e(o.x + w, o.y + h);
        const float side = std::round(w * 0.32f), bottom = std::round(h * 0.38f);
        ImVec2 p0, p1; // the pane
        if (t.pane == 0)
            p0 = o, p1 = ImVec2(o.x + side, e.y);
        else if (t.pane == 1)
            p0 = ImVec2(o.x, e.y - bottom), p1 = e;
        else
            p0 = ImVec2(e.x - side, o.y), p1 = e;
        if (on)
            dl->AddRectFilled(p0, p1, c);
        else if (t.pane == 1)
            dl->AddLine(p0, ImVec2(p1.x, p0.y), c, thickness);
        else
            dl->AddLine(t.pane == 0 ? ImVec2(p1.x, p0.y) : p0, t.pane == 0 ? p1 : ImVec2(p0.x, p1.y), c, thickness);
        dl->AddRect(o, e, c, 1.5f * s_, thickness);
    }
    ImGui::PopID();
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

bool MainView::beginPanel(const char* id, const char* title, ImVec2 size, const char* detail, std::function<void()> onClose,
                          const char* closeTooltip)
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
    float right = p.x + w - 10.0f * s_; // where the detail text ends
    if (onClose) {
        // An X button at the header's right edge; the detail text moves left of it.
        const float button = std::min(h - 4.0f * s_, ImGui::GetFrameHeight());
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - button - 4.0f * s_, p.y + (h - button) * 0.5f));
        const bool clicked = flatIconButton(ICON_FA_XMARK "##close", closeTooltip, button);
        registerLastItem(std::string("panel-close:") + (id[0] == '#' && id[1] == '#' ? id + 2 : id), "button",
                         closeTooltip ? closeTooltip : "Close", onClose);
        if (clicked)
            onClose();
        right = p.x + w - button - 10.0f * s_;
    }
    if (detail) {
        const float dw = ImGui::CalcTextSize(detail).x;
        dl->AddText(ImVec2(right - dw, textY), palette_.textFaint, detail);
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

namespace {

model::Document* documentWithId(viewmodel::NotepadViewModel& vm, int id, int* index = nullptr)
{
    const auto& docs = vm.documents();
    for (size_t i = 0; i < docs.size(); ++i)
        if (docs[i]->id == id) {
            if (index)
                *index = int(i);
            return docs[i].get();
        }
    return nullptr;
}

} // namespace

void MainView::focusColumn(int column)
{
    if (!editor2_ || column == focusedColumn_)
        return;
    const model::Document* active = vm_.activeDocument();
    int index = -1;
    documentWithId(vm_, otherDocument_, &index);
    otherDocument_ = active ? active->id : -1;
    focusedColumn_ = column;
    if (index >= 0)
        vm_.activate(index);
    focusedEditor().focus();
}

void MainView::closeColumn(int column)
{
    if (column == focusedColumn_) { // the other column's document stays
        int index = -1;
        documentWithId(vm_, otherDocument_, &index);
        if (index >= 0)
            vm_.activate(index);
    }
    vm_.splitView.set(false); // the second view goes on the next frame (it may be drawing now)
}

void MainView::renderEditors(ImVec2 size)
{
    // Each column is a child window of its own: the two views' windows and items get separate IDs,
    // and a click anywhere in the column without the keyboard is seen here.
    const ImGuiWindowFlags columnFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
    const bool split = vm_.splitView.get() && layout_ == Layout::Desktop && !vm_.documents().empty();
    if (!split) {
        if (editor2_) { // joined: the second column and its caches go
            editor2_.reset();
            vm_.requestMemoryTrim(); // released outside the close path (docs/MEMORY.md rule 9)
            if (focusedColumn_ == 1)
                editor_.focus();
            focusedColumn_ = 0;
            otherDocument_ = -1;
        }
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild("##column0", size, ImGuiChildFlags_None, columnFlags);
        ImGui::PopStyleVar();
        editor_.render(ImGui::GetContentRegionAvail());
        ImGui::EndChild();
        return;
    }
    if (!editor2_) {
        editor2_ = std::make_unique<EditorView>(vm_, fonts_, palette_, s_);
        editor2_->setEmoji(&emoji_);
        focusedColumn_ = 0;
        // The second column starts on the next document (the same one when there is only one).
        const auto& docs = vm_.documents();
        const int n = int(docs.size());
        otherDocument_ = docs[size_t((std::max(0, vm_.activeIndex.get()) + 1) % n)]->id;
    }
    model::Document* active = vm_.activeDocument();
    model::Document* other = documentWithId(vm_, otherDocument_);
    if (!other) { // closed: the active document in both
        other = active;
        otherDocument_ = active ? active->id : -1;
    }
    // The layout values are one set: the column with the keyboard may change them.
    EditorView& second = *editor2_;
    second.sideBarWidth() = editor_.sideBarWidth();
    second.markdownColumn() = editor_.markdownColumn();
    second.blameColumns() = editor_.blameColumns();

    const float gap = kGap * s_;
    const float avail = std::max(2.0f, size.x - gap);
    const float minW = std::min(avail * 0.5f, 220.0f * s_);
    float firstW = std::clamp(std::round(avail * splitRatio_), minW, avail - minW);
    int clicked = -1;
    for (int c = 0; c < 2; ++c) {
        if (c == 1) {
            ImGui::SameLine(0.0f, 0.0f);
            verticalSplitter("##columnSplit", firstW, 1.0f, size.y, minW, avail - minW);
            splitRatio_ = firstW / avail;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                splitRatio_ = 0.5f; // halves again
                vm_.settings().markDirty();
            }
            ImGui::SetItemTooltip("Drag to resize the columns; double-click for halves");
            ImGui::SameLine(0.0f, 0.0f);
        }
        EditorView& view = c == 0 ? editor_ : second;
        const bool focused = c == focusedColumn_;
        model::Document* doc = focused ? active : other;
        const float w = c == 0 ? firstW : avail - firstW;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::BeginChild(c == 0 ? "##column0" : "##column1", ImVec2(w, size.y), ImGuiChildFlags_None, columnFlags);
        ImGui::PopStyleVar();

        // Header: the document's name; the column with the keyboard is marked with the accent.
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float frameH = ImGui::GetFrameHeight();
        const float headerH = frameH + 4.0f * s_;
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + headerH), focused ? palette_.editorBg : palette_.panelBg);
        dl->AddLine(ImVec2(p.x, p.y + headerH - 1.0f), ImVec2(p.x + w, p.y + headerH - 1.0f), palette_.border);
        if (focused)
            dl->AddRectFilled(p, ImVec2(p.x + w, p.y + 2.0f * s_), palette_.accent);
        if (doc) {
            ImGui::SetCursorScreenPos(ImVec2(p.x + 10.0f * s_, p.y + (headerH - frameH) * 0.5f));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col(focused ? palette_.accent : palette_.textDim), "%s", fileIcon(doc->title));
            ImGui::SameLine(0.0f, 6.0f * s_);
            ImGui::TextColored(col(focused ? palette_.text : palette_.textDim), "%s%s", doc->title.c_str(), doc->modified ? "  \xe2\x97\x8f" : "");
        }
        ImGui::SetCursorScreenPos(ImVec2(p.x + w - frameH - 6.0f * s_, p.y + (headerH - frameH) * 0.5f));
        bool closing = false;
        if (flatIconButton(ICON_FA_XMARK "##closeColumn", "Close this column", frameH)) {
            closeColumn(c);
            closing = true; // not also a click that moves the keyboard here
        }
        registerLastItem(c == 0 ? "column:0:close" : "column:1:close", "button", "Close this column", [this, c]() { closeColumn(c); });
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + headerH));
        ImGui::Dummy(ImVec2(w, 0.0f));
        ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + headerH));

        const ImVec2 body = ImGui::GetContentRegionAvail();
        if (focused) {
            view.render(body);
        } else { // to look at and scroll; its buttons wait for the column to have the keyboard
            ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
            ImGui::BeginDisabled();
            view.render(body, doc);
            ImGui::EndDisabled();
            ImGui::PopStyleVar();
            if (!closing && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                (ImGui::IsMouseClicked(ImGuiMouseButton_Left) || ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                 ImGui::IsMouseClicked(ImGuiMouseButton_Middle)))
                clicked = c;
        }
        UiAutomation::instance().add({c == 0 ? "column:0" : "column:1", "panel", doc ? doc->title : std::string("Column"), p,
                                      ImVec2(p.x + w, p.y + size.y), true, [this, c]() { focusColumn(c); }});
        ImGui::EndChild();
    }
    if (focusedColumn_ == 1) { // the second view's changes are the layout's
        editor_.sideBarWidth() = second.sideBarWidth();
        editor_.markdownColumn() = second.markdownColumn();
        editor_.blameColumns() = second.blameColumns();
    }
    if (clicked >= 0)
        focusColumn(clicked);
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
    if (vm_.editorMode.get() == viewmodel::EditorMode::Map) {
        map_.render(ImGui::GetContentRegionAvail());
    } else {
        renderEditors(ImGui::GetContentRegionAvail());
    }
    endPanel();
    if (side) {
        ImGui::SameLine(0.0f, 0.0f);
        verticalSplitter("##splitAgent", agentSideWidth_, -1.0f, topH, 260.0f * s_, avail.x * 0.5f);
        ImGui::SameLine(0.0f, 0.0f);
        const char* busy = vm_.agent().busy.get() ? "working..." : nullptr;
        beginPanel("##agentSide", ICON_FA_ROBOT "  Agent", ImVec2(agentSideWidth_, topH), busy,
                   [this]() { vm_.showAgentPanel.set(false); }, "Close the Agent panel (Ctrl+Shift+I)");
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
        {PanelTab::Status, "STATUS", gitView_.changeCount() ? std::to_string(gitView_.changeCount()) : std::string()},
        {PanelTab::History, "HISTORY", {}},
    };
    const PanelTab current = vm_.panelTab.get();
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    float x = p.x + 8.0f * s_;
    const float textY = p.y + (headerH - ImGui::GetTextLineHeight()) * 0.5f;
    auto tabName = [](const char* label) { // "TERMINAL" -> "Terminal"
        std::string name = label;
        for (size_t k = 1; k < name.size(); ++k)
            name[k] = char(std::tolower(static_cast<unsigned char>(name[k])));
        return name;
    };
    float allTabsW = 0.0f;
    for (const TabInfo& t : tabs)
        allTabsW += ImGui::CalcTextSize(t.label).x + (t.badge.empty() ? 0.0f : ImGui::CalcTextSize(t.badge.c_str()).x + 12.0f * s_) +
                    20.0f * s_;
    // Too narrow for the row (a phone): the current tab, opening a list of them all.
    const bool tabList = allTabsW > w - 110.0f * s_;
    if (tabList) {
        const TabInfo* shown = &tabs[0];
        for (const TabInfo& t : tabs)
            if (t.tab == current)
                shown = &t;
        std::string caption = std::string(shown->label) + (shown->badge.empty() ? "" : "  " + shown->badge) + "  " ICON_FA_CARET_DOWN;
        const float tabW = ImGui::CalcTextSize(caption.c_str()).x + 20.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        if (ImGui::InvisibleButton("##panelTabList", ImVec2(tabW, headerH)))
            ImGui::OpenPopup("##panelTabs");
        for (const TabInfo& t : tabs) { // agents still reach every tab by its id
            const PanelTab tab = t.tab;
            registerLastItem("panel-tab:" + tabName(t.label), "tab", tabName(t.label), [this, tab]() { vm_.showPanelTab(tab); });
        }
        dl->AddText(ImVec2(x + 10.0f * s_, textY), palette_.text, caption.c_str());
        dl->AddRectFilled(ImVec2(x + 8.0f * s_, p.y + headerH - 2.0f * s_), ImVec2(x + tabW - 8.0f * s_, p.y + headerH),
                          palette_.accent);
        ImGui::SetNextWindowPos(ImVec2(x, p.y + headerH));
        if (ImGui::BeginPopup("##panelTabs")) {
            for (const TabInfo& t : tabs) {
                const std::string item = tabName(t.label) + (t.badge.empty() ? "" : "  (" + t.badge + ")");
                if (ImGui::Selectable(item.c_str(), t.tab == current))
                    vm_.showPanelTab(t.tab);
            }
            ImGui::EndPopup();
        }
        x += tabW;
    }
    for (size_t i = 0; i < (tabList ? 0 : std::size(tabs)); ++i) {
        const TabInfo& t = tabs[i];
        const float labelW = ImGui::CalcTextSize(t.label).x;
        const float badgeW = t.badge.empty() ? 0.0f : ImGui::CalcTextSize(t.badge.c_str()).x + 12.0f * s_;
        const float tabW = labelW + badgeW + 20.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, p.y));
        ImGui::PushID(int(t.tab));
        if (ImGui::InvisibleButton("##panelTab", ImVec2(tabW, headerH)))
            vm_.showPanelTab(t.tab);
        {
            const PanelTab tab = t.tab;
            registerLastItem("panel-tab:" + tabName(t.label), "tab", tabName(t.label), [this, tab]() { vm_.showPanelTab(tab); });
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
    else if (current == PanelTab::Status)
        gitView_.renderStatus();
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
    if (layout_ == Layout::Desktop) { // a phone shows the panel as a whole screen of its own
        if (action(ICON_FA_XMARK "##closePanel", "Close panel (Ctrl+J)")) {
            vm_.showPanel.set(false);
            vm_.panelMaximized.set(false);
        }
        const bool maximized = vm_.panelMaximized.get();
        if (action(maximized ? ICON_FA_CHEVRON_DOWN "##restorePanel" : ICON_FA_CHEVRON_UP "##maximizePanel",
                   maximized ? "Restore panel size" : "Maximize panel"))
            vm_.execute(CommandId::ToggleMaximizePanel);
    }
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
        renderEditors(rest);
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
        // No disclosure arrow: the folder icon says it, closed or open (the icon of "Open a folder").
        // The row is a leaf tree node, so the open state is kept here, in the window's storage.
        ImGuiStorage* storage = ImGui::GetStateStorage();
        const ImGuiID openKey = ImGui::GetID(("open:" + item.path).c_str());
        bool open = storage->GetInt(openKey, root ? 1 : 0) != 0;
        if (!explorerFilter_.empty() || (reveal && holdsActive))
            open = true;
        else if (explorerCollapse_ && !root)
            open = false;
        // A label-less row; icon and name are drawn from its left edge (a tree node would keep an
        // arrow-wide gap in front of them even without the arrow).
        ImGui::TreeNodeEx(("##folder:" + item.path).c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen |
                                                                  ImGuiTreeNodeFlags_SpanAvailWidth);
        {
            const ImVec2 rowMin = ImGui::GetItemRectMin(), rowMax = ImGui::GetItemRectMax();
            const float textY = (rowMin.y + rowMax.y) * 0.5f - ImGui::GetTextLineHeight() * 0.5f;
            const float iconX = rowMin.x + explorerRowPad();
            dl->AddText(ImVec2(iconX, textY), palette_.textDim, open ? ICON_FA_FOLDER_OPEN : ICON_FA_FOLDER);
            dl->AddText(ImVec2(iconX + explorerIconColumn(), textY), palette_.text, item.name.c_str());
        }
        // A click toggles it, as do Enter and the arrow keys when it has keyboard focus.
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || (!explorerToggle_.empty() && explorerToggle_ == item.path)) {
            open = !open;
            os::mem::release(explorerToggle_);
        }
        else if (ImGui::IsItemFocused()) {
            if (ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter))
                open = !open;
            else if (ImGui::IsKeyPressed(ImGuiKey_RightArrow))
                open = true;
            else if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow))
                open = false;
        }
        storage->SetInt(openKey, open ? 1 : 0);
        registerLastItem("folder:" + item.path, "button", item.name, [this, path = item.path]() { explorerToggle_ = path; });
        if (ImGui::BeginPopupContextItem(("##folderMenu" + item.path).c_str())) {
            explorerMenu(item, root);
            ImGui::EndPopup();
        }
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
            if (!item.scanned)
                vm_.expandExplorer(item.path); // deep folders are read when first shown
            ImGui::TreePush(item.path.c_str()); // the children's indent and ID scope
            for (const viewmodel::ExplorerItem& child : item.children)
                renderExplorerItem(child);
            if (item.truncated)
                ImGui::TextDisabled("%s", "...  more entries than the explorer shows");
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
    if (ImGui::BeginPopupContextItem(("##fileMenu" + item.path).c_str())) {
        explorerMenu(item, false);
        ImGui::EndPopup();
    }
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
        const float iconX = rowMin.x + explorerRowPad();
        dl->AddText(ImVec2(iconX, textY), selected ? palette_.accent : openDoc ? palette_.textDim : palette_.textFaint, icon);
        const float nameX = iconX + explorerIconColumn();
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
        if (flatIconButton(ICON_FA_CROSSHAIRS "##reveal", "Reveal the active file", button, vm_.activeDocument() != nullptr))
            explorerRevealSerial_ = -1;
        registerLastItem("explorer-reveal", "button", "Reveal the active file", [this]() { explorerRevealSerial_ = -1; });
        ImGui::SameLine(0.0f, spacing);
        if (flatIconButton(ICON_FA_FOLDER_MINUS "##collapse", "Collapse all folders", button))
            explorerCollapse_ = true;
        registerLastItem("explorer-collapse", "button", "Collapse all folders", [this]() { explorerCollapse_ = true; });
    }

    ImGui::BeginChild("##explorerBody", ImVec2(0.0f, 0.0f));
    if (files) {
        if (vm_.noFolder()) {
            ImGui::Spacing();
            ImGui::TextDisabled("No folder open.");
            ImGui::Spacing();
            if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open a folder", ImVec2(-1.0f, 0.0f)))
                vm_.execute(CommandId::OpenFolder);
            registerLastItem("explorer:open-folder", "button", "Open a folder", [this]() { vm_.execute(CommandId::OpenFolder); });
            if (!vm_.recentFolders().empty()) {
                ImGui::Spacing();
                ImGui::TextDisabled("Recent");
                for (const std::string& path : vm_.recentFolders()) {
                    ImGui::PushID(path.c_str());
                    if (ImGui::Selectable((ICON_FA_FOLDER "  " + path.substr(path.find_last_of('/') + 1)).c_str()))
                        vm_.openRecent(path);
                    ImGui::SetItemTooltip("%s", path.c_str());
                    ImGui::PopID();
                }
            }
        }
        ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 12.0f * s_);
        if (!vm_.noFolder())
            renderExplorerItem(vm_.explorer());
        ImGui::PopStyleVar();
        // Right-click below the tree: the opened folder's menu (new file, new folder, ...).
        if (!vm_.noFolder() && ImGui::BeginPopupContextWindow("##explorerBlank", ImGuiPopupFlags_MouseButtonRight | ImGuiPopupFlags_NoOpenOverItems)) {
            explorerMenu(vm_.explorer(), true);
            ImGui::EndPopup();
        }
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

// ---- Explorer: right-click menu ------------------------------------------------------------------

void MainView::explorerMenu(const viewmodel::ExplorerItem& item, bool root)
{
    // `item` is a file, a folder, or (root) the opened folder itself.
    const bool editable = vm_.canEditFiles();
    const bool folder = item.isDirectory || root;
    const std::string path = root ? std::string() : item.path;
    const std::string parentFolder = [&]() {
        const size_t slash = path.find_last_of('/');
        return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }();
    const std::string here = folder ? path : parentFolder; // where New File / New Folder go
    const std::string name = root ? std::string("the folder") : item.name;
    viewmodel::GitViewModel& git = vm_.git();
    const bool gitReady = !git.demo() && git.available() && git.repo().repository;

    if (!folder) {
        const int openIndex = vm_.documentIndex(path);
        if (openIndex < 0 && ImGui::MenuItem(ICON_FA_FILE "  Open"))
            vm_.openFile(path);
        if (openIndex >= 0 && ImGui::MenuItem(ICON_FA_XMARK "  Close"))
            vm_.requestCloseDocument(openIndex);
        const char status = vm_.git().statusOf(path);
        if (gitReady && status && ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Open changes"))
            git.showChange(git.repoPath(path), status == 'U' ? model::git::Area::Untracked : model::git::Area::Changes);
        ImGui::Separator();
    }
    ImGui::BeginDisabled(!editable);
    if (ImGui::MenuItem(ICON_FA_FILE_CIRCLE_PLUS "  New File..."))
        explorerAsk("New file in " + (here.empty() ? std::string("the folder") : here), "File name, e.g. notes.md", {}, "Create",
                    [this, here](const std::string& n) { vm_.createFile(here, n); });
    if (ImGui::MenuItem(ICON_FA_FOLDER_PLUS "  New Folder..."))
        explorerAsk("New folder in " + (here.empty() ? std::string("the folder") : here), "Folder name", {}, "Create",
                    [this, here](const std::string& n) { vm_.createFolder(here, n); });
    ImGui::EndDisabled();
    if (folder) {
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_MAGNIFYING_GLASS "  Find in Folder...", nullptr, false, vm_.canEditFiles()))
            vm_.findInFolder(path);
        if (ImGui::MenuItem(ICON_FA_TERMINAL "  Open Terminal Here", nullptr, false, vm_.canEditFiles() && vm_.terminal().supported()))
            vm_.openTerminalIn(path);
    }
    if (root) {
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_EYE "  Show Hidden Files", nullptr, vm_.showHiddenFiles.get()))
            vm_.showHiddenFiles.set(!vm_.showHiddenFiles.get());
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy Path"))
        ImGui::SetClipboardText(vm_.absolutePath(path.empty() ? "." : path).c_str());
    if (!root && ImGui::MenuItem(ICON_FA_COPY "  Copy Relative Path"))
        ImGui::SetClipboardText(path.c_str());
#if defined(_WIN32)
    const char* revealLabel = ICON_FA_FOLDER_OPEN "  Reveal in File Explorer";
#elif defined(__APPLE__)
    const char* revealLabel = ICON_FA_FOLDER_OPEN "  Reveal in Finder";
#else
    const char* revealLabel = ICON_FA_FOLDER_OPEN "  Open in File Manager";
#endif
    if (ImGui::MenuItem(revealLabel, nullptr, false, vm_.canEditFiles()))
        vm_.revealInFileManager(path.empty() ? "." : path);

    // Git, for what changed under this file or folder.
    if (gitReady) {
        std::vector<std::string> changed, staged;
        const std::string prefix = git.repoPath(path);
        for (const model::git::Change& c : git.repo().status.changes) {
            const bool inside = folder ? (prefix.empty() || c.path.rfind(prefix + "/", 0) == 0) : c.path == prefix;
            if (!inside)
                continue;
            (c.group == model::git::Area::Staged ? staged : changed).push_back(c.path);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(ICON_FA_CODE_BRANCH "  Git")) {
            if (ImGui::MenuItem(ICON_FA_PLUS "  Stage", nullptr, false, !changed.empty()))
                git.stage(changed);
            if (ImGui::MenuItem(ICON_FA_MINUS "  Unstage", nullptr, false, !staged.empty()))
                git.unstage(staged);
            if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Discard Changes...", nullptr, false, !changed.empty()))
                explorerConfirm("Discard changes?",
                                "Discard the changes to " + (changed.size() == 1 ? changed[0] : std::to_string(changed.size()) + " files") +
                                    "? This cannot be undone.",
                                "Discard", [this, changed]() { vm_.git().discard(changed); });
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CLOCK_ROTATE_LEFT "  File History", nullptr, false, !root))
                vm_.gitFileHistoryRequested.emit(prefix);
            if (!folder && ImGui::MenuItem(ICON_FA_USER_PEN "  Blame")) {
                vm_.openFile(path);
                if (const model::Document* doc = vm_.activeDocument())
                    git.toggleBlame(doc->path, doc->buffer.text());
            }
            ImGui::EndMenu();
        }
    }
    if (!root) {
        ImGui::Separator();
        ImGui::BeginDisabled(!editable);
        if (ImGui::MenuItem(ICON_FA_PEN "  Rename..."))
            explorerAsk("Rename " + name, "New name", item.name, "Rename",
                        [this, path](const std::string& n) { vm_.renamePath(path, n); });
        if (ImGui::MenuItem(ICON_FA_TRASH "  Delete...")) {
            int unsaved = 0;
            for (const auto& d : vm_.documents())
                if (d->modified && (d->path == path || d->path.rfind(path + "/", 0) == 0))
                    ++unsaved;
#if defined(_WIN32)
            std::string text = "Move " + path + " to the Recycle Bin?";
#else
            std::string text = "Delete " + path + (folder ? " and everything in it" : "") + "? This cannot be undone.";
#endif
            if (unsaved)
                text += " Its unsaved changes in " + std::to_string(unsaved) + " open document(s) are lost.";
            explorerConfirm("Delete " + name + "?", text, "Delete", [this, path]() { vm_.deletePath(path); });
        }
        ImGui::EndDisabled();
    }
    if (root && vm_.folderOpen()) {
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_FOLDER_CLOSED "  Close Folder"))
            vm_.requestCloseFolder();
    }
    if (!editable)
        ImGui::TextDisabled("Open a folder to create, rename or delete files");
}

void MainView::explorerAsk(std::string title, std::string hint, std::string value, std::string button,
                           std::function<void(const std::string&)> action)
{
    explorerPrompt_ = {std::move(title), std::move(hint), std::move(value), std::move(button), false, std::move(action), {}};
    openExplorerPrompt_ = true;
}

void MainView::explorerConfirm(std::string title, std::string text, std::string button, std::function<void()> action)
{
    explorerPrompt_ = {std::move(title), std::move(text), {}, std::move(button), true, {}, std::move(action)};
    openExplorerPrompt_ = true;
}

void MainView::renderExplorerPrompt()
{
    if (openExplorerPrompt_) {
        ImGui::OpenPopup("##explorerPrompt");
        openExplorerPrompt_ = false;
    }
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(420.0f * s_, 0.0f)));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("##explorerPrompt", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    ExplorerPrompt& p = explorerPrompt_;
    bool accept = false;
    if (p.confirm) {
        ImGui::TextColored(col(palette_.severityWarning), ICON_FA_TRIANGLE_EXCLAMATION "  %s", p.title.c_str());
        ImGui::Spacing();
        ImGui::TextWrapped("%s", p.hint.c_str());
    } else {
        ImGui::TextUnformatted(p.title.c_str());
        ImGui::Spacing();
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-1.0f);
        accept = ImGui::InputTextWithHint("##value", p.hint.c_str(), &p.value, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        registerLastItem("explorer-prompt:value", "textbox", p.title, nullptr);
    }
    ImGui::Spacing();
    const float w = 100.0f * s_;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 2.0f * w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(!p.confirm && p.value.empty());
    if (p.confirm)
        ImGui::PushStyleColor(ImGuiCol_Button, col(withAlpha(palette_.severityError, 170)));
    accept = ImGui::Button(p.button.c_str(), ImVec2(w, 0.0f)) || (accept && !p.value.empty());
    if (p.confirm)
        ImGui::PopStyleColor();
    ImGui::EndDisabled();
    registerLastItem("explorer-prompt:ok", "button", p.button, nullptr);
    ImGui::SameLine();
    const bool cancel = ImGui::Button("Cancel", ImVec2(w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape);
    registerLastItem("explorer-prompt:cancel", "button", "Cancel", nullptr);
    if (accept) {
        if (p.confirm && p.onConfirm)
            p.onConfirm();
        else if (!p.confirm && p.onValue)
            p.onValue(p.value);
    }
    if (accept || cancel)
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
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

    // Right-hand segments, Notepad++ style: short (symbols and numbers), the full wording on hover.
    // When the bar is too narrow the least important ones are left out rather than drawn over
    // the branch and the message.
    struct Segment {
        std::string text;    // what the bar shows
        std::string tooltip; // what hovering says
        const char* id;      // ImGui id; also the popup for the clickable ones ("##eol"), else plain
        int priority;        // higher stays longer when space runs out
        bool popup = false;
        CommandId click = CommandId::Count; // a command run on click, if any
    };
    std::vector<Segment> segments;
    if (vm_.developerMode.get()) {
        // What the process holds (refreshed once a second): a number that climbs while the app
        // idles points at a leak (see os::ProcessStats, docs/MEMORY.md).
        const double now = ImGui::GetTime();
        if (now - resourceTextAt_ >= 1.0 || resourceText_.empty()) {
            resourceTextAt_ = now;
            const os::ProcessStats p = os::processStats();
            const ImGuiContext& g = *GImGui;
            const os::mem::Stats heap = os::mem::stats();
            auto mb = [](unsigned long long bytes) {
                char t[32];
                std::snprintf(t, sizeof(t), bytes >= (100ull << 20) ? "%.0fM" : "%.1fM", double(bytes) / double(1 << 20));
                return std::string(t);
            };
            // Everything the process holds: its private memory and the shared memory it wrote.
            resourceText_ = std::string(ICON_FA_MEMORY " ") + mb(p.privateBytes + p.sharedBytes) + "  " ICON_FA_CUBES " " + mb(heap.liveBytes);
            char tip[768];
            std::snprintf(tip, sizeof(tip),
                          "Process memory (developer mode, every second)\n\n"
                          "Private memory: %llu KB\nShared memory: %llu KB\nResident: %llu KB\nC++ heap: %llu KB in %llu blocks\n"
                          "malloc: %llu KB in use, %llu KB held\nGraphics: %llu KB\nCompressed: %llu KB\n"
                          "Handles: %d\nGDI objects: %d\nUSER objects: %d\nThreads: %d\n"
                          "ImGui: %d allocations, %d windows\n\n"
                          "A number that climbs while the app is idle points at a leak.",
                          static_cast<unsigned long long>(p.privateBytes >> 10), static_cast<unsigned long long>(p.sharedBytes >> 10),
                          static_cast<unsigned long long>(p.residentBytes >> 10),
                          static_cast<unsigned long long>(heap.liveBytes >> 10), static_cast<unsigned long long>(heap.liveBlocks),
                          static_cast<unsigned long long>(p.mallocInUseBytes >> 10), static_cast<unsigned long long>(p.mallocHeldBytes >> 10),
                          static_cast<unsigned long long>(p.graphicsBytes >> 10), static_cast<unsigned long long>(p.compressedBytes >> 10),
                          p.handles, p.gdiObjects, p.userObjects, p.threads,
                          g.DebugAllocInfo.TotalAllocCount - g.DebugAllocInfo.TotalFreeCount, g.Windows.Size);
            resourceTip_ = tip;
        }
        segments.push_back({resourceText_, resourceTip_, "##resources", 1});
    }
    if (vm_.agent().busy.get())
        segments.push_back({ICON_FA_CIRCLE_NOTCH, "The agent is working", "##agentBusy", 9});
    if (vm_.recording.get())
        segments.push_back({ICON_FA_CIRCLE_DOT " REC", "Recording a macro", "##recording", 9});
    const model::Document* doc = vm_.activeDocument();
    if (doc) {
        const int eolLength = doc->eol == model::EolMode::CrLf ? 2 : 1;
        const std::string& line = doc->buffer.line(doc->caret.line);
        const int column = model::codePointCount(std::string_view(line).substr(0, size_t(doc->caret.column))) + 1;
        // With several carets: every selection's characters (line breaks between them not counted).
        int selChars = 0;
        int selLines = 0;
        auto count = [&](const model::Selection& sel) {
            if (sel.empty())
                return;
            selChars += int(model::codePointCount(doc->buffer.text(sel.start(), sel.end())));
            selLines += sel.end().line - sel.start().line + 1;
        };
        count(doc->primary());
        for (const model::Selection& sel : doc->cursors)
            count(sel);
        std::string language = model::languageName(doc->language);
        if (language.size() > 5 && language.compare(language.size() - 5, 5, " file") == 0)
            language.resize(language.size() - 5); // "Markdown file" -> "Markdown"
        segments.push_back({language, std::string(model::languageName(doc->language)) + "\nClick to choose the language",
                            "##language", 7, true});
        const int lines = doc->buffer.lineCount();
        const int chars = int(doc->buffer.characterCount(eolLength));
        segments.push_back({std::string(ICON_FA_FILE_LINES " ") + std::to_string(lines),
                            std::to_string(lines) + " lines, " + std::to_string(chars) + " characters", "##length", 4});
        std::string caret = std::to_string(doc->caret.line + 1) + ":" + std::to_string(column);
        if (selChars > 0)
            caret += " (" + std::to_string(selChars) + ")";
        if (doc->multiCursor())
            caret += " +" + std::to_string(doc->cursors.size()); // more carets (Escape: back to one)
        segments.push_back({caret,
                            "Line " + std::to_string(doc->caret.line + 1) + ", column " + std::to_string(column) +
                                (selChars > 0 ? "\n" + std::to_string(selChars) + " characters selected on " + std::to_string(selLines) +
                                                    (selLines == 1 ? " line" : " lines")
                                              : std::string()) +
                                (doc->multiCursor() ? "\n" + std::to_string(doc->cursors.size() + 1) + " carets (Escape: back to one)" : std::string()) +
                                "\nClick to go to a line (" + commandInfo(CommandId::GoToLine).shortcut + ")",
                            "##caret", 10, false, CommandId::GoToLine});
        segments.push_back({std::string(doc->indent.tabs ? ICON_FA_ARROW_RIGHT_TO_BRACKET " " : ICON_FA_INDENT " ") + std::to_string(doc->indent.width),
                            std::string(doc->indent.tabs ? "Indent with tabs, " : "Indent with spaces, ") + std::to_string(doc->indent.width) +
                                " wide\nClick to change",
                            "##indent", 5, true});
        segments.push_back({model::eolShortName(doc->eol), std::string("Line endings: ") + model::eolName(doc->eol) + "\nClick to change",
                            "##eol", 6, true});
        segments.push_back({model::encodingName(doc->encoding), std::string("Encoding: ") + model::encodingName(doc->encoding) + "\nClick to change",
                            "##encoding", 6, true});
    }
    const int zoom = int(std::lround(vm_.zoom.get() * 100.0f));
    if (zoom != 100) // only when it is not the usual size
        segments.push_back({std::to_string(zoom) + "%", "Editor zoom " + std::to_string(zoom) + "%\nCtrl+wheel to change", "##zoom", 3});
    segments.push_back({"INS", "Insert mode: typing inserts text", "##ins", 2});

    // Tooltips of the bar open above it (below the pointer they would leave the window), on the
    // side of the pointer that has room.
    auto statusTip = [&](const std::string& text) {
        const float mouseX = ImGui::GetIO().MousePos.x;
        const bool rightHalf = mouseX > origin.x + width * 0.5f;
        ImGui::SetNextWindowPos(ImVec2(mouseX, origin.y - 4.0f * s_), ImGuiCond_Always, ImVec2(rightHalf ? 1.0f : 0.0f, 1.0f));
        ImGui::SetTooltip("%s", text.c_str());
    };

    // Fit: drop the least important segments until the rest leave room for the badges and a
    // little of the message.
    const float pad = 10.0f * s_;
    auto widthOf = [&](const Segment& s) { return ImGui::CalcTextSize(s.text.c_str()).x + pad * 2.0f; };
    const float room = origin.x + width - 8.0f * s_ - (messageX + 60.0f * s_);
    for (;;) {
        float total = 0.0f;
        for (const Segment& s : segments)
            total += widthOf(s);
        if (total <= room || segments.empty())
            break;
        auto least = std::min_element(segments.begin(), segments.end(), [](const Segment& a, const Segment& b) { return a.priority < b.priority; });
        segments.erase(least);
    }

    float x = origin.x + width - 8.0f * s_;
    const char* openPopup = nullptr;
    ImVec2 popupPos;
    for (auto it = segments.rbegin(); it != segments.rend(); ++it) {
        const float w = widthOf(*it);
        x -= w;
        const ImVec2 min(x, origin.y + 1.0f), max(x + w, origin.y + h);
        ImGui::SetCursorScreenPos(min);
        ImGui::PushID(it->id);
        const bool clicked = ImGui::InvisibleButton("##segment", ImVec2(max.x - min.x, max.y - min.y));
        const bool hovered = ImGui::IsItemHovered();
        const bool clickable = it->popup || it->click != CommandId::Count;
        if (hovered && !it->tooltip.empty())
            statusTip(it->tooltip);
        ImGui::PopID();
        if (clicked && it->popup) {
            openPopup = it->id;
            popupPos = ImVec2(min.x, origin.y);
        } else if (clicked && it->click != CommandId::Count) {
            vm_.execute(it->click);
        }
        if (hovered && clickable)
            dl->AddRectFilled(min, max, palette_.hover);
        const bool agentSegment = std::strcmp(it->id, "##agentBusy") == 0;
        dl->AddText(ImVec2(x + pad, textY), agentSegment ? palette_.modeAgent : hovered ? palette_.text : palette_.textDim,
                    it->text.c_str());
        dl->AddLine(ImVec2(x, origin.y + 5.0f * s_), ImVec2(x, origin.y + h - 5.0f * s_), palette_.border);
    }

    // Status message between the badge and the segments.
    {
        const ImVec2 mMin(messageX + 8.0f * s_, origin.y), mMax(x - 4.0f * s_, origin.y + h);
        const std::string& message = vm_.statusText.get();
        dl->PushClipRect(mMin, mMax, true);
        dl->AddText(ImVec2(messageX + 10.0f * s_, textY), palette_.textDim, message.c_str());
        dl->PopClipRect();
        // The whole message on hover when it does not fit.
        if (mMax.x > mMin.x && ImGui::IsMouseHoveringRect(mMin, mMax) && ImGui::IsWindowHovered() &&
            ImGui::CalcTextSize(message.c_str()).x > mMax.x - mMin.x - 4.0f * s_)
            statusTip(message);
    }
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
    fit::constrainNextWindow();
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
        focusedEditor().focus();
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
    fit::constrainNextWindow();
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
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Preferences", &open, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (registerWindowClose("dialog:preferences:close", "Close Preferences"))
        ImGui::CloseCurrentPopup();
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
    ImGui::Spacing();
    ImGui::TextDisabled("EXPLORER");
    toggle("Show hidden files and folders", vm_.showHiddenFiles);
    ImGui::SetItemTooltip("%s", "Dot files and folders such as .github and .gitignore (and, on Windows, hidden ones). The .git folder never shows.");
    ImGui::Spacing();
    ImGui::TextDisabled("INDENTATION");
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
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_SLIDERS "  IntelliSense..."))
        openIntellisenseSettings_ = true;
    ImGui::SetItemTooltip("Suggestions, the model that writes code from comments (local or online), C/C++ standards and include folders");
    ImGui::Spacing();
    ImGui::TextDisabled(vm_.settingsPath().empty() ? "Settings last for this session." : "Settings are saved automatically.");
    if (ImGui::Button("Close", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MainView::renderIntellisenseSettingsPopup()
{
    viewmodel::IntellisenseViewModel& is = vm_.intellisense();
    viewmodel::CodeAssistant& assistant = vm_.codeAssistant();
    if (openIntellisenseSettings_) {
        ImGui::OpenPopup("IntelliSense Settings");
        openIntellisenseSettings_ = false;
        editClaudeModel_ = assistant.claudeModel.get();
        editCommand_ = assistant.command.get();
        editIncludeFolders_ = is.includeFolders.get();
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("IntelliSense Settings", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!editCommand_.empty() || !editClaudeModel_.empty() || !editIncludeFolders_.empty()) { // closed: the fields go
            os::mem::release(editClaudeModel_);
            os::mem::release(editCommand_);
            os::mem::release(editIncludeFolders_);
        }
        return;
    }
    bool close = registerWindowClose("dialog:intellisense:close", "Close IntelliSense Settings");
    auto toggle = [](const char* label, viewmodel::Property<bool>& property) {
        bool value = property.get();
        if (ImGui::Checkbox(label, &value))
            property.set(value);
    };
    const float fieldW = 300.0f * s_;
    // Help text at a fixed width: in a window that sizes itself, TextWrapped would wrap at the
    // window's own (narrow) width and make it very tall.
    auto help = [this](const char* text) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(460.0f * s_, ImGui::GetMainViewport()->WorkSize.x - 80.0f * s_));
        ImGui::TextDisabled("%s", text);
        ImGui::PopTextWrapPos();
    };

    ImGui::TextDisabled("SUGGESTIONS");
    toggle("Suggest names while typing", is.autoComplete);
    ImGui::SetItemTooltip("After two letters, \".\", \"->\" or \"::\". Ctrl+Space shows them anyway.");
    toggle("Code suggestions from comments", is.aiSuggestions);
    ImGui::SetItemTooltip("Write a comment saying what the code should do, then press Enter and wait a moment, or press "
                          "Ctrl+Shift+Space. The model below writes the code; Enter or Tab inserts it, Esc stops it.");

    ImGui::Spacing();
    ImGui::TextDisabled("THE MODEL THAT WRITES CODE");
    {
        const char* const labels[] = {viewmodel::CodeAssistant::sourceName(viewmodel::CodeAssistant::Source::Local),
                                      viewmodel::CodeAssistant::sourceName(viewmodel::CodeAssistant::Source::Claude),
                                      viewmodel::CodeAssistant::sourceName(viewmodel::CodeAssistant::Source::Command)};
        int source = std::clamp(assistant.source.get(), 0, 2);
        ImGui::SetNextItemWidth(fieldW);
        if (ImGui::Combo("Model##codeModel", &source, labels, 3))
            assistant.source.set(source);
        switch (viewmodel::CodeAssistant::Source(source)) {
        case viewmodel::CodeAssistant::Source::Local:
            help("Runs on this device, offline. The same model as the agent's local answers.");
            if (ImGui::Button(ICON_FA_MICROCHIP "  Local model set-up..."))
                vm_.execute(CommandId::LocalModel);
            break;
        case viewmodel::CodeAssistant::Source::Claude:
            ImGui::SetNextItemWidth(fieldW);
            if (ImGui::InputTextWithHint("Claude model##claudeModel", "haiku, sonnet, opus (empty: Claude Code's own)", &editClaudeModel_))
                assistant.claudeModel.set(editClaudeModel_);
            help("Online, through Claude Code (set up in the Agent panel). The comment, the code around it and the "
                                     "declarations it uses are sent with each request.");
            break;
        case viewmodel::CodeAssistant::Source::Command:
            ImGui::SetNextItemWidth(fieldW);
            if (ImGui::InputTextWithHint("Command##codeCommand", "ollama run qwen2.5-coder:7b", &editCommand_))
                assistant.command.set(editCommand_);
            help("Any program, local or online: it gets the prompt on its standard input and prints the code "
                                     "(e.g. ollama run ..., llm -m ..., a script calling an API). It runs in the workspace folder.");
            break;
        }
        std::string why;
        const bool ready = assistant.ready(&why);
        if (ready)
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.iconGreen), ICON_FA_CIRCLE_CHECK "  Ready");
        else
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(palette_.severityWarning), ICON_FA_TRIANGLE_EXCLAMATION "  Not ready: %s", why.c_str());
    }

    ImGui::Spacing();
    ImGui::TextDisabled("C AND C++");
    auto standardCombo = [&](const char* label, viewmodel::Property<int>& property, bool c) {
        size_t count = 0;
        const int* years = c ? model::cStandards(count) : model::cppStandards(count);
        // "From the project (C++20, CMakeLists.txt)" while it is in use; plain when another is chosen.
        char automatic[96];
        if (property.get() == 0)
            std::snprintf(automatic, sizeof(automatic), "From the project (%s, %s)",
                          model::standardName(c ? is.cStandardInUse() : is.cppStandardInUse(), c), is.standardSource(c).c_str());
        else
            std::snprintf(automatic, sizeof(automatic), "From the project");
        ImGui::SetNextItemWidth(fieldW);
        if (ImGui::BeginCombo(label, property.get() ? model::standardName(property.get(), c) : automatic)) {
            if (ImGui::Selectable(automatic, property.get() == 0))
                property.set(0);
            for (size_t i = 0; i < count; ++i)
                if (ImGui::Selectable(model::standardName(years[i], c), property.get() == years[i]))
                    property.set(years[i]);
            ImGui::EndCombo();
        }
    };
    standardCombo("C++ standard", is.cppStandard, false);
    ImGui::SetItemTooltip("Which library names and keywords are suggested in C++ files. From the project: CMAKE_CXX_STANDARD, "
                          "cxx_std_NN or -std=c++NN in CMakeLists.txt, meson.build or a Makefile; else C++17.");
    standardCombo("C standard", is.cStandard, true);
    ImGui::SetItemTooltip("The same for .c files (CMAKE_C_STANDARD, c_std_NN, -std=cNN; else C17).");
    ImGui::SetNextItemWidth(fieldW);
    if (ImGui::InputTextWithHint("Include folders", "include;src;third_party/lib/include", &editIncludeFolders_))
        is.includeFolders.set(editIncludeFolders_);
    ImGui::SetItemTooltip("Workspace folders, separated by ';', where #include looks for headers (after the file's own "
                          "folder; include/ and src/ are always tried). The names of included headers are suggested first, "
                          "and a name from a header that is not included says which #include it needs.");

    ImGui::Spacing();
    ImGui::TextDisabled("INDEX");
    if (is.indexing.get())
        ImGui::TextUnformatted("Reading the workspace...");
    else if (is.indexReady())
        ImGui::Text("%zu names from %zu files", is.index().size(), is.index().fileCount());
    else
        ImGui::TextDisabled("Read on first use");
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_ROTATE "  Rebuild"))
        vm_.execute(CommandId::RebuildSymbolIndex);

    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        close = true;
    if (close)
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
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(820.0f * s_, 560.0f * s_)), ImGuiCond_Appearing);
    bool open = true;
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Syntax Colours", &open))
        return;
    if (registerWindowClose("dialog:syntax-colours:close", "Close Syntax Colours"))
        ImGui::CloseCurrentPopup();
    const bool dark = vm_.darkTheme.get();
    const auto& all = model::languages();
    const bool allLanguages = syntaxColorsLanguage_ < 0 || syntaxColorsLanguage_ >= int(all.size());
    const model::LanguageSpec* spec = allLanguages ? nullptr : &all[size_t(syntaxColorsLanguage_)];
    const std::string key = spec ? spec->key : viewmodel::NotepadViewModel::kAllLanguages;
    const float footerH = ImGui::GetFrameHeightWithSpacing() + 8.0f * s_;

    // Languages.
    ImGui::BeginChild("##languages", ImVec2(std::min(190.0f * s_, ImGui::GetContentRegionAvail().x * 0.36f), -footerH),
                      ImGuiChildFlags_Borders);
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
    const bool roomy = ImGui::GetContentRegionAvail().x > 420.0f * s_; // measured before the table
    if (ImGui::BeginTable("##tokenTable", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("Colour", ImGuiTableColumnFlags_WidthFixed, 36.0f * s_);
        ImGui::TableSetupColumn("Token", ImGuiTableColumnFlags_WidthStretch);
        // Where the colour comes from: a fixed column when there is room, otherwise it shares the space.
        ImGui::TableSetupColumn("Source", roomy ? ImGuiTableColumnFlags_WidthFixed : ImGuiTableColumnFlags_WidthStretch, roomy ? 200.0f * s_ : 0.0f);
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

bool MainView::navigateBack()
{
    if (engineLab_.isOpen()) {
        engineLab_.close();
        return true;
    }
    return false;
}

void MainView::renderAboutPopup()
{
    if (openAbout_) {
        ImGui::OpenPopup("About EasyNotepad");
        openAbout_ = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("About EasyNotepad", &open, ImGuiWindowFlags_AlwaysAutoResize))
        return;
    if (registerWindowClose("dialog:about:close", "Close About"))
        ImGui::CloseCurrentPopup();
    // Lines wrap at a readable width, or the screen's on a phone.
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + std::min(460.0f * s_, ImGui::GetMainViewport()->WorkSize.x - 60.0f * s_));
    ImGui::TextColored(col(palette_.accent), ICON_FA_PEN_TO_SQUARE);
    ImGui::SameLine();
    ImGui::Text("EasyNotepad %s", ENP_VERSION_FULL);
    // Seven taps on the version turn on developer mode (as on Android): nothing shows until then.
    if (ImGui::IsItemClicked() && !vm_.developerMode.get()) {
        if (++versionTaps_ >= 7) {
            vm_.developerMode.set(true);
            vm_.statusText.set("Developer mode is on: Engine Lab is in Help.");
        } else if (versionTaps_ >= 3) {
            vm_.statusText.set(std::to_string(7 - versionTaps_) + " more taps to turn on developer mode.");
        }
    }
    registerLastItem("dialog:about:version", "text", std::string("EasyNotepad ") + ENP_VERSION_FULL, [this]() {
        if (++versionTaps_ >= 7)
            vm_.developerMode.set(true);
    });
    ImGui::TextDisabled(ENP_RELEASE ? "Release %s, commit %s" : "Development build of %s, commit %s", ENP_VERSION, ENP_COMMIT);
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_COPY " Copy")) // for a bug report: the build, the system
        ImGui::SetClipboardText((std::string("EasyNotepad ") + ENP_VERSION_FULL + " (" + os::osName() + " " + os::archName() + ")").c_str());
    registerLastItem("dialog:about:copy-version", "button", "Copy the version", nullptr);
    ImGui::TextDisabled("Native text editor with Agent and Code workspaces.");
    ImGui::Separator();
    ImGui::Text("Platform: %s %s", os::osName(), os::archName());
    ImGui::Text("Dear ImGui %s", IMGUI_VERSION);
    if (!rendererInfo_.empty())
        ImGui::TextWrapped("Renderer: %s", rendererInfo_.c_str());
    ImGui::Spacing();
    if (vm_.developerMode.get()) {
        ImGui::Spacing();
        ImGui::TextColored(col(palette_.accent), ICON_FA_FLASK "  Developer mode is on.");
        if (ImGui::SmallButton("Engine Lab")) {
            ImGui::CloseCurrentPopup();
            vm_.execute(CommandId::EngineLab);
        }
        registerLastItem("dialog:about:engine-lab", "button", "Engine Lab", [this]() { vm_.execute(CommandId::EngineLab); });
        ImGui::SameLine();
        if (ImGui::SmallButton("Turn off")) {
            vm_.developerMode.set(false);
            versionTaps_ = 0;
            engineLab_.close();
        }
    }
    ImGui::PopTextWrapPos();
    if (ImGui::Button("OK", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void MainView::renderShortcutsDialog()
{
    auto lower = [](std::string s) {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    if (openShortcuts_) {
        ImGui::OpenPopup("Keyboard Shortcuts");
        openShortcuts_ = false;
        os::mem::release(shortcutsFilter_);
        os::mem::release(shortcutsNeedle_);
        // One row per command, its keys together (Redo: Ctrl+Y, Ctrl+Shift+Z), in the table's
        // order; then what the mouse does.
        os::mem::release(shortcutRows_);
        std::vector<CommandId> seen;
        for (const Binding& b : keyBindings()) {
            auto it = std::find(seen.begin(), seen.end(), b.id);
            if (it != seen.end()) {
                shortcutRows_[size_t(it - seen.begin())].keys += ",  " + chordLabel(b.chord);
                continue;
            }
            seen.push_back(b.id);
            shortcutRows_.push_back({chordLabel(b.chord), {}, commandInfo(b.id).label, commandInfo(b.id).tooltip});
        }
        shortcutRows_.push_back({"Ctrl+Click", {}, "Definition", "Go to where the name under the mouse is declared"});
        shortcutRows_.push_back({"Shift+Alt+Drag", {}, "Column Select", "Select a box of text, one caret per line (or drag with the middle button)"});
        shortcutRows_.push_back({"Double-click", {}, "Edit Markdown", "In rendered Markdown: edit the text at that place"});
        for (ShortcutRow& row : shortcutRows_)
            row.search = lower(row.keys + '\n' + row.command + '\n' + row.what);
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(760.0f * s_, 580.0f * s_)), ImGuiCond_Appearing);
    bool open = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s_, 12.0f * s_));
    const bool shown = ImGui::BeginPopupModal("Keyboard Shortcuts", &open);
    ImGui::PopStyleVar();
    if (!shown) {
        if (shortcutRows_.capacity() > 0) { // closed: what it held goes
            os::mem::release(shortcutRows_);
            os::mem::release(shortcutsFilter_);
            os::mem::release(shortcutsNeedle_);
        }
        return;
    }
    if (registerWindowClose("dialog:shortcuts:close", "Close Keyboard Shortcuts"))
        ImGui::CloseCurrentPopup();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Key or command", &shortcutsFilter_))
        shortcutsNeedle_ = lower(shortcutsFilter_);
    registerLastItem("dialog:shortcuts:filter", "textbox", "Filter shortcuts", nullptr);

    const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp;
    const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y;
    if (ImGui::BeginTable("##shortcuts", 3, flags, ImVec2(0.0f, -footer))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Keys", ImGuiTableColumnFlags_WidthFixed, 200.0f * s_);
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthFixed, 120.0f * s_);
        ImGui::TableSetupColumn("What it does", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const ShortcutRow& row : shortcutRows_) {
            if (!shortcutsNeedle_.empty() && row.search.find(shortcutsNeedle_) == std::string::npos)
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.accent));
            ImGui::TextWrapped("%s", row.keys.c_str()); // several keys for one command wrap within the column
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row.command);
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.textDim));
            ImGui::TextWrapped("%s", row.what);
            ImGui::PopStyleColor();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Close", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

} // namespace enp::view
