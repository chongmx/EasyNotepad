#include "enp/view/AgentPanel.h"

#include "enp/view/DiffView.h"
#include "enp/view/Icons.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>

namespace enp::view {

using viewmodel::CommandId;

namespace {

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

const char* roleName(model::ChatRole role)
{
    switch (role) {
    case model::ChatRole::User: return ICON_FA_USER "  You";
    case model::ChatRole::Agent: return ICON_FA_ROBOT "  Agent";
    case model::ChatRole::Tool: return ICON_FA_TERMINAL "  Tool";
    }
    return "";
}

} // namespace

AgentPanel::AgentPanel(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

// ---- Conversation -----------------------------------------------------------------------------

void AgentPanel::renderConversation(bool compact)
{
    renderHeader(compact);
    const float composerH = ImGui::GetTextLineHeightWithSpacing() * 4.0f + ImGui::GetFrameHeightWithSpacing() * 1.6f;
    renderMessages(std::max(40.0f * s_, ImGui::GetContentRegionAvail().y - composerH));
    renderComposer();
}

void AgentPanel::renderHeader(bool compact)
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    const float comboW = compact ? ImGui::GetContentRegionAvail().x * 0.5f - 4.0f * s_ : 160.0f * s_;
    int model = agent.modelIndex.get();
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("##model", &model, agent.modelNames().data(), int(agent.modelNames().size())))
        agent.modelIndex.set(model);
    ImGui::SetItemTooltip("Model (dummy: not connected)");
    ImGui::SameLine();
    int autonomy = agent.autonomyIndex.get();
    ImGui::SetNextItemWidth(comboW);
    if (ImGui::Combo("##autonomy", &autonomy, agent.autonomyNames().data(), int(agent.autonomyNames().size())))
        agent.autonomyIndex.set(autonomy);
    ImGui::SetItemTooltip("How much the agent may do without asking");
    if (!compact) {
        ImGui::SameLine();
        ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x -
                                                                  ImGui::CalcTextSize(ICON_FA_PLUS " New chat").x -
                                                                  ImGui::GetStyle().FramePadding.x * 2.0f));
        if (ImGui::Button(ICON_FA_PLUS " New chat"))
            vm_.execute(CommandId::NewChat);
    }
}

void AgentPanel::bubble(const model::ChatMessage& message, float width)
{
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float pad = 8.0f * s_;
    const bool user = message.role == model::ChatRole::User;
    const float bubbleW = user ? width * 0.86f : width;
    const float x0 = ImGui::GetCursorScreenPos().x + (user ? width - bubbleW : 0.0f);

    ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    dl->ChannelsSplit(2);
    dl->ChannelsSetCurrent(1);
    ImGui::SetCursorScreenPos(ImVec2(start.x + pad, start.y + pad));
    ImGui::BeginGroup();
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    ImGui::TextColored(col(message.role == model::ChatRole::Agent ? palette_.modeAgent : palette_.textDim), "%s",
                       roleName(message.role));
    ImGui::PopFont();
    // The wrap position is window-local, not screen space.
    ImGui::PushTextWrapPos(start.x + bubbleW - pad - ImGui::GetWindowPos().x + ImGui::GetScrollX());
    if (message.role == model::ChatRole::Tool)
        ImGui::TextDisabled("%s", message.text.c_str());
    else
        ImGui::TextUnformatted(message.text.c_str());
    ImGui::PopTextWrapPos();
    ImGui::EndGroup();
    const ImVec2 end(start.x + bubbleW, ImGui::GetItemRectMax().y + pad);
    dl->ChannelsSetCurrent(0);
    const ImU32 bg = user ? palette_.bubbleUser : message.role == model::ChatRole::Agent ? palette_.bubbleAgent : palette_.bubbleTool;
    dl->AddRectFilled(start, end, bg, 8.0f * s_);
    dl->ChannelsMerge();
    ImGui::SetCursorScreenPos(ImVec2(ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x, end.y + 8.0f * s_));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
}

