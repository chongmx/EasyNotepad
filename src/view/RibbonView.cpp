#include "enp/view/RibbonView.h"
#include "enp/view/Fit.h"

#include "enp/view/Icons.h"
#include "enp/view/UiAutomation.h"
#include "enp/os/Memory.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace enp::view {

using viewmodel::CommandId;
using viewmodel::WorkspaceMode;

namespace {

constexpr float kTabRowHeight = 30.0f;
constexpr float kButtonHeight = 50.0f;
constexpr float kButtonMinWidth = 48.0f;
constexpr float kCaptionHeight = 17.0f;
constexpr float kWorkspaceWidth = 96.0f;
constexpr float kLabelSize = 12.0f;
constexpr float kCaptionSize = 11.5f;
constexpr float kSearchWidth = 220.0f;
// Slim: icon-only buttons in one row, no captions.
constexpr float kSlimButtonHeight = 28.0f;
constexpr float kSlimButtonWidth = 30.0f;
constexpr float kSlimIconSize = 16.0f;
constexpr float kSlimWorkspaceWidth = 70.0f;
constexpr float kToggleWidth = 18.0f;
constexpr float kToggleHeight = 14.0f;

ImU32 fade(ImU32 color, float alpha)
{
    ImVec4 c = ImGui::ColorConvertU32ToFloat4(color);
    c.w *= alpha;
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImVec2 textSize(const char* text, float size)
{
    ImGui::PushFont(nullptr, size);
    const ImVec2 out = ImGui::CalcTextSize(text);
    ImGui::PopFont();
    return out;
}

void drawText(ImDrawList* dl, ImVec2 pos, ImU32 color, const char* text, float size)
{
    ImGui::PushFont(nullptr, size);
    dl->AddText(pos, color, text);
    ImGui::PopFont();
}

std::string lower(std::string s)
{
    for (char& c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

RibbonView::RibbonView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
    // The agent cursor points at commands that may sit on another tab: bring that tab forward.
    subscriptions_.push_back(vm_.ribbonRevealRequested.subscribe([this](const CommandId& id) {
        int& selected = selectedTab();
        auto has = [&](const Tab& tab) {
            for (const Group& g : tab.groups)
                if (std::find(g.buttons.begin(), g.buttons.end(), id) != g.buttons.end())
                    return true;
            return false;
        };
        if (has(shownTab(selected)))
            return;
        for (int i = 0; i < shownCount(); ++i)
            if (has(shownTab(i))) {
                selected = i;
                return;
            }
    }));
    subscriptions_.push_back(vm_.customizeRibbonRequested.subscribe([this]() { customizeRequested_ = true; }));
}

float RibbonView::height() const
{
    return slim() ? (kTabRowHeight + kSlimButtonHeight + 10.0f) * s_ : (kTabRowHeight + kButtonHeight + kCaptionHeight + 10.0f) * s_;
}

std::vector<RibbonView::Tab> RibbonView::builtInTabs(bool agent) const
{
    using C = CommandId;
    static const Group review[] = {
        {"Analyze", {C::RunLint, C::RunStaticAnalysis, C::RunSecurityScan, C::RunStyleCheck, C::RunComplexity}, {}},
        {"AI Review", {C::RunAgentReview, C::FixFindings}, {}},
        {"Run", {C::RunAllReviews}, {}},
        {"Compare", {C::CompareWithSaved, C::CompareAgentChanges}, {}},
        {"Discuss", {C::AddReviewComment}, {}},
        {"Findings", {C::PreviousFinding, C::NextFinding}, {C::ClearFindings}},
    };
    static const Group git[] = {
        {"Changes", {C::GitCommit, C::GitStageAll, C::GitDiscardAll}, {C::ShowGit}},
        {"Branch", {C::GitBranches, C::GitMerge}, {}},
        {"Sync", {C::GitSync, C::GitPull, C::GitPush, C::GitFetch}, {}},
        {"Inspect", {C::ShowGitStatus, C::ShowGitHistory, C::GitBlame}, {}},
        {"Stash", {C::GitStash, C::GitStashPop}, {}},
        {"Assist", {C::GitWriteMessage}, {}},
        {"Learn", {C::GitGuide}, {}},
    };
    // Reading comes first after the workspace's home tab: this editor is built around it.
    static const Group read[] = {
        {"View", {C::ShowCodeView, C::ShowMapView}, {C::RefreshMap}},
        {"Detail", {C::DetailFull, C::DetailBalanced, C::DetailOverview, C::DetailOutline}, {}},
        {"Folding", {C::FoldingSyntax, C::FoldingLogical, C::ToggleFold}, {C::FoldAll, C::UnfoldAll, C::SummarizeFile}},
        {"Side Bar", {C::ShowLayers, C::ShowMinimap, C::ToggleLayersMap}, {}},
        {"Navigate", {C::GoToLine, C::Find}, {C::ToggleBookmark}},
        {"Understand", {C::DissectProject, C::ExplainSelection, C::AskAgent}, {C::GenerateTests}},
    };
    static const Group view[] = {
        {"Panels", {C::ToggleExplorer, C::TogglePanel, C::ToggleTerminal, C::ToggleAgentPanel}, {C::ToggleMaximizePanel, C::ToggleSlimRibbon}},
        {"Display", {C::ToggleLineNumbers, C::ToggleWhitespace, C::ToggleIndentGuides, C::ToggleWordWrap, C::ToggleMarkdownView},
         {C::ShowLayers, C::ShowMinimap, C::ToggleLayersMap}},
        {"Zoom", {C::ZoomIn, C::ZoomOut, C::ZoomReset}, {}},
        {"Theme", {C::ToggleTheme, C::SyntaxColors}, {}},
    };
    static const std::vector<Tab> codeTabs = {
        {"Home",
         {
             {"File", {C::NewFile, C::OpenFile, C::Save, C::CloseFile}, {C::SaveAs, C::SaveAll, C::OpenFolder, C::OpenWorkspace}},
             {"Clipboard", {C::Paste, C::Cut, C::Copy}, {C::SelectAll}},
             {"History", {C::Undo, C::Redo}, {}},
             {"Find", {C::Find, C::Replace, C::GoToLine}, {C::FindNext, C::FindPrevious, C::FindInFiles}},
             {"Assist", {C::AskAgent, C::ExplainSelection}, {C::RefactorSelection, C::GenerateTests}},
         }},
        {"Read", {std::begin(read), std::end(read)}},
        {"Edit",
         {
             {"Lines", {C::DuplicateLine, C::DeleteLine, C::MoveLineUp, C::MoveLineDown}, {C::CopyLineUp, C::SortLines}},
             {"Carets", {C::AddNextOccurrence, C::SelectAllOccurrences, C::AddCursorAbove, C::AddCursorBelow}, {C::CursorsAtLineEnds}},
             {"Case", {C::UpperCase, C::LowerCase}, {}},
             {"Code", {C::ToggleComment, C::Indent, C::Outdent}, {C::TrimTrailing}},
             {"Bookmarks", {C::ToggleBookmark, C::PreviousBookmark, C::NextBookmark}, {C::ClearBookmarks}},
         }},
        {"Review", {std::begin(review), std::end(review)}},
        {"Git", {std::begin(git), std::end(git)}},
        {"View", {std::begin(view), std::end(view)}},
        {"Document",
         {
             {"Line Endings", {C::EolCrLf, C::EolLf, C::EolCr}, {}},
             {"Encoding", {C::EncodingUtf8, C::EncodingUtf8Bom, C::EncodingAnsi}, {C::EncodingUtf16Le, C::EncodingUtf16Be}},
             {"Indentation", {C::ToggleSmartIndent, C::IndentWithSpaces, C::IndentWithTabs},
              {C::IndentWidth2, C::IndentWidth4, C::IndentWidth8, C::DetectIndentation, C::ConvertIndentation}},
             {"Language", {C::ChooseLanguage}, {}},
         }},
        {"Tools",
         {
             {"Terminal", {C::NewTerminal, C::ToggleTerminal, C::PopOutTerminal}, {C::ClearTerminal, C::KillTerminal}},
             {"Macro", {C::StartRecording, C::StopRecording, C::PlayMacro}, {}},
             {"Utilities", {C::HashSha256}, {}},
             {"Application", {C::StartTour, C::Preferences, C::About}, {C::Exit}},
         }},
    };
    static const std::vector<Tab> agentTabs = {
        {"Agent",
         {
             {"Session", {C::NewChat, C::AskAgent, C::StopAgent, C::StartTour}, {}},
             {"Ask", {C::DissectProject, C::ExplainSelection, C::RefactorSelection, C::GenerateTests, C::FixFindings}, {}},
             {"Changes", {C::CompareAgentChanges, C::AcceptAllChanges, C::RejectAllChanges}, {}},
             {"Terminal", {C::NewTerminal, C::ToggleTerminal, C::PopOutTerminal}, {C::ClearTerminal, C::KillTerminal}},
         }},
        {"Read", {std::begin(read), std::end(read)}},
        {"Review", {std::begin(review), std::end(review)}},
        {"Git", {std::begin(git), std::end(git)}},
        {"Files",
         {
             {"File", {C::NewFile, C::OpenFile, C::Save}, {C::SaveAs, C::SaveAll, C::OpenFolder, C::OpenWorkspace, C::CloseFile}},
             {"History", {C::Undo, C::Redo}, {}},
             {"Find", {C::Find, C::GoToLine}, {C::Replace, C::FindInFiles}},
         }},
        {"View", {std::begin(view), std::end(view)}},
    };
    std::vector<Tab> result = agent ? agentTabs : codeTabs;
    result.push_back({"My Tools",
                      {{"Pinned", vm_.pinnedCommands(), {}}, {"Custom Actions", {}, {}, true}}});
    return result;
}

const RibbonView::Layout& RibbonView::layout(int mode)
{
    Layout& l = layouts_[mode];
    if (l.revision == vm_.ribbonRevision())
        return l;
    // Keep the selected tab selected (by name) when the order or visibility changes.
    int& selected = selectedTab_[mode];
    std::string selectedName;
    if (selected >= 0 && selected < int(l.shown.size()))
        selectedName = l.all[size_t(l.shown[size_t(selected)])].name;

    l.revision = vm_.ribbonRevision();
    l.all = builtInTabs(mode == 1);
    // Merge: the saved order first (names the ribbon no longer has are dropped), then any tab
    // the saved layout does not know (added in a later version), shown.
    os::mem::release(l.order);
    os::mem::release(l.orderIndex);
    auto indexOf = [&](const std::string& name) {
        for (size_t i = 0; i < l.all.size(); ++i)
            if (name == l.all[i].name)
                return int(i);
        return -1;
    };
    for (const viewmodel::RibbonTabSetting& t : vm_.ribbonTabs(mode == 1 ? WorkspaceMode::Agent : WorkspaceMode::Code)) {
        const int i = indexOf(t.name);
        if (i >= 0 && std::find(l.orderIndex.begin(), l.orderIndex.end(), i) == l.orderIndex.end()) {
            l.order.push_back(t);
            l.orderIndex.push_back(i);
        }
    }
    for (size_t i = 0; i < l.all.size(); ++i)
        if (std::find(l.orderIndex.begin(), l.orderIndex.end(), int(i)) == l.orderIndex.end()) {
            l.order.push_back({l.all[i].name, true});
            l.orderIndex.push_back(int(i));
        }
    os::mem::release(l.shown);
    for (size_t p = 0; p < l.order.size(); ++p)
        if (l.order[p].visible)
            l.shown.push_back(l.orderIndex[p]);
    if (l.shown.empty()) // never an empty ribbon
        l.shown.push_back(l.orderIndex.front());
    os::mem::release(l.summary);
    for (const Tab& tab : l.all) {
        std::string text;
        for (const Group& g : tab.groups)
            text += (text.empty() ? "" : "  \xC2\xB7  ") + std::string(g.name);
        l.summary.push_back(std::move(text));
    }
    selected = 0;
    for (size_t i = 0; i < l.shown.size(); ++i)
        if (!selectedName.empty() && selectedName == l.all[size_t(l.shown[i])].name)
            selected = int(i);
    return l;
}

int& RibbonView::selectedTab()
{
    const int mode = modeIndex();
    const Layout& l = layout(mode);
    int& index = selectedTab_[mode];
    index = std::clamp(index, 0, int(l.shown.size()) - 1);
    return index;
}

void RibbonView::queueTabEdit(TabEdit kind, int mode, std::string name, int pos, int arg)
{
    editKind_ = kind;
    editMode_ = mode;
    editName_ = std::move(name);
    editPos_ = pos;
    editArg_ = arg;
}

void RibbonView::applyTabEdit()
{
    if (editKind_ == TabEdit::None)
        return;
    const TabEdit kind = editKind_;
    editKind_ = TabEdit::None;
    const WorkspaceMode mode = editMode_ == 1 ? WorkspaceMode::Agent : WorkspaceMode::Code;
    if (kind == TabEdit::Restore) {
        vm_.restoreRibbonTabs(mode);
        os::mem::release(editName_);
        return;
    }
    std::vector<viewmodel::RibbonTabSetting> order = layout(editMode_).order; // the merged list, edited
    const int n = int(order.size());
    auto find = [&](const std::string& name) {
        for (int i = 0; i < n; ++i)
            if (order[size_t(i)].name == name)
                return i;
        return -1;
    };
    int visible = 0;
    for (const auto& t : order)
        visible += t.visible ? 1 : 0;
    switch (kind) {
    case TabEdit::Show:
    case TabEdit::Hide:
        if (const int i = find(editName_); i >= 0) {
            if (kind == TabEdit::Hide && order[size_t(i)].visible && visible <= 1) {
                vm_.statusText.set("At least one ribbon tab stays visible");
                break;
            }
            order[size_t(i)].visible = kind == TabEdit::Show;
        }
        break;
    case TabEdit::Step: // one place up or down in the full list
        if (editPos_ >= 0 && editPos_ < n && editPos_ + editArg_ >= 0 && editPos_ + editArg_ < n)
            std::swap(order[size_t(editPos_)], order[size_t(editPos_ + editArg_)]);
        break;
    case TabEdit::MoveTo: // dragged from editPos_ onto the row at editArg_
        if (editPos_ >= 0 && editPos_ < n && editArg_ >= 0 && editArg_ < n && editPos_ != editArg_) {
            viewmodel::RibbonTabSetting moved = order[size_t(editPos_)];
            order.erase(order.begin() + editPos_);
            order.insert(order.begin() + editArg_, std::move(moved));
        }
        break;
    case TabEdit::StepAmongShown: // the ribbon's "Move left/right": past the next visible tab
        if (const int i = find(editName_); i >= 0) {
            int j = i + editArg_;
            while (j >= 0 && j < n && !order[size_t(j)].visible)
                j += editArg_;
            if (j >= 0 && j < n) {
                viewmodel::RibbonTabSetting moved = order[size_t(i)];
                order.erase(order.begin() + i);
                order.insert(order.begin() + j, std::move(moved));
            }
        }
        break;
    default:
        break;
    }
    os::mem::release(editName_);
    vm_.setRibbonTabs(mode, std::move(order));
}

// ---- Frame -------------------------------------------------------------------------------------

void RibbonView::render()
{
    ImGui::PushID("ribbon");
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = ImGui::GetContentRegionAvail().x;
    const float h = height();
    dl->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + h), palette_.ribbonBg);
    dl->AddLine(ImVec2(origin.x, origin.y + h - 1.0f), ImVec2(origin.x + width, origin.y + h - 1.0f), palette_.border);

    const float blockWidth = (slim() ? kSlimWorkspaceWidth : kWorkspaceWidth) * s_;
    ImGui::SetCursorScreenPos(origin);
    renderWorkspaceBlock(blockWidth, h - 1.0f);
    dl->AddLine(ImVec2(origin.x + blockWidth, origin.y + 8.0f * s_), ImVec2(origin.x + blockWidth, origin.y + h - 8.0f * s_),
                palette_.border);

    const float left = origin.x + blockWidth + 8.0f * s_;
    ImGui::SetCursorScreenPos(ImVec2(left, origin.y + 3.0f * s_));
    renderTabRow(origin.x + width - left - 8.0f * s_);

    // The groups scroll sideways when they do not fit; the size toggle keeps a corner of its own.
    const float toggleRoom = (kToggleWidth + 10.0f) * s_;
    ImGui::SetCursorScreenPos(ImVec2(left, origin.y + (kTabRowHeight + 4.0f) * s_));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * s_);
    const float groupsH = (slim() ? kSlimButtonHeight : kButtonHeight + kCaptionHeight) + 4.0f;
    ImGui::BeginChild("##ribbonGroups", ImVec2(origin.x + width - left - toggleRoom, groupsH * s_), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
    renderGroups(shownTab(selectedTab()));
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    renderSizeToggle(ImVec2(origin.x + width - (kToggleWidth + 6.0f) * s_, origin.y + h - (kToggleHeight + 6.0f) * s_));

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, h));
    if (customizeRequested_) {
        ImGui::OpenPopup("Customize Ribbon");
        customizeRequested_ = false;
    }
    renderCustomizeDialog();
    ImGui::PopID();
}

