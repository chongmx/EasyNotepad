#include "enp/view/MainView.h"

#include "enp/os/Platform.h"
#include "enp/view/Icons.h"
#include "enp/viewmodel/DummyData.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cmath>

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
      agentPanel_(vm, fonts, palette_, uiScale), reviewPanel_(vm, fonts, palette_, uiScale),
      explorerWidth_(230.0f * uiScale), agentSideWidth_(360.0f * uiScale), reviewHeight_(220.0f * uiScale),
      planWidth_(270.0f * uiScale), conversationWidth_(430.0f * uiScale)
{
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

void MainView::render()
{
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
    renderPreferencesPopup();
    renderAboutPopup();
    handleCaptionMouse();
    ImGui::End();
}

// ---- Input --------------------------------------------------------------------------------------

void MainView::handleShortcuts()
{
    struct Binding {
        ImGuiKeyChord chord;
        CommandId id;
        bool editing; // only while no text field has focus
    };
    using C = CommandId;
    static const Binding bindings[] = {
        {ImGuiMod_Ctrl | ImGuiKey_N, C::NewFile, false},
        {ImGuiMod_Ctrl | ImGuiKey_O, C::OpenFile, false},
        {ImGuiMod_Ctrl | ImGuiKey_S, C::Save, false},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, C::SaveAll, false},
        {ImGuiMod_Ctrl | ImGuiKey_W, C::CloseFile, false},
        {ImGuiMod_Ctrl | ImGuiKey_X, C::Cut, true},
        {ImGuiMod_Ctrl | ImGuiKey_C, C::Copy, true},
        {ImGuiMod_Ctrl | ImGuiKey_V, C::Paste, true},
        {ImGuiMod_Ctrl | ImGuiKey_Z, C::Undo, true},
        {ImGuiMod_Ctrl | ImGuiKey_Y, C::Redo, true},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, C::Redo, true},
        {ImGuiMod_Ctrl | ImGuiKey_A, C::SelectAll, true},
        {ImGuiMod_Ctrl | ImGuiKey_D, C::DuplicateLine, true},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_L, C::DeleteLine, true},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_UpArrow, C::MoveLineUp, true},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_DownArrow, C::MoveLineDown, true},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_U, C::UpperCase, true},
        {ImGuiMod_Ctrl | ImGuiKey_U, C::LowerCase, true},
        {ImGuiMod_Ctrl | ImGuiKey_Q, C::ToggleComment, true},
        {ImGuiMod_Ctrl | ImGuiKey_F, C::Find, false},
        {ImGuiMod_Ctrl | ImGuiKey_H, C::Replace, false},
        {ImGuiKey_F3, C::FindNext, false},
        {ImGuiMod_Shift | ImGuiKey_F3, C::FindPrevious, false},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F, C::FindInFiles, false},
        {ImGuiMod_Ctrl | ImGuiKey_G, C::GoToLine, false},
        {ImGuiMod_Ctrl | ImGuiKey_F2, C::ToggleBookmark, false},
        {ImGuiKey_F2, C::NextBookmark, false},
        {ImGuiMod_Shift | ImGuiKey_F2, C::PreviousBookmark, false},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_E, C::ToggleExplorer, false},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_R, C::ToggleReviewPanel, false},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I, C::ToggleAgentPanel, false},
        {ImGuiMod_Alt | ImGuiKey_Z, C::ToggleWordWrap, false},
        {ImGuiMod_Ctrl | ImGuiKey_Equal, C::ZoomIn, false},
        {ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, C::ZoomIn, false},
        {ImGuiMod_Ctrl | ImGuiKey_Minus, C::ZoomOut, false},
        {ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, C::ZoomOut, false},
        {ImGuiMod_Ctrl | ImGuiKey_0, C::ZoomReset, false},
        {ImGuiKey_F7, C::RunLint, false},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_F7, C::RunAllReviews, false},
        {ImGuiKey_F8, C::NextFinding, false},
        {ImGuiMod_Shift | ImGuiKey_F8, C::PreviousFinding, false},
        {ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_M, C::AddReviewComment, false},
        {ImGuiMod_Ctrl | ImGuiKey_I, C::AskAgent, false},
        {ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_M, C::ToggleWorkspaceMode, false},
    };
    const bool typing = ImGui::GetIO().WantTextInput;
    for (const Binding& b : bindings) {
        if (b.editing && typing)
            continue;
        if (ImGui::Shortcut(b.chord, ImGuiInputFlags_RouteGlobal | (b.editing ? 0 : ImGuiInputFlags_Repeat)))
            vm_.execute(b.id);
    }
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
        const CommandId items[] = {CommandId::NewFile, CommandId::OpenFile, CommandId::Save, CommandId::SaveAll,
                                   CommandId::CloseFile};
        for (CommandId id : items) {
            const std::string label = std::string(commandIcon(id)) + "  " + commandInfo(id).tooltip;
            if (ImGui::MenuItem(label.c_str(), commandInfo(id).shortcut, false, vm_.command(id).canExecute()))
                vm_.execute(id);
        }
        ImGui::Separator();
        if (ImGui::BeginMenu(ICON_FA_CLOCK_ROTATE_LEFT "  Recent files")) {
            for (const char* path : {"src/main.cpp", "src/TextUtils.cpp", "scripts/build_index.py", "docs/README.md"})
                if (ImGui::MenuItem(path))
                    vm_.openFile(path);
            ImGui::EndMenu();
        }
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
        frame_->setDragRegions(dragRegions_);

    ImGui::SetCursorScreenPos(ImVec2(rightX, y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * s_);
    const bool agent = vm_.workspace.get() == WorkspaceMode::Agent;
    ImGui::PushStyleColor(ImGuiCol_Text, agent ? col(palette_.modeAgent) : col(palette_.textDim));
    if (flatButton(ICON_FA_ROBOT " Agent", "Agent workspace: describe the goal, review the changes", true, agent))
        vm_.execute(CommandId::SwitchToAgentMode);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 2.0f * s_);
    ImGui::PushStyleColor(ImGuiCol_Text, !agent ? col(palette_.modeCode) : col(palette_.textDim));
    if (flatButton(ICON_FA_CODE " Code", "Code workspace: manual editing", true, !agent))
        vm_.execute(CommandId::SwitchToCodeMode);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::SameLine(0.0f, 12.0f * s_);
    if (flatButton(vm_.darkTheme.get() ? ICON_FA_SUN : ICON_FA_MOON, "Toggle theme"))
        vm_.execute(CommandId::ToggleTheme);
    ImGui::SameLine(0.0f, 2.0f * s_);
    if (flatButton(ICON_FA_GEAR, "Preferences"))
        vm_.execute(CommandId::Preferences);
    ImGui::SameLine(0.0f, 2.0f * s_);
    if (flatButton(ICON_FA_CIRCLE_QUESTION, "About EasyNotepad"))
        vm_.execute(CommandId::About);
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
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float plusW = ImGui::GetFrameHeight();
    const float avail = std::max(0.0f, x1 - x0 - plusW - 4.0f * s_);
    const float maxTabW = 200.0f * s_;
    const float tabW = docs.empty() ? 0.0f : std::min(maxTabW, avail / float(docs.size()));
    const float tabTop = y + 5.0f * s_;
    const float tabH = height - 5.0f * s_;
    int closeIndex = -1;

    ImGui::PushID("documentTabs");
    dl->PushClipRect(ImVec2(x0, y), ImVec2(x1, y + height), true);
    for (int i = 0; i < int(docs.size()); ++i) {
        const model::Document& doc = *docs[size_t(i)];
        const bool active = i == vm_.activeIndex.get();
        const float tx = x0 + float(i) * tabW;
        const ImVec2 min(tx, tabTop), max(tx + tabW - 2.0f * s_, tabTop + tabH);
        ImGui::PushID(doc.id);
        ImGui::SetCursorScreenPos(min);
        ImGui::SetNextItemAllowOverlap();
        if (ImGui::InvisibleButton("##tab", ImVec2(std::max(1.0f, max.x - min.x), tabH))) {
            vm_.activate(i);
            previewTabRequest_ = 1;
        }
        const bool hovered = ImGui::IsItemHovered();
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle))
            closeIndex = i;
        if (hovered)
            ImGui::SetTooltip("%s", doc.path.empty() ? doc.title.c_str() : doc.path.c_str());
        if (active) {
            dl->AddRectFilled(min, max, palette_.ribbonBg, kRounding * s_, ImDrawFlags_RoundTop);
            dl->AddLine(ImVec2(min.x + kRounding * s_, min.y + 1.0f), ImVec2(max.x - kRounding * s_, min.y + 1.0f),
                        palette_.accent, 2.0f * s_);
        } else if (hovered) {
            dl->AddRectFilled(min, max, palette_.hover, kRounding * s_, ImDrawFlags_RoundTop);
        }

        const float textY = min.y + (tabH - ImGui::GetTextLineHeight()) * 0.5f;
        const char* icon = fileIcon(doc.title);
        dl->AddText(ImVec2(min.x + 10.0f * s_, textY), active ? palette_.accent : palette_.textFaint, icon);
        const float closeSize = ImGui::GetTextLineHeight();
        const float textX = min.x + 10.0f * s_ + ImGui::CalcTextSize(icon).x + 6.0f * s_;
        dl->PushClipRect(ImVec2(textX, min.y), ImVec2(max.x - closeSize - 8.0f * s_, max.y), true);
        dl->AddText(ImVec2(textX, textY), active ? palette_.text : palette_.textDim, doc.title.c_str());
        dl->PopClipRect();

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
            dl->AddCircleFilled(center, 3.5f * s_, palette_.textDim);
        } else if (hovered || closeHovered || active) {
            const float r = closeSize * 0.22f;
            const ImU32 c = closeHovered ? palette_.text : palette_.textFaint;
            dl->AddLine(ImVec2(center.x - r, center.y - r), ImVec2(center.x + r, center.y + r), c, 1.5f * s_);
            dl->AddLine(ImVec2(center.x - r, center.y + r), ImVec2(center.x + r, center.y - r), c, 1.5f * s_);
        }
        ImGui::PopID();
    }
    dl->PopClipRect();

    ImGui::SetCursorScreenPos(ImVec2(x0 + float(docs.size()) * tabW + 2.0f * s_, y + (height - ImGui::GetFrameHeight()) * 0.5f));
    if (flatButton(ICON_FA_PLUS "##newTab", "New document (Ctrl+N)"))
        vm_.execute(CommandId::NewFile);
    const float end = ImGui::GetItemRectMax().x;
    ImGui::PopID();
    if (closeIndex >= 0)
        vm_.closeDocument(closeIndex);
    return end;
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
                frame_->close();
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
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, kRounding * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, col(title ? palette_.panelBg : palette_.editorBg));
    ImGui::PushStyleColor(ImGuiCol_Border, col(palette_.border));
    ImGui::BeginChild(id, size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(2);
    panelHasHeader_.push_back(title != nullptr);
    if (!title)
        return true;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetWindowPos();
    const float w = ImGui::GetWindowWidth();
    const float h = kPanelHeader * s_;
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), palette_.panelHeaderBg, kRounding * s_, ImDrawFlags_RoundTop);
    dl->AddLine(ImVec2(p.x, p.y + h), ImVec2(p.x + w, p.y + h), palette_.border);
    const float textY = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    dl->AddText(ImVec2(p.x + 10.0f * s_, textY), palette_.text, title);
    if (detail) {
        const float dw = ImGui::CalcTextSize(detail).x;
        dl->AddText(ImVec2(p.x + w - dw - 10.0f * s_, textY), palette_.textFaint, detail);
    }
    ImGui::SetCursorPos(ImVec2(0.0f, h + 1.0f));
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
}

