#include "enp/view/RibbonView.h"

#include "enp/view/Icons.h"

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
}

float RibbonView::height() const
{
    return (kTabRowHeight + kButtonHeight + kCaptionHeight + 10.0f) * s_;
}

const std::vector<RibbonView::Tab>& RibbonView::tabs() const
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
    static const Group view[] = {
        {"Panels", {C::ToggleExplorer, C::ToggleReviewPanel, C::ToggleAgentPanel}, {}},
        {"Display", {C::ToggleLineNumbers, C::ToggleWhitespace, C::ToggleIndentGuides, C::ToggleWordWrap}, {}},
        {"Zoom", {C::ZoomIn, C::ZoomOut, C::ZoomReset}, {}},
        {"Theme", {C::ToggleTheme}, {}},
    };
    static const std::vector<Tab> codeTabs = {
        {"Home",
         {
             {"File", {C::NewFile, C::OpenFile, C::Save, C::CloseFile}, {C::SaveAll}},
             {"Clipboard", {C::Paste, C::Cut, C::Copy}, {C::SelectAll}},
             {"History", {C::Undo, C::Redo}, {}},
             {"Find", {C::Find, C::Replace, C::GoToLine}, {C::FindNext, C::FindPrevious, C::FindInFiles}},
             {"Assist", {C::AskAgent, C::ExplainSelection}, {C::RefactorSelection, C::GenerateTests}},
         }},
        {"Edit",
         {
             {"Lines", {C::DuplicateLine, C::DeleteLine, C::MoveLineUp, C::MoveLineDown}, {C::SortLines}},
             {"Case", {C::UpperCase, C::LowerCase}, {}},
             {"Code", {C::ToggleComment, C::Indent, C::Outdent}, {C::TrimTrailing}},
             {"Bookmarks", {C::ToggleBookmark, C::PreviousBookmark, C::NextBookmark}, {C::ClearBookmarks}},
         }},
        {"Review", {std::begin(review), std::end(review)}},
        {"View", {std::begin(view), std::end(view)}},
        {"Document",
         {
             {"Line Endings", {C::EolCrLf, C::EolLf, C::EolCr}, {}},
             {"Encoding", {C::EncodingUtf8, C::EncodingUtf8Bom, C::EncodingAnsi}, {C::EncodingUtf16Le, C::EncodingUtf16Be}},
             {"Language", {C::LanguagePlainText, C::LanguageCpp, C::LanguagePython, C::LanguageMarkdown},
              {C::LanguageJavaScript, C::LanguageJson, C::LanguageCMake}},
         }},
        {"Tools",
         {
             {"Macro", {C::StartRecording, C::StopRecording, C::PlayMacro}, {}},
             {"Utilities", {C::HashSha256}, {}},
             {"Application", {C::Preferences, C::About}, {C::Exit}},
         }},
    };
    static const std::vector<Tab> agentTabs = {
        {"Agent",
         {
             {"Session", {C::NewChat, C::AskAgent, C::StopAgent}, {}},
             {"Ask", {C::ExplainSelection, C::RefactorSelection, C::GenerateTests, C::FixFindings}, {}},
             {"Changes", {C::CompareAgentChanges, C::AcceptAllChanges, C::RejectAllChanges}, {}},
         }},
        {"Review", {std::begin(review), std::end(review)}},
        {"Files",
         {
             {"File", {C::NewFile, C::OpenFile, C::Save}, {C::SaveAll, C::CloseFile}},
             {"History", {C::Undo, C::Redo}, {}},
             {"Find", {C::Find, C::GoToLine}, {C::Replace, C::FindInFiles}},
         }},
        {"View", {std::begin(view), std::end(view)}},
    };
    return vm_.workspace.get() == WorkspaceMode::Agent ? agentTabs : codeTabs;
}

int& RibbonView::selectedTab()
{
    int& index = selectedTab_[vm_.workspace.get() == WorkspaceMode::Agent ? 0 : 1];
    index = std::clamp(index, 0, int(tabs().size()) - 1);
    return index;
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

    const float blockWidth = kWorkspaceWidth * s_;
    ImGui::SetCursorScreenPos(origin);
    renderWorkspaceBlock(blockWidth, h - 1.0f);
    dl->AddLine(ImVec2(origin.x + blockWidth, origin.y + 8.0f * s_), ImVec2(origin.x + blockWidth, origin.y + h - 8.0f * s_),
                palette_.border);

    const float left = origin.x + blockWidth + 8.0f * s_;
    ImGui::SetCursorScreenPos(ImVec2(left, origin.y + 3.0f * s_));
    renderTabRow(origin.x + width - left - 8.0f * s_);

    ImGui::SetCursorScreenPos(ImVec2(left, origin.y + (kTabRowHeight + 4.0f) * s_));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 4.0f * s_);
    ImGui::BeginChild("##ribbonGroups", ImVec2(origin.x + width - left, (kButtonHeight + kCaptionHeight + 4.0f) * s_),
                      ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
    renderGroups(tabs()[size_t(selectedTab())]);
    ImGui::EndChild();
    ImGui::PopStyleVar(2);

    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(width, h));
    ImGui::PopID();
}