void RibbonView::renderWorkspaceBlock(float width, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool agent = vm_.workspace.get() == WorkspaceMode::Agent;
    ImGui::InvisibleButton("##workspace", ImVec2(width, height));
    registerLastItem("workspace", "button", agent ? "Agent workspace" : "Code workspace",
                     [this]() { vm_.execute(CommandId::ToggleWorkspaceMode); });
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked())
        ImGui::OpenPopup("##workspacePopup");
    if (hovered)
        ImGui::SetItemTooltip("Switch workspace (%s)", commandInfo(CommandId::ToggleWorkspaceMode).shortcut);

    const float pad = 6.0f * s_;
    if (hovered)
        dl->AddRectFilled(ImVec2(p.x + pad, p.y + pad), ImVec2(p.x + width - pad, p.y + height - pad), palette_.hover, 6.0f * s_);

    const ImU32 modeColor = agent ? palette_.modeAgent : palette_.modeCode;
    const char* icon = commandIcon(agent ? CommandId::SwitchToAgentMode : CommandId::SwitchToCodeMode);
    const char* name = agent ? "Agent" : "Code";
    if (slim()) { // the icon over "Code ▾"
        const std::string caption = std::string(name) + " " ICON_FA_CARET_DOWN;
        const ImVec2 iconSize = textSize(icon, 20.0f);
        const ImVec2 captionSize = textSize(caption.c_str(), kLabelSize);
        float y = p.y + (height - iconSize.y - captionSize.y - 3.0f * s_) * 0.5f;
        drawText(dl, ImVec2(p.x + (width - iconSize.x) * 0.5f, y), modeColor, icon, 20.0f);
        y += iconSize.y + 3.0f * s_;
        drawText(dl, ImVec2(p.x + (width - captionSize.x) * 0.5f, y), palette_.text, caption.c_str(), kLabelSize);
        ImGui::SetNextWindowPos(ImVec2(p.x + pad, p.y + height));
        renderWorkspacePopup();
        return;
    }
    const ImVec2 iconSize = textSize(icon, 26.0f);
    const ImVec2 nameSize = textSize(name, 15.0f);
    const char* sub = "Workspace " ICON_FA_CARET_DOWN;
    const ImVec2 subSize = textSize(sub, kCaptionSize);
    float y = p.y + (height - iconSize.y - nameSize.y - subSize.y - 6.0f * s_) * 0.5f;
    drawText(dl, ImVec2(p.x + (width - iconSize.x) * 0.5f, y), modeColor, icon, 26.0f);
    y += iconSize.y + 4.0f * s_;
    drawText(dl, ImVec2(p.x + (width - nameSize.x) * 0.5f, y), palette_.text, name, 15.0f);
    y += nameSize.y + 2.0f * s_;
    drawText(dl, ImVec2(p.x + (width - subSize.x) * 0.5f, y), palette_.textDim, sub, kCaptionSize);

    ImGui::SetNextWindowPos(ImVec2(p.x + pad, p.y + height));
    renderWorkspacePopup();
}