void AgentPanel::renderMessages(float height)
{
    const viewmodel::AgentViewModel& agent = vm_.agent();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f * s_, 6.0f * s_));
    ImGui::BeginChild("##messages", ImVec2(0.0f, height), ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    const float width = ImGui::GetContentRegionAvail().x;
    if (agent.messages().empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s  Describe what you want to build or fix.", ICON_FA_LIGHTBULB);
        ImGui::TextDisabled("The agent plans, edits and runs the review tools;");
        ImGui::TextDisabled("you review the changes before they land.");
    }
    for (const model::ChatMessage& message : agent.messages())
        bubble(message, width);
    if (agent.busy.get()) {
        static const char* const dots[] = {".", "..", "..."};
        ImGui::TextColored(col(palette_.modeAgent), ICON_FA_ROBOT "  Thinking%s", dots[int(ImGui::GetTime() * 3.0) % 3]);
    }
    if (agent.messages().size() != lastMessageCount_ || agent.busy.get()) {
        lastMessageCount_ = agent.messages().size();
        ImGui::SetScrollHereY(1.0f);
    }
    ImGui::EndChild();
}

void AgentPanel::renderComposer()
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    const std::string context = vm_.selectionContextLabel();
    if (!context.empty()) {
        ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
        ImGui::TextColored(col(palette_.textDim), ICON_FA_FILE_CODE " %s", context.c_str());
        ImGui::PopFont();
        ImGui::SetItemTooltip("Context sent with the prompt: the active file and selected lines");
    }
    if (focusPrompt_) {
        ImGui::SetKeyboardFocusHere();
        focusPrompt_ = false;
    }
    const float buttonW = ImGui::GetFrameHeight() * 1.6f;
    const float inputH = ImGui::GetTextLineHeight() * 3.0f + ImGui::GetStyle().FramePadding.y * 2.0f;
    const bool send = ImGui::InputTextMultiline(
        "##prompt", &prompt_, ImVec2(ImGui::GetContentRegionAvail().x - buttonW - 4.0f * s_, inputH),
        ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CtrlEnterForNewLine | ImGuiInputTextFlags_WordWrap);
    if (prompt_.empty() && !ImGui::IsItemActive()) {
        const ImVec2 p = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + ImGui::GetStyle().FramePadding.x, p.y + ImGui::GetStyle().FramePadding.y),
                                            palette_.textFaint, "Ask the agent...  (Enter to send, Ctrl+Enter for a new line)");
    }
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (agent.busy.get()) {
        ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.iconRed));
        if (ImGui::Button(ICON_FA_STOP "##stop", ImVec2(buttonW, inputH)))
            vm_.execute(CommandId::StopAgent);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Stop the agent");
    } else {
        ImGui::BeginDisabled(prompt_.empty());
        ImGui::PushStyleColor(ImGuiCol_Button, col(palette_.accent));
        const bool clicked = ImGui::Button(ICON_FA_PAPER_PLANE "##send", ImVec2(buttonW, inputH));
        ImGui::PopStyleColor();
        ImGui::EndDisabled();
        ImGui::SetItemTooltip("Send (Enter)");
        if ((send || clicked) && !prompt_.empty()) {
            agent.submitPrompt(prompt_, context);
            prompt_.clear();
            focusPrompt_ = true;
        }
    }
}

// ---- Plan and changes -------------------------------------------------------------------------