void RibbonView::renderWorkspaceBlock(float width, float height)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool agent = vm_.workspace.get() == WorkspaceMode::Agent;
    ImGui::InvisibleButton("##workspace", ImVec2(width, height));
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
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const std::vector<Tab>& list = tabs();
    int& selected = selectedTab();
    const ImVec2 start = ImGui::GetCursorScreenPos();
    float x = start.x;
    const float pillH = (kTabRowHeight - 6.0f) * s_;
    for (int i = 0; i < int(list.size()); ++i) {
        const ImVec2 size = ImGui::CalcTextSize(list[size_t(i)].name);
        const float w = size.x + 24.0f * s_;
        ImGui::SetCursorScreenPos(ImVec2(x, start.y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab", ImVec2(w, pillH)))
            selected = i;
        ImGui::PopID();
        const bool active = i == selected;
        if (active)
            dl->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + pillH), palette_.tabActiveBg, 5.0f * s_);
        else if (ImGui::IsItemHovered())
            dl->AddRectFilled(ImVec2(x, start.y), ImVec2(x + w, start.y + pillH), palette_.hover, 5.0f * s_);
        dl->AddText(ImVec2(x + 12.0f * s_, start.y + (pillH - size.y) * 0.5f), active ? palette_.text : palette_.textDim,
                    list[size_t(i)].name);
        x += w + 2.0f * s_;
    }

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
        for (CommandId id : group.buttons) {
            ImGui::SetCursorScreenPos(ImVec2(x, top));
            if (ribbonButton(id))
                vm_.execute(id);
            x = ImGui::GetItemRectMax().x + 2.0f * s_;
        }
        const std::string caption = group.more.empty() ? std::string(group.name) : std::string(group.name) + " " ICON_FA_CARET_DOWN;
        const float captionW = textSize(caption.c_str(), kCaptionSize).x + 8.0f * s_;
        const float x1 = std::max(x - 2.0f * s_, x0 + captionW);
        groupCaption(group, x0, x1, top + kButtonHeight * s_ + 1.0f * s_);
        ImGui::PopID();

        const float sepX = x1 + 8.0f * s_;
        if (g + 1 < tab.groups.size())
            dl->AddLine(ImVec2(sepX, top + 6.0f * s_), ImVec2(sepX, top + (kButtonHeight + kCaptionHeight - 2.0f) * s_),
                        palette_.border);
        x = sepX + 8.0f * s_;
    }
    ImGui::SetCursorScreenPos(ImVec2(start.x, top));
    ImGui::Dummy(ImVec2(x - start.x, (kButtonHeight + kCaptionHeight) * s_)); // scrollable extent
}

bool RibbonView::ribbonButton(CommandId id)
{
    const viewmodel::CommandInfo& info = commandInfo(id);
    const bool enabled = vm_.command(id).canExecute();
    const bool checked = vm_.isChecked(id);
    const ImVec2 labelSize = textSize(info.label, kLabelSize);
    const float w = std::max(kButtonMinWidth * s_, labelSize.x + 14.0f * s_);
    const float h = kButtonHeight * s_;

    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushID(int(id));
    const bool clicked = ImGui::InvisibleButton("##ribbonButton", ImVec2(w, h));
    ImGui::PopID();
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled);
    const bool held = ImGui::IsItemActive();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 q(p.x + w, p.y + h);
    if (checked)
        dl->AddRectFilled(p, q, palette_.accentSoft, 6.0f * s_);
    if (hovered && enabled)
        dl->AddRectFilled(p, q, held ? fade(palette_.hover, 2.0f) : palette_.hover, 6.0f * s_);

    const char* icon = fonts_.hasIcons ? commandIcon(id) : "?";
    const ImVec2 iconSize = textSize(icon, Fonts::kRibbonIconSize);
    const ImU32 tint = commandTint(id, palette_);
    drawText(dl, ImVec2(p.x + (w - iconSize.x) * 0.5f, p.y + 5.0f * s_), enabled ? tint : fade(tint, 0.35f), icon,
             Fonts::kRibbonIconSize);
    drawText(dl, ImVec2(p.x + (w - labelSize.x) * 0.5f, q.y - labelSize.y - 5.0f * s_),
             enabled ? palette_.text : palette_.textFaint, info.label, kLabelSize);

    if (hovered && ImGui::BeginTooltip()) {
        ImGui::TextUnformatted(info.label);
        if (info.shortcut[0]) {
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", info.shortcut);
        }
        ImGui::TextDisabled("%s", info.tooltip);
        ImGui::EndTooltip();
    }
    return clicked && enabled;
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
            for (CommandId id : group.more) {
                const viewmodel::CommandInfo& info = commandInfo(id);
                const std::string label = std::string(commandIcon(id)) + "  " + info.label;
                if (ImGui::MenuItem(label.c_str(), info.shortcut, vm_.isChecked(id), vm_.command(id).canExecute()))
                    vm_.execute(id);
            }
            ImGui::EndPopup();
        }
        ImGui::PopID();
    }
    drawText(dl, pos, hovered ? palette_.text : palette_.textFaint, caption.c_str(), kCaptionSize);
    ImGui::SetCursorScreenPos(backup);
}

} // namespace enp::view