void RibbonView::renderWorkspacePopup()
{
    if (!ImGui::BeginPopup("##workspacePopup"))
        return;
    ImGui::TextDisabled("WORKSPACE");
    const CommandId modes[] = {CommandId::SwitchToAgentMode, CommandId::SwitchToCodeMode};
    for (CommandId id : modes) {
        const viewmodel::CommandInfo& info = commandInfo(id);
        const bool selected = vm_.isChecked(id);
        const float rowH = 46.0f * s_;
        ImGui::PushID(int(id));
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##mode", selected, ImGuiSelectableFlags_None, ImVec2(320.0f * s_, rowH)))
            vm_.execute(id);
        ImGui::PopID();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const ImU32 color = id == CommandId::SwitchToAgentMode ? palette_.modeAgent : palette_.modeCode;
        drawText(dl, ImVec2(p.x + 8.0f * s_, p.y + 12.0f * s_), color, commandIcon(id), 22.0f);
        drawText(dl, ImVec2(p.x + 44.0f * s_, p.y + 5.0f * s_), palette_.text, info.label, 15.0f);
        const char* desc = info.tooltip;
        const char* colon = std::strchr(desc, ':');
        ImGui::PushFont(nullptr, kLabelSize);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(p.x + 44.0f * s_, p.y + 25.0f * s_), palette_.textDim,
                    colon ? colon + 2 : desc, nullptr, 270.0f * s_);
        ImGui::PopFont();
    }
    ImGui::Separator();
    ImGui::TextDisabled("Toggle with %s", commandInfo(CommandId::ToggleWorkspaceMode).shortcut);
    ImGui::EndPopup();
}