void AgentPanel::renderPlan()
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    ImGui::TextDisabled("PLAN");
    if (agent.tasks().empty())
        ImGui::TextDisabled("No plan yet.");
    for (const model::AgentTask& task : agent.tasks()) {
        switch (task.state) {
        case model::TaskState::Done:
            ImGui::TextColored(col(palette_.iconGreen), ICON_FA_CIRCLE_CHECK);
            break;
        case model::TaskState::Running:
            ImGui::TextColored(col(palette_.accent), ICON_FA_CIRCLE_NOTCH);
            break;
        case model::TaskState::Pending:
            ImGui::TextColored(col(palette_.textFaint), ICON_FA_CIRCLE);
            break;
        }
        ImGui::SameLine();
        if (task.state == model::TaskState::Done)
            ImGui::TextDisabled("%s", task.title.c_str());
        else
            ImGui::TextUnformatted(task.title.c_str());
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("CHANGES  %d pending", agent.pendingChangeCount());
    for (int i = 0; i < int(agent.changes().size()); ++i) {
        const model::FileChange& change = agent.changes()[size_t(i)];
        ImGui::PushID(i);
        const bool selected = agent.selectedChange.get() == i;
        const ImVec2 p = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##change", selected, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, ImGui::GetTextLineHeight())))
            agent.selectedChange.set(i);
        ImGui::SetCursorScreenPos(p);
        const char* icon = change.state == model::ChangeState::Accepted ? ICON_FA_CHECK
                           : change.state == model::ChangeState::Rejected ? ICON_FA_BAN
                                                                          : ICON_FA_PEN;
        const ImU32 iconColor = change.state == model::ChangeState::Accepted ? palette_.iconGreen
                                : change.state == model::ChangeState::Rejected ? palette_.textFaint
                                                                               : palette_.iconOrange;
        ImGui::TextColored(col(iconColor), "%s", icon);
        ImGui::SameLine();
        ImGui::TextUnformatted(change.path.c_str());
        ImGui::SameLine();
        ImGui::TextColored(col(palette_.diffAddedText), "+%d", change.stats.added);
        ImGui::SameLine(0.0f, 4.0f * s_);
        ImGui::TextColored(col(palette_.diffRemovedText), "-%d", change.stats.removed);
        ImGui::PopID();
    }
    ImGui::Spacing();
    const float half = ImGui::GetContentRegionAvail().x * 0.5f - 2.0f * s_;
    ImGui::BeginDisabled(!vm_.command(CommandId::AcceptAllChanges).canExecute());
    if (ImGui::Button(ICON_FA_CHECK_DOUBLE " Accept all", ImVec2(half, 0.0f)))
        vm_.execute(CommandId::AcceptAllChanges);
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (ImGui::Button(ICON_FA_BAN " Reject all", ImVec2(half, 0.0f)))
        vm_.execute(CommandId::RejectAllChanges);
    ImGui::EndDisabled();
}

void AgentPanel::renderChangePreview(ImVec2 size)
{
    viewmodel::AgentViewModel& agent = vm_.agent();
    const model::FileChange* change = agent.currentChange();
    if (!change) {
        ImGui::TextDisabled("Select a change to review it.");
        return;
    }
    const float startY = ImGui::GetCursorPosY();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(change->path.c_str());
    ImGui::SameLine();
    ImGui::TextColored(col(palette_.diffAddedText), "+%d", change->stats.added);
    ImGui::SameLine(0.0f, 4.0f * s_);
    ImGui::TextColored(col(palette_.diffRemovedText), "-%d", change->stats.removed);
    ImGui::SameLine();
    const char* state = change->state == model::ChangeState::Accepted   ? "Accepted"
                        : change->state == model::ChangeState::Rejected ? "Rejected"
                                                                        : "Proposed";
    ImGui::TextDisabled("%s", state);

    const float buttonsW = 3.0f * 86.0f * s_;
    ImGui::SameLine();
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - buttonsW));
    const int index = agent.selectedChange.get();
    ImGui::BeginDisabled(change->state != model::ChangeState::Proposed);
    ImGui::PushStyleColor(ImGuiCol_Button, col(palette_.diffAddedBg));
    if (ImGui::Button(ICON_FA_CHECK " Accept"))
        agent.acceptChange(index);
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (ImGui::Button(ICON_FA_XMARK " Reject"))
        agent.rejectChange(index);
    ImGui::EndDisabled();
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (ImGui::Button(ICON_FA_FILE_CODE " Open"))
        vm_.openFile(change->path);
    ImGui::SetItemTooltip("Open the file in the editor");

    const float used = ImGui::GetCursorPosY() - startY;
    renderSideBySideDiff("##changeDiff", change->diff, fonts_, palette_, vm_.zoom.get(), s_,
                         ImVec2(size.x, std::max(1.0f, size.y - used)));
}

} // namespace enp::view