// ---- Workspaces ---------------------------------------------------------------------------------

void MainView::renderReviewArea(float width)
{
    const viewmodel::ReviewViewModel& review = vm_.review();
    const std::string detail = std::to_string(review.findings().size()) + " findings";
    beginPanel("##reviewPanel", ICON_FA_CLIPBOARD_CHECK "  Review", ImVec2(width, reviewHeight_), detail.c_str());
    reviewPanel_.render(ImGui::GetContentRegionAvail());
    endPanel();
}

void MainView::renderCodeWorkspace()
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = kGap * s_;
    const bool review = vm_.showReviewPanel.get();
    reviewHeight_ = std::clamp(reviewHeight_, 90.0f * s_, std::max(90.0f * s_, avail.y - 160.0f * s_));
    const float topH = review ? avail.y - reviewHeight_ - gap : avail.y;

    if (vm_.showExplorer.get()) {
        beginPanel("##explorer", ICON_FA_SITEMAP "  Explorer", ImVec2(explorerWidth_, topH), viewmodel::dummy::kWorkspaceName);
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
    if (review) {
        horizontalSplitter("##splitReview", reviewHeight_, avail.x, 90.0f * s_, avail.y - 160.0f * s_);
        renderReviewArea(avail.x);
    }
}

void MainView::renderAgentWorkspace()
{
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float gap = kGap * s_;
    const bool review = vm_.showReviewPanel.get();
    reviewHeight_ = std::clamp(reviewHeight_, 90.0f * s_, std::max(90.0f * s_, avail.y - 160.0f * s_));
    const float topH = review ? avail.y - reviewHeight_ - gap : avail.y;

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

    if (review) {
        horizontalSplitter("##splitReview", reviewHeight_, avail.x, 90.0f * s_, avail.y - 160.0f * s_);
        renderReviewArea(avail.x);
    }
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
    else
        editor_.render(rest);
}