void RibbonView::renderTabRow(float width)
{
    const int mode = modeIndex();
    int& selected = selectedTab();
    const Layout& l = layout(mode); // not rebuilt while drawn: edits from the menu are queued
    const int count = int(l.shown.size());
    const float pillH = (kTabRowHeight - 6.0f) * s_;
    const float customizeW = ImGui::CalcTextSize(ICON_FA_SLIDERS).x + 18.0f * s_;
    float needed = customizeW + 12.0f * s_;
    for (int i = 0; i < count; ++i)
        needed += ImGui::CalcTextSize(l.all[size_t(l.shown[size_t(i)])].name).x + 26.0f * s_;
    // Too narrow for every tab (a phone): the row scrolls sideways on its own, by finger or its thin
    // scrollbar, and the command search makes room for it.
    const bool scrolling = needed > width;
    if (scrolling) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 3.0f * s_);
        ImGui::BeginChild("##ribbonTabs", ImVec2(width, (kTabRowHeight - 2.0f) * s_), ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float x = start.x;
    for (int i = 0; i < count; ++i) {
        const Tab& tab = l.all[size_t(l.shown[size_t(i)])];
        const ImVec2 size = ImGui::CalcTextSize(tab.name);
        const float w = size.x + 24.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, pillH)))
            selected = i;
        if (scrolling && i == selected && i != shownTab_) { // a newly chosen tab is brought into view
            ImGui::SetScrollHereX(0.5f);
            shownTab_ = i;
        }
        registerLastItem(std::string("ribbon-tab:") + tab.name, "tab", tab.name, [this, i]() { selectedTab() = i; });
        ImGui::SetItemTooltip("Right-click to hide or move this tab");
        if (ImGui::BeginPopupContextItem("##tabMenu")) {
            tabMenu(mode, l, i);
            ImGui::EndPopup();
        }
        ImGui::PopID();
        const bool active = i == selected;
        if (active)
            dl->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + pillH), palette_.tabActiveBg, 5.0f * s_);
        else if (ImGui::IsItemHovered())
            dl->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + pillH), palette_.hover, 5.0f * s_);
        dl->AddText(ImVec2(x + 12.0f * s_, start.y + (pillH - size.y) * 0.5f), active ? palette_.text : palette_.textDim,
                    tab.name);
        x += w + 2.0f * s_;
    }

    // Customize: right after the tabs, like an extra pill.
    {
        const char* label = ICON_FA_SLIDERS;
        const float w = ImGui::CalcTextSize(label).x + 18.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x + 4.0f * s_, start.y));
        if (ImGui::InvisibleButton("##customize", ImVec2(w, pillH)))
            customizeRequested_ = true;
        const bool hovered = ImGui::IsItemHovered();
        if (hovered)
            dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), palette_.hover, 5.0f * s_);
        dl->AddText(ImVec2(x + 4.0f * s_ + 9.0f * s_, start.y + (pillH - ImGui::GetTextLineHeight()) * 0.5f),
                    hovered ? palette_.text : palette_.textFaint, label);
        ImGui::SetItemTooltip("Customize the ribbon: show, hide and arrange its tabs, pin commands, add your own actions");
        registerLastItem("ribbon-customize", "button", "Customize the ribbon", [this]() { customizeRequested_ = true; });
        x += w + 8.0f * s_;
    }
    if (scrolling) {
        ImGui::SetCursorScreenPos(start);
        ImGui::Dummy(ImVec2(x - start.x, pillH)); // the scrollable extent
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        applyTabEdit();
        return;
    }
    applyTabEdit();

    const float searchW = std::min(kSearchWidth * s_, std::max(0.0f, start.x + width - x - 8.0f * s_));
    if (searchW > 80.0f * s_) {
        ImGui::SetCursorScreenPos(ImVec2(start.x + width - searchW, start.y));
        renderCommandSearch(searchW);
    }
}

void RibbonView::renderCommandSearch(float width)
{
    ImGui::SetNextItemWidth(width);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f * s_);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f * s_, 3.0f * s_));
    const bool enter = ImGui::InputTextWithHint("##commandSearch", ICON_FA_MAGNIFYING_GLASS "  Search commands", &search_,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::PopStyleVar(2);
    const bool active = ImGui::IsItemActive();
    const ImVec2 below(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y + 2.0f * s_);
    if (active && ImGui::IsKeyPressed(ImGuiKey_Escape))
        search_.clear();

    std::vector<CommandId> matches;
    const std::string needle = lower(search_);
    if (!needle.empty())
        for (int i = 0; i < int(CommandId::Count) && matches.size() < 12; ++i) {
            const viewmodel::CommandInfo& info = commandInfo(CommandId(i));
            if (lower(info.label).find(needle) != std::string::npos || lower(info.tooltip).find(needle) != std::string::npos)
                matches.push_back(CommandId(i));
        }
    if (enter && !matches.empty()) {
        vm_.execute(matches.front());
        search_.clear();
        return;
    }
    if (!active && !searchFocused_)
        return;
    if (needle.empty())
        return;

    ImGui::SetNextWindowPos(below);
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, 0.0f), ImVec2(width * 1.6f, 420.0f * s_));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoNav;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::GetStyle().Colors[ImGuiCol_PopupBg]);
    ImGui::Begin("##commandResults", nullptr, flags);
    ImGui::PopStyleColor();
    if (matches.empty())
        ImGui::TextDisabled("No matching commands");
    for (CommandId id : matches) {
        const viewmodel::CommandInfo& info = commandInfo(id);
        const std::string label = std::string(commandIcon(id)) + "  " + info.label + "##" + std::to_string(int(id));
        if (ImGui::MenuItem(label.c_str(), info.shortcut, false, vm_.command(id).canExecute())) {
            vm_.execute(id);
            search_.clear();
        }
        ImGui::SetItemTooltip("%s", info.tooltip);
    }
    searchFocused_ = ImGui::IsWindowHovered() && !search_.empty();
    ImGui::End();
}

void RibbonView::renderGroups(const Tab& tab)
{
    // Buttons are placed explicitly: SameLine() state would leak between the button row
    // and the caption row.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float top = start.y;
    float x = start.x + 2.0f * s_;
    for (size_t g = 0; g < tab.groups.size(); ++g) {
        const Group& group = tab.groups[g];
        const float x0 = x;
        ImGui::PushID(int(g));
        if (group.customActions)
            renderCustomActions(x, top);
        for (CommandId id : group.buttons) {
            ImGui::SetCursorScreenPos(ImVec2(x, top));
            if (ribbonButton(id))
                vm_.execute(id);
            x = ImGui::GetItemRectMax().x + 2.0f * s_;
        }
        float x1 = x - 2.0f * s_;
        if (slim()) {
            if (!group.more.empty()) {
                ImGui::SetCursorScreenPos(ImVec2(x - 1.0f * s_, top));
                if (ImGui::InvisibleButton("##moreCaret", ImVec2(13.0f * s_, kSlimButtonHeight * s_)))
                    ImGui::OpenPopup("##more");
                const bool hovered = ImGui::IsItemHovered();
                if (hovered)
                    dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), palette_.hover, 4.0f * s_);
                const ImVec2 caretSize = textSize(ICON_FA_CARET_DOWN, kCaptionSize);
                drawText(dl, ImVec2(ImGui::GetItemRectMin().x + (13.0f * s_ - caretSize.x) * 0.5f, top + (kSlimButtonHeight * s_ - caretSize.y) * 0.5f),
                         hovered ? palette_.text : palette_.textFaint, ICON_FA_CARET_DOWN, kCaptionSize);
                ImGui::SetItemTooltip("More: %s", group.name);
                ImGui::SetNextWindowPos(ImVec2(x0, top + kSlimButtonHeight * s_ + 2.0f * s_));
                if (ImGui::BeginPopup("##more")) {
                    morePopup(group);
                    ImGui::EndPopup();
                }
                x1 = ImGui::GetItemRectMax().x;
            }
        } else {
            const std::string caption = group.more.empty() ? std::string(group.name) : std::string(group.name) + " " ICON_FA_CARET_DOWN;
            const float captionW = textSize(caption.c_str(), kCaptionSize).x + 8.0f * s_;
            x1 = std::max(x1, x0 + captionW);
            groupCaption(group, x0, x1, top + kButtonHeight * s_ + 1.0f * s_);
        }
        ImGui::PopID();

        const float rowH = (slim() ? kSlimButtonHeight : kButtonHeight + kCaptionHeight) * s_;
        const float sepX = x1 + (slim() ? 6.0f : 8.0f) * s_;
        if (g + 1 < tab.groups.size())
            dl->AddLine(ImVec2(sepX, top + (slim() ? 5.0f : 6.0f) * s_), ImVec2(sepX, top + rowH - (slim() ? 5.0f : 2.0f) * s_),
                        palette_.border);
        x = sepX + (slim() ? 6.0f : 8.0f) * s_;
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, top));
    ImGui::Dummy(ImVec2(x - start.x, (slim() ? kSlimButtonHeight : kButtonHeight + kCaptionHeight) * s_)); // scrollable extent
}

bool RibbonView::ribbonButton(CommandId id)
{
    const viewmodel::CommandInfo& info = commandInfo(id);
    ImGui::PushID(int(id));
    const std::string detail = info.shortcut[0] ? std::string(info.tooltip) + "\n" + info.shortcut : std::string(info.tooltip);
    const bool clicked = drawButton("##ribbonButton", fonts_.hasIcons ? commandIcon(id) : "?", info.label,
                                    commandTint(id, palette_), vm_.command(id).canExecute(), vm_.isChecked(id), info.label,
                                    detail.c_str());
    registerLastItem(std::string("command:") + commandKey(id), "button", info.label, [this, id]() { vm_.execute(id); },
                     vm_.command(id).canExecute());
    ImGui::PopID();
    return clicked;
}

void RibbonView::renderCustomActions(float& x, float top)
{
    const auto& actions = vm_.customActions();
    for (int i = 0; i < int(actions.size()); ++i) {
        ImGui::SetCursorScreenPos(ImVec2(x, top));
        ImGui::PushID(i);
        const std::string detail = "Runs in the terminal:\n" + actions[size_t(i)].command;
        if (drawButton("##customAction", ICON_FA_BOLT, actions[size_t(i)].name.c_str(), palette_.iconGreen, true, false,
                       actions[size_t(i)].name.c_str(), detail.c_str()))
            vm_.runCustomAction(i);
        registerLastItem("action:" + actions[size_t(i)].name, "button", actions[size_t(i)].name,
                         [this, i]() { vm_.runCustomAction(i); });
        ImGui::PopID();
        x = ImGui::GetItemRectMax().x + 2.0f * s_;
    }
    ImGui::SetCursorScreenPos(ImVec2(x, top));
    if (drawButton("##addAction", ICON_FA_PLUS, "Add", palette_.iconGray, true, false, "Add an action",
                   "Create a button that runs a command in the terminal")) {
        customizeRequested_ = true;
        customizeSectionRequest_ = 2;
    }
    x = ImGui::GetItemRectMax().x + 2.0f * s_;
}