void MainView::renderExplorerItem(const viewmodel::ExplorerItem& item)
{
    const model::Document* doc = vm_.activeDocument();
    if (item.isDirectory) {
        const std::string label = std::string(ICON_FA_FOLDER "  ") + item.name + "##" + item.path;
        ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.text));
        const bool open = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanAvailWidth);
        ImGui::PopStyleColor();
        if (open) {
            for (const viewmodel::ExplorerItem& child : item.children)
                renderExplorerItem(child);
            ImGui::TreePop();
        }
        return;
    }
    const bool selected = doc && doc->path == item.path;
    const std::string label = std::string(fileIcon(item.name)) + "  " + item.name + "##" + item.path;
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth;
    if (selected)
        flags |= ImGuiTreeNodeFlags_Selected;
    ImGui::TreeNodeEx(label.c_str(), flags);
    if (ImGui::IsItemClicked())
        vm_.openFile(item.path);
    // Finding count badge.
    const auto findings = vm_.review().findingsForFile(item.path);
    if (!findings.empty()) {
        ImGui::SameLine();
        const model::Severity worst = (*std::max_element(findings.begin(), findings.end(), [](auto a, auto b) {
                                          return a->severity < b->severity;
                                      }))->severity;
        const ImU32 c = worst == model::Severity::Error     ? palette_.severityError
                        : worst == model::Severity::Warning ? palette_.severityWarning
                                                            : palette_.severityInfo;
        const std::string badge = std::to_string(findings.size());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(badge.c_str()).x - 4.0f * s_));
        ImGui::TextColored(col(c), "%s", badge.c_str());
        ImGui::SetItemTooltip("%d open review finding(s)", int(findings.size()));
    }
}