void RibbonView::tabMenu(int mode, const Layout& l, int shownIndex)
{
    const std::string name = l.all[size_t(l.shown[size_t(shownIndex)])].name;
    const int count = int(l.shown.size());
    ImGui::TextDisabled("%s", name.c_str());
    if (ImGui::MenuItem(ICON_FA_EYE_SLASH "  Hide this tab", nullptr, false, count > 1))
        queueTabEdit(TabEdit::Hide, mode, name);
    if (count <= 1)
        ImGui::SetItemTooltip("At least one tab stays visible");
    if (ImGui::MenuItem(ICON_FA_ARROW_LEFT "  Move left", nullptr, false, shownIndex > 0))
        queueTabEdit(TabEdit::StepAmongShown, mode, name, 0, -1);
    if (ImGui::MenuItem(ICON_FA_ARROW_RIGHT "  Move right", nullptr, false, shownIndex + 1 < count))
        queueTabEdit(TabEdit::StepAmongShown, mode, name, 0, +1);
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_SLIDERS "  Customize the ribbon..."))
        customizeRequested_ = true;
    const bool custom = !vm_.ribbonTabs(mode == 1 ? WorkspaceMode::Agent : WorkspaceMode::Code).empty();
    if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Restore default tabs", nullptr, false, custom))
        queueTabEdit(TabEdit::Restore, mode, {});
}

void RibbonView::renderCustomizeDialog()
{
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(820.0f * s_, 560.0f * s_)), ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    bool open = true;
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Customize Ribbon", &open, ImGuiWindowFlags_NoSavedSettings))
        return;
    if (ImGui::IsWindowAppearing())
        customizeMode_ = modeIndex(); // start with the workspace in use
    if (registerWindowClose("dialog:customize-ribbon:close", "Close Customize Ribbon"))
        ImGui::CloseCurrentPopup();

    // Three sections, one per kind of change.
    const int request = customizeSectionRequest_;
    customizeSectionRequest_ = -1;
    if (ImGui::BeginTabBar("##sections")) {
        const char* const names[] = {ICON_FA_TABLE_COLUMNS "  Tabs###tabs", ICON_FA_THUMBTACK "  Pinned commands###pinned",
                                     ICON_FA_BOLT "  Custom actions###actions"};
        const char* const ids[] = {"customize-ribbon:section:tabs", "customize-ribbon:section:pinned", "customize-ribbon:section:actions"};
        for (int k = 0; k < 3; ++k) {
            if (ImGui::BeginTabItem(names[k], nullptr, request == k ? ImGuiTabItemFlags_SetSelected : 0)) {
                customizeSection_ = k;
                ImGui::EndTabItem();
            }
            registerLastItem(ids[k], "tab", names[k], [this, k]() { customizeSectionRequest_ = k; });
        }
        ImGui::EndTabBar();
    }
    const float footerH = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f;
    ImGui::BeginChild("##section", ImVec2(0.0f, std::max(60.0f * s_, ImGui::GetContentRegionAvail().y - footerH)));
    if (customizeSection_ == 0)
        renderTabsSection();
    else if (customizeSection_ == 1)
        renderPinnedSection();
    else
        renderActionsSection();
    ImGui::EndChild();
    applyTabEdit();

    // Footer: the section's reset on the left, Done on the right.
    ImGui::Separator();
    if (customizeSection_ == 0) {
        const WorkspaceMode mode = customizeMode_ == 1 ? WorkspaceMode::Agent : WorkspaceMode::Code;
        const bool custom = !vm_.ribbonTabs(mode).empty();
        ImGui::BeginDisabled(!custom);
        if (ImGui::Button(customizeMode_ == 1 ? ICON_FA_ROTATE_LEFT "  Restore default Agent tabs" : ICON_FA_ROTATE_LEFT "  Restore default Code tabs"))
            vm_.restoreRibbonTabs(mode);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip(custom ? "Every tab of this workspace, shown, in the original order" : "The tabs are already as they came");
        registerLastItem("customize-ribbon:restore", "button", "Restore default tabs", [this, mode]() { vm_.restoreRibbonTabs(mode); }, custom);
    } else if (customizeSection_ == 1) {
        const bool any = !vm_.pinnedCommands().empty();
        ImGui::BeginDisabled(!any);
        if (ImGui::Button(ICON_FA_ROTATE_LEFT "  Unpin all")) {
            const std::vector<CommandId> pinned = vm_.pinnedCommands();
            for (CommandId id : pinned)
                vm_.setPinned(id, false);
        }
        ImGui::EndDisabled();
    } else {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Custom actions appear in My Tools.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("  Changes apply at once and are remembered.");
    ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 90.0f * s_));
    if (ImGui::Button("Done", ImVec2(90.0f * s_, 0.0f)) || !open)
        ImGui::CloseCurrentPopup();
    registerLastItem("customize-ribbon:done", "button", "Done", [this]() { customizeSectionRequest_ = -2; });
    if (customizeSectionRequest_ == -2) {
        customizeSectionRequest_ = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void RibbonView::renderTabsSection()
{
    const ImVec4 dim = ImGui::GetStyle().Colors[ImGuiCol_TextDisabled];
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Tabs of the");
    ImGui::SameLine();
    // A two-way switch: the workspace whose tabs are listed.
    const char* const labels[] = {ICON_FA_CODE "  Code", ICON_FA_ROBOT "  Agent"};
    const char* const ids[] = {"customize-ribbon:workspace:code", "customize-ribbon:workspace:agent"};
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
    for (int m = 0; m < 2; ++m) {
        const bool active = customizeMode_ == m;
        ImGui::PushStyleColor(ImGuiCol_Button, active ? ImGui::ColorConvertU32ToFloat4(palette_.accentSoft) : ImGui::GetStyle().Colors[ImGuiCol_FrameBg]);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(active ? palette_.text : palette_.textDim));
        if (m > 0)
            ImGui::SameLine();
        if (ImGui::Button(labels[m]))
            customizeMode_ = m;
        ImGui::PopStyleColor(2);
        registerLastItem(ids[m], "button", m == 0 ? "Code workspace" : "Agent workspace", [this, m]() { customizeMode_ = m; });
    }
    ImGui::PopStyleVar();
    ImGui::SameLine();
    ImGui::TextUnformatted("workspace");
    const int mode = customizeMode_;
    const Layout& l = layout(mode); // edits below are queued and applied after this list
    int visible = 0;
    for (const auto& t : l.order)
        visible += t.visible ? 1 : 0;
    ImGui::SameLine();
    ImGui::TextDisabled("   %d of %d tabs shown", visible, int(l.order.size()));
    ImGui::PushStyleColor(ImGuiCol_Text, dim);
    ImGui::TextWrapped("Tick a tab to show it on the ribbon, untick it to hide it. Change the order by dragging a row by its "
                       ICON_FA_GRIP_VERTICAL " handle, or with the arrows. Each workspace has its own tabs. You can also "
                       "right-click a tab on the ribbon.");
    ImGui::PopStyleColor();
    ImGui::Spacing();

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_ScrollY;
    if (!ImGui::BeginTable("##tabList", 5, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("##handle", ImGuiTableColumnFlags_WidthFixed, 22.0f * s_);
    ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed, 44.0f * s_);
    ImGui::TableSetupColumn("Tab", ImGuiTableColumnFlags_WidthFixed, 120.0f * s_);
    ImGui::TableSetupColumn("What it holds", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Order", ImGuiTableColumnFlags_WidthFixed, 64.0f * s_);
    ImGui::TableHeadersRow();
    const int n = int(l.order.size());
    for (int p = 0; p < n; ++p) {
        const viewmodel::RibbonTabSetting& t = l.order[size_t(p)];
        ImGui::PushID(p);
        ImGui::TableNextRow(ImGuiTableRowFlags_None, ImGui::GetFrameHeight() + 4.0f * s_);
        // The whole row is the drag handle and the drop target; the controls on it stay clickable.
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::Selectable(ICON_FA_GRIP_VERTICAL "##row", false,
                          ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                          ImVec2(0.0f, ImGui::GetFrameHeight()));
        if (ImGui::BeginDragDropSource()) {
            ImGui::SetDragDropPayload("ENP_RIBBON_TAB", &p, sizeof(int));
            ImGui::Text(ICON_FA_GRIP_VERTICAL "  %s", t.name.c_str());
            ImGui::EndDragDropSource();
        }
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ENP_RIBBON_TAB"))
                queueTabEdit(TabEdit::MoveTo, mode, {}, *static_cast<const int*>(payload->Data), p);
            ImGui::EndDragDropTarget();
        }
        if (ImGui::IsItemHovered() && !ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);

        ImGui::TableSetColumnIndex(1);
        bool show = t.visible;
        const bool last = t.visible && visible <= 1;
        ImGui::BeginDisabled(last);
        if (ImGui::Checkbox("##show", &show))
            queueTabEdit(show ? TabEdit::Show : TabEdit::Hide, mode, t.name);
        ImGui::EndDisabled();
        if (last)
            ImGui::SetItemTooltip("At least one tab stays visible");
        registerLastItem("customize-ribbon:show:" + t.name, "checkbox", (t.visible ? "Hide " : "Show ") + t.name,
                         [this, mode, name = t.name, visibleNow = t.visible]() {
                             queueTabEdit(visibleNow ? TabEdit::Hide : TabEdit::Show, mode, name);
                         },
                         !last);

        ImGui::TableSetColumnIndex(2);
        ImGui::AlignTextToFramePadding();
        if (t.visible)
            ImGui::TextUnformatted(t.name.c_str());
        else
            ImGui::TextDisabled("%s", t.name.c_str());

        ImGui::TableSetColumnIndex(3);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", l.summary[size_t(l.orderIndex[size_t(p)])].c_str());

        ImGui::TableSetColumnIndex(4);
        ImGui::BeginDisabled(p == 0);
        if (ImGui::Button(ICON_FA_ARROW_UP "##up"))
            queueTabEdit(TabEdit::Step, mode, {}, p, -1);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Move up (earlier on the ribbon)");
        registerLastItem("customize-ribbon:up:" + t.name, "button", "Move " + t.name + " up",
                         [this, mode, p]() { queueTabEdit(TabEdit::Step, mode, {}, p, -1); }, p > 0);
        ImGui::SameLine(0.0f, 2.0f * s_);
        ImGui::BeginDisabled(p + 1 >= n);
        if (ImGui::Button(ICON_FA_ARROW_DOWN "##down"))
            queueTabEdit(TabEdit::Step, mode, {}, p, +1);
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Move down (later on the ribbon)");
        registerLastItem("customize-ribbon:down:" + t.name, "button", "Move " + t.name + " down",
                         [this, mode, p]() { queueTabEdit(TabEdit::Step, mode, {}, p, +1); }, p + 1 < n);
        ImGui::PopID();
    }
    ImGui::EndTable();
}

void RibbonView::renderPinnedSection()
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("Pinned commands appear in the My Tools tab, in this order. Tick any command below to pin it.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    // The pinned ones first, with their order.
    const std::vector<CommandId>& pinned = vm_.pinnedCommands();
    CommandId moveId = CommandId::Count, unpinId = CommandId::Count;
    int moveDir = 0;
    if (pinned.empty()) {
        ImGui::TextDisabled(ICON_FA_THUMBTACK "  Nothing pinned yet.");
    } else if (ImGui::BeginTable("##pinnedList", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("##icon", ImGuiTableColumnFlags_WidthFixed, 22.0f * s_);
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthFixed, 96.0f * s_);
        for (int i = 0; i < int(pinned.size()); ++i) {
            const CommandId id = pinned[size_t(i)];
            ImGui::PushID(int(id));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(commandTint(id, palette_)), "%s", commandIcon(id));
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(commandInfo(id).label);
            ImGui::TableSetColumnIndex(2);
            ImGui::BeginDisabled(i == 0);
            if (ImGui::SmallButton(ICON_FA_ARROW_UP "##up"))
                moveId = id, moveDir = -1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 >= int(pinned.size()));
            if (ImGui::SmallButton(ICON_FA_ARROW_DOWN "##down"))
                moveId = id, moveDir = +1;
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton(ICON_FA_XMARK "##unpin"))
                unpinId = id;
            ImGui::SetItemTooltip("Unpin");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (moveId != CommandId::Count)
        vm_.movePinned(moveId, moveDir);
    if (unpinId != CommandId::Count)
        vm_.setPinned(unpinId, false);

    ImGui::Spacing();
    ImGui::SeparatorText("All commands");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Filter commands", &pinFilter_);
    ImGui::BeginChild("##commandList", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    const std::string needle = lower(pinFilter_);
    CommandId toggled = CommandId::Count;
    bool toggledTo = false;
    for (int i = 0; i < int(CommandId::Count); ++i) {
        const CommandId id = CommandId(i);
        const viewmodel::CommandInfo& info = commandInfo(id);
        if (!needle.empty() && lower(info.label).find(needle) == std::string::npos &&
            lower(info.tooltip).find(needle) == std::string::npos)
            continue;
        bool isPinned = vm_.isPinned(id);
        ImGui::PushID(i);
        if (ImGui::Checkbox("##pin", &isPinned))
            toggled = id, toggledTo = isPinned;
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(commandTint(id, palette_)), "%s", commandIcon(id));
        ImGui::SameLine();
        ImGui::TextUnformatted(info.label);
        ImGui::SameLine();
        ImGui::TextDisabled("%s", info.tooltip);
        ImGui::PopID();
    }
    ImGui::EndChild();
    if (toggled != CommandId::Count)
        vm_.setPinned(toggled, toggledTo);
}