void MainView::renderExplorer()
{
    ImGui::PushStyleVar(ImGuiStyleVar_IndentSpacing, 14.0f * s_);
    renderExplorerItem(vm_.explorer());
    ImGui::PopStyleVar();
    ImGui::Spacing();
    ImGui::TextDisabled("OPEN");
    for (int i = 0; i < int(vm_.documents().size()); ++i) {
        const model::Document& doc = *vm_.documents()[size_t(i)];
        const std::string label = std::string(fileIcon(doc.title)) + "  " + doc.title + (doc.modified ? "  " ICON_FA_CIRCLE : "") +
                                  "##open" + std::to_string(doc.id);
        if (ImGui::Selectable(label.c_str(), i == vm_.activeIndex.get()))
            vm_.activate(i);
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
    dl->PushClipRect(ImVec2(bMax.x + 8.0f * s_, origin.y), ImVec2(x - 4.0f * s_, origin.y + h), true);
    dl->AddText(ImVec2(bMax.x + 10.0f * s_, textY), palette_.textDim, vm_.statusText.get().c_str());
    dl->PopClipRect();
    ImGui::PopFont();

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
    menu("##encoding", {CommandId::EncodingUtf8, CommandId::EncodingUtf8Bom, CommandId::EncodingUtf16Le,
                        CommandId::EncodingUtf16Be, CommandId::EncodingAnsi});
    menu("##language", {CommandId::LanguagePlainText, CommandId::LanguageCpp, CommandId::LanguagePython,
                        CommandId::LanguageJavaScript, CommandId::LanguageMarkdown, CommandId::LanguageJson,
                        CommandId::LanguageCMake});
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
        vm_.reviewTab.set(viewmodel::ReviewTab::Comments);
        vm_.showReviewPanel.set(true);
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
    ImGui::Spacing();
    ImGui::TextDisabled("WORKSPACE");
    int mode = vm_.workspace.get() == WorkspaceMode::Agent ? 0 : 1;
    if (ImGui::RadioButton("Agent", mode == 0))
        vm_.workspace.set(WorkspaceMode::Agent);
    ImGui::SameLine();
    if (ImGui::RadioButton("Code", mode == 1))
        vm_.workspace.set(WorkspaceMode::Code);
    ImGui::Spacing();
    ImGui::TextDisabled("Settings are not saved yet (no settings service).");
    if (ImGui::Button("Close", ImVec2(96.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
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