void RibbonView::renderActionsSection()
{
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyle().Colors[ImGuiCol_TextDisabled]);
    ImGui::TextWrapped("Each action is a button in the My Tools tab that runs a shell command in the integrated terminal.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    const auto& actions = vm_.customActions();
    int remove = -1;
    if (actions.empty())
        ImGui::TextDisabled(ICON_FA_BOLT "  No actions yet.");
    else if (ImGui::BeginTable("##actionTable", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 140.0f * s_);
        ImGui::TableSetupColumn("Command", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(actions.size()); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(actions[size_t(i)].name.c_str());
            ImGui::TableSetColumnIndex(1);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("%s", actions[size_t(i)].command.c_str());
            ImGui::TableSetColumnIndex(2);
            if (ImGui::SmallButton(ICON_FA_TRASH_CAN "##remove"))
                remove = i;
            ImGui::SetItemTooltip("Remove");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remove >= 0)
        vm_.removeCustomAction(remove);
    ImGui::Spacing();
    ImGui::SeparatorText("New action");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##actionName", "Name, e.g. Build", &newActionName_);
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputTextWithHint("##actionCommand", "Command, e.g. cmake --build build", &newActionCommand_,
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::BeginDisabled(newActionName_.empty() || newActionCommand_.empty());
    if (ImGui::Button(ICON_FA_PLUS "  Add action") || (enter && !newActionName_.empty() && !newActionCommand_.empty())) {
        vm_.addCustomAction(newActionName_, newActionCommand_);
        os::mem::release(newActionName_);
        os::mem::release(newActionCommand_);
    }
    ImGui::EndDisabled();
}

bool RibbonView::drawButton(const char* id, const char* icon, const char* label, ImU32 tint, bool enabled, bool checked,
                            const char* tooltip, const char* detail)
{
    const bool small = slim();
    const ImVec2 labelSize = small ? ImVec2(0.0f, 0.0f) : textSize(label, kLabelSize);
    const float w = small ? kSlimButtonWidth * s_ : std::max(kButtonMinWidth * s_, labelSize.x + 14.0f * s_);
    const float h = (small ? kSlimButtonHeight : kButtonHeight) * s_;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    if (checked)
        dl->AddRectFilled(p, q, palette_.accentSoft, 6.0f * s_);
    if (hovered && enabled)
        dl->AddRectFilled(p, q, held ? fade(palette_.hover, 2.0f) : palette_.hover, 6.0f * s_);

    if (small) {
        const ImVec2 iconSize = textSize(icon, kSlimIconSize);
        drawText(dl, ImVec2(p.x + (w - iconSize.x) * 0.5f, p.y + (h - iconSize.y) * 0.5f), enabled ? tint : fade(tint, 0.35f), icon,
                 kSlimIconSize);
    } else {
        const ImVec2 iconSize = textSize(icon, Fonts::kRibbonIconSize);
        drawText(dl, ImVec2(p.x + (w - iconSize.x) * 0.5f, p.y + 5.0f * s_), enabled ? tint : fade(tint, 0.35f), icon,
                 Fonts::kRibbonIconSize);
        drawText(dl, ImVec2(p.x + (w - labelSize.x) * 0.5f, q.y - labelSize.y - 5.0f * s_),
                 enabled ? palette_.text : palette_.textFaint, label, kLabelSize);
    }

    if (hovered && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(tooltip);
        ImGui::TextDisabled("%s", detail);
        ImGui::EndTooltip();
    }
    return clicked && enabled;
}

void RibbonView::morePopup(const Group& group)
{
    for (CommandId id : group.more) {
        const viewmodel::CommandInfo& info = commandInfo(id);
        const std::string label = std::string(commandIcon(id)) + "  " + info.label;
        if (ImGui::MenuItem(label.c_str(), info.shortcut, vm_.isChecked(id), vm_.command(id).canExecute()))
            vm_.execute(id);
    }
}

void RibbonView::renderSizeToggle(ImVec2 topLeft)
{
    // Quiet until hovered: a faint chevron in the corner, a soft pill under the pointer.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 size(kToggleWidth * s_, kToggleHeight * s_);
    ImGui::SetCursorScreenPos(topLeft);
    if (ImGui::InvisibleButton("##ribbonSize", size))
        vm_.execute(CommandId::ToggleSlimRibbon);
    registerLastItem("ribbon-size", "button", slim() ? "Full ribbon" : "Slim ribbon",
                     [this]() { vm_.execute(CommandId::ToggleSlimRibbon); });
    const bool hovered = ImGui::IsItemHovered();
    if (hovered)
        dl->AddRectFilled(topLeft, ImVec2(topLeft.x + size.x, topLeft.y + size.y), palette_.hover, 4.0f * s_);
    const char* icon = slim() ? ICON_FA_CHEVRON_DOWN : ICON_FA_CHEVRON_UP;
    const float iconPx = 9.0f;
    const ImVec2 iconSize = textSize(icon, iconPx);
    drawText(dl, ImVec2(topLeft.x + (size.x - iconSize.x) * 0.5f, topLeft.y + (size.y - iconSize.y) * 0.5f),
             hovered ? palette_.text : palette_.textFaint, icon, iconPx);
    ImGui::SetItemTooltip(slim() ? "Full ribbon (Ctrl+F1)" : "Slim ribbon: icons only, more room for your work (Ctrl+F1)");
}

void RibbonView::groupCaption(const Group& group, float x0, float x1, float y)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::string caption = group.more.empty() ? std::string(group.name) : std::string(group.name) + " " ICON_FA_CARET_DOWN;
    const ImVec2 size = textSize(caption.c_str(), kCaptionSize);
    const ImVec2 pos((x0 + x1 - size.x) * 0.5f, y);
    const ImVec2 backup = ImGui::GetCursorScreenPos();

    bool hovered = false;
    if (!group.more.empty()) {
        ImGui::SetCursorScreenPos(ImVec2(pos.x - 4.0f * s_, pos.y));
        ImGui::PushID("caption");
        if (ImGui::InvisibleButton("##caption", ImVec2(size.x + 8.0f * s_, size.y + 2.0f * s_)))
            ImGui::OpenPopup("##more");
        hovered = ImGui::IsItemHovered();
        if (hovered)
            dl->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), palette_.hover, 3.0f * s_);
        ImGui::SetNextWindowPos(ImVec2(x0, ImGui::GetItemRectMax().y + 2.0f * s_));
        if (ImGui::BeginPopup("##more")) {
            morePopup(group);
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    drawText(dl, pos, hovered ? palette_.text : palette_.textFaint, caption.c_str(), kCaptionSize);
    ImGui::SetCursorScreenPos(backup);
}

} // namespace enp::view
