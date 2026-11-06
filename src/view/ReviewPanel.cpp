#include "enp/view/ReviewPanel.h"

#include "enp/view/DiffView.h"
#include "enp/view/Fit.h"
#include "enp/view/Icons.h"
#include "enp/view/UiAutomation.h"
#include "enp/view/UiStyle.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <cstdio>
#include <string>

namespace enp::view {

using model::ReviewTool;
using model::Severity;
using viewmodel::CommandId;
using viewmodel::PanelTab;

namespace {

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImU32 severityColor(Severity severity, const Palette& p)
{
    switch (severity) {
    case Severity::Error: return p.severityError;
    case Severity::Warning: return p.severityWarning;
    case Severity::Info: break;
    }
    return p.severityInfo;
}

const char* severityIcon(Severity severity)
{
    switch (severity) {
    case Severity::Error: return ICON_FA_CIRCLE_EXCLAMATION;
    case Severity::Warning: return ICON_FA_TRIANGLE_EXCLAMATION;
    case Severity::Info: break;
    }
    return ICON_FA_CIRCLE_INFO;
}

CommandId runCommand(ReviewTool tool)
{
    switch (tool) {
    case ReviewTool::Lint: return CommandId::RunLint;
    case ReviewTool::StaticAnalysis: return CommandId::RunStaticAnalysis;
    case ReviewTool::Security: return CommandId::RunSecurityScan;
    case ReviewTool::Style: return CommandId::RunStyleCheck;
    case ReviewTool::Complexity: return CommandId::RunComplexity;
    case ReviewTool::AgentReview: return CommandId::RunAgentReview;
    }
    return CommandId::RunLint;
}

} // namespace

ReviewPanel::ReviewPanel(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
}

void ReviewPanel::renderTab(PanelTab tab)
{
    switch (tab) {
    case PanelTab::Problems: renderProblems(); break;
    case PanelTab::Diff: renderDiff(); break;
    case PanelTab::Comments: renderComments(); break;
    case PanelTab::Checklist: renderChecklist(); break;
    case PanelTab::Terminal: break; // TerminalView
    case PanelTab::Search: break;   // SearchPanel
    case PanelTab::History: break;  // GitView
    case PanelTab::Status: break;   // GitView
    }
}

// ---- Problems -----------------------------------------------------------------------------------

void ReviewPanel::renderToolChips()
{
    viewmodel::ReviewViewModel& review = vm_.review();
    bool first = true;
    for (ReviewTool tool : model::kAllReviewTools) {
        const CommandId run = runCommand(tool);
        const bool visible = review.toolVisible(tool);
        const std::string label = std::string(commandIcon(run)) + " " + model::reviewToolName(tool) + "  " +
                                  (review.hasRun(tool) ? std::to_string(review.count(tool)) : std::string("-"));
        if (!first)
            fit::sameLineIfFits(fit::buttonWidth(label.c_str(), true), 4.0f * s_);
        first = false;
        ImGui::PushID(int(tool));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 10.0f * s_);
        ImGui::PushStyleColor(ImGuiCol_Button, visible ? col(palette_.accentSoft) : ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, visible ? col(palette_.text) : col(palette_.textFaint));
        if (ImGui::SmallButton(label.c_str()))
            review.setToolVisible(tool, !visible);
        ImGui::PopStyleColor(2);
        ImGui::PopStyleVar();
        if (ImGui::BeginPopupContextItem("##toolMenu")) {
            if (ImGui::MenuItem(ICON_FA_PLAY "  Run now"))
                vm_.execute(run);
            ImGui::EndPopup();
        }
        if (ui::beginItemTooltip()) {
            ImGui::TextUnformatted(model::reviewToolDescription(tool));
            ImGui::TextDisabled("Click to filter, right-click to run");
            ui::endTooltip();
        }
        ImGui::PopID();
    }
    fit::sameLineIfFits(fit::buttonWidth(ICON_FA_CHECK_DOUBLE " Run all", true), 12.0f * s_);
    if (ImGui::SmallButton(ICON_FA_CHECK_DOUBLE " Run all"))
        vm_.execute(CommandId::RunAllReviews);
    fit::sameLineIfFits(ImGui::GetFrameHeight() + ImGui::CalcTextSize("Resolved").x + ImGui::GetStyle().ItemInnerSpacing.x);
    bool showResolved = review.showResolved.get();
    if (ImGui::Checkbox("Resolved", &showResolved))
        review.showResolved.set(showResolved);

    // Severity summary, right-aligned.
    const std::string summary = std::string(ICON_FA_CIRCLE_EXCLAMATION " ") + std::to_string(review.count(Severity::Error)) +
                                "   " ICON_FA_TRIANGLE_EXCLAMATION " " + std::to_string(review.count(Severity::Warning)) +
                                "   " ICON_FA_CIRCLE_INFO " " + std::to_string(review.count(Severity::Info));
    const float w = ImGui::CalcTextSize(summary.c_str()).x;
    fit::sameLineIfFits(w);
    const float x = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w;
    if (x >= ImGui::GetCursorPosX()) {
        ImGui::SetCursorPosX(x);
        ImGui::TextColored(col(palette_.severityError), ICON_FA_CIRCLE_EXCLAMATION " %d", review.count(Severity::Error));
        ImGui::SameLine();
        ImGui::TextColored(col(palette_.severityWarning), ICON_FA_TRIANGLE_EXCLAMATION " %d", review.count(Severity::Warning));
        ImGui::SameLine();
        ImGui::TextColored(col(palette_.severityInfo), ICON_FA_CIRCLE_INFO " %d", review.count(Severity::Info));
    } else {
        ImGui::NewLine();
    }
}

void ReviewPanel::renderProblems()
{
    viewmodel::ReviewViewModel& review = vm_.review();
    renderToolChips();

    const std::vector<int> visible = review.visibleFindings();
    if (visible.empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(review.findings().empty() ? "No findings. Run a review tool from the Review tab."
                                                      : "No findings match the current filter.");
        ImGui::PopTextWrapPos();
        return;
    }
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp;
    const bool narrow = ImGui::GetContentRegionAvail().x < 560.0f * s_; // measured before the table
    if (!ImGui::BeginTable("##findings", 5, flags))
        return;
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 22.0f * s_);
    // A narrow panel leaves the tool out (each row's icon still shows its kind).
    ImGui::TableSetupColumn("Tool", ImGuiTableColumnFlags_WidthStretch | (narrow ? ImGuiTableColumnFlags_Disabled : 0), 1.1f);
    ImGui::TableSetupColumn("Location", ImGuiTableColumnFlags_WidthStretch, 1.6f);
    ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch, 5.0f);
    ImGui::TableSetupColumn("Done", ImGuiTableColumnFlags_WidthFixed, 40.0f * s_);
    ImGui::TableHeadersRow();

    for (int index : visible) {
        const model::ReviewFinding& f = review.findings()[size_t(index)];
        ImGui::PushID(index);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        const bool selected = review.selectedFinding.get() == index;
        if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
            review.openFinding(index);
        ImGui::SameLine(0.0f, 0.0f);
        ImGui::TextColored(col(severityColor(f.severity, palette_)), "%s", severityIcon(f.severity));
        ui::itemTooltip("%s", model::severityName(f.severity));
        ImGui::TableSetColumnIndex(1);
        ImGui::TextDisabled("%s", model::reviewToolName(f.tool));
        ImGui::TableSetColumnIndex(2);
        ImGui::TextColored(col(palette_.accent), "%s:%d", f.file.c_str(), f.line + 1);
        ImGui::TableSetColumnIndex(3);
        if (narrow)
            ImGui::PushTextWrapPos(0.0f);
        if (f.resolved)
            ImGui::TextDisabled("%s", f.message.c_str());
        else
            ImGui::TextUnformatted(f.message.c_str());
        if (narrow)
            ImGui::PopTextWrapPos();
        ui::itemTooltip("%s\n%s", f.rule.c_str(), f.message.c_str());
        ImGui::TableSetColumnIndex(4);
        bool resolved = f.resolved;
        if (ImGui::Checkbox("##resolved", &resolved))
            review.setResolved(index, resolved);
        ui::itemTooltip("Mark as resolved");
        ImGui::PopID();
    }
    ImGui::EndTable();
}

// ---- Diff, comments, checklist ----------------------------------------------------------------

void ReviewPanel::renderDiff()
{
    const viewmodel::ReviewViewModel& review = vm_.review();
    if (review.diffTitle().empty()) {
        ImGui::Spacing();
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("Nothing to compare yet. Use Review > Vs Saved to compare the document with its saved "
                            "version, or Review > Changes to inspect the agent's proposal.");
        ImGui::PopTextWrapPos();
        return;
    }
    const model::DiffStats stats = model::diffStats(review.diff());
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(review.diffTitle().c_str());
    ImGui::SameLine();
    ImGui::TextColored(col(palette_.diffAddedText), "+%d", stats.added);
    ImGui::SameLine(0.0f, 4.0f * s_);
    ImGui::TextColored(col(palette_.diffRemovedText), "-%d", stats.removed);

    // A working-tree or staged git diff: its changes can be staged, unstaged or discarded in parts.
    viewmodel::GitViewModel& git = vm_.git();
    const viewmodel::GitDiffContext& context = git.diffContext();
    const bool gitDiff = context.active && context.title == review.diffTitle() &&
                         (context.area == model::git::Area::Changes || context.area == model::git::Area::Staged);
    if (gitDiff) {
        if (chosenFor_ != context.title) {
            chosenFor_ = context.title;
            chosen_.assign(context.rows.size(), false);
        }
        chosen_.resize(context.rows.size(), false);
        int picked = 0;
        for (bool b : chosen_)
            picked += b ? 1 : 0;
        using Rows = viewmodel::GitViewModel::RowsAction;
        const bool staged = context.area == model::git::Area::Staged;
        char chosenText[64];
        std::snprintf(chosenText, sizeof(chosenText), picked ? "%d line(s) chosen" : "Click lines to choose them", picked);
        fit::sameLineIfFits(ImGui::CalcTextSize(chosenText).x, 16.0f * s_);
        ImGui::TextDisabled("%s", chosenText);
        fit::sameLineIfFitsButton(staged ? ICON_FA_MINUS "  Unstage chosen" : ICON_FA_PLUS "  Stage chosen", true);
        ImGui::BeginDisabled(picked == 0 || git.busy());
        if (ImGui::SmallButton(staged ? ICON_FA_MINUS "  Unstage chosen" : ICON_FA_PLUS "  Stage chosen")) {
            git.applyRows(chosen_, staged ? Rows::Unstage : Rows::Stage);
            chosen_.assign(chosen_.size(), false);
        }
        registerLastItem("diff:stage-chosen", "button", staged ? "Unstage the chosen lines" : "Stage the chosen lines", nullptr);
        if (!staged) {
            fit::sameLineIfFitsButton(ICON_FA_ROTATE_LEFT "  Discard chosen", true);
            if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT "  Discard chosen")) {
                git.applyRows(chosen_, Rows::Discard);
                chosen_.assign(chosen_.size(), false);
            }
            ui::itemTooltip("Take the chosen changes out of the file (save the file first if it is open with edits)");
        }
        ImGui::EndDisabled();
        fit::sameLineIfFitsButton("All", true);
        if (ImGui::SmallButton("All"))
            for (size_t i = 0; i < chosen_.size(); ++i)
                chosen_[i] = context.rows[i].kind != model::DiffKind::Same;
        fit::sameLineIfFitsButton("None", true);
        if (ImGui::SmallButton("None"))
            chosen_.assign(chosen_.size(), false);
    }
    renderSideBySideDiff("##reviewDiff", review.diff(), fonts_, palette_, vm_.zoom.get(), s_, ImVec2(0.0f, 0.0f),
                         gitDiff ? &chosen_ : nullptr);
}

void ReviewPanel::renderComments()
{
    viewmodel::ReviewViewModel& review = vm_.review();
    const model::Document* doc = vm_.activeDocument();
    const float inputW = ImGui::GetContentRegionAvail().x - 120.0f * s_;
    ImGui::SetNextItemWidth(std::max(100.0f * s_, inputW));
    const std::string where = doc ? (doc->path.empty() ? doc->title : doc->path) + ":" + std::to_string(doc->caret.line + 1) : "";
    const std::string hint = doc ? "Comment on " + where : "Open a document to comment";
    const bool enter = ImGui::InputTextWithHint("##newComment", hint.c_str(), &commentDraft_, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    ImGui::BeginDisabled(!doc || commentDraft_.empty());
    if ((ImGui::Button(ICON_FA_COMMENT_DOTS " Add") || enter) && doc && !commentDraft_.empty()) {
        review.addComment(doc->path.empty() ? doc->title : doc->path, doc->caret.line, commentDraft_);
        commentDraft_.clear();
    }
    ImGui::EndDisabled();

    ImGui::BeginChild("##commentList");
    for (int i = 0; i < int(review.comments().size()); ++i) {
        const model::ReviewComment& c = review.comments()[size_t(i)];
        ImGui::PushID(i);
        bool resolved = c.resolved;
        if (ImGui::Checkbox("##resolved", &resolved))
            review.setCommentResolved(i, resolved);
        ui::itemTooltip("Resolved");
        ImGui::SameLine();
        ImGui::TextColored(col(c.author == "Agent" ? palette_.modeAgent : palette_.text), "%s", c.author.c_str());
        ImGui::SameLine();
        const std::string location = c.file + ":" + std::to_string(c.line + 1);
        ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.accent));
        if (ImGui::Selectable(location.c_str(), false, ImGuiSelectableFlags_None, ImGui::CalcTextSize(location.c_str())))
            review.openComment(i);
        ImGui::PopStyleColor();
        ImGui::Indent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        if (c.resolved)
            ImGui::TextDisabled("%s", c.text.c_str());
        else
            ImGui::TextWrapped("%s", c.text.c_str());
        ImGui::Unindent(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x);
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void ReviewPanel::renderChecklist()
{
    viewmodel::ReviewViewModel& review = vm_.review();
    int done = 0;
    for (const model::ChecklistItem& item : review.checklist())
        done += item.done ? 1 : 0;
    const float fraction = review.checklist().empty() ? 0.0f : float(done) / float(review.checklist().size());
    const std::string overlay = std::to_string(done) + " of " + std::to_string(review.checklist().size()) + " done";
    ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), overlay.c_str());
    for (int i = 0; i < int(review.checklist().size()); ++i) {
        bool checked = review.checklist()[size_t(i)].done;
        ImGui::PushID(i);
        if (ImGui::Checkbox(review.checklist()[size_t(i)].text.c_str(), &checked))
            review.setChecklistDone(i, checked);
        ImGui::PopID();
    }
}

} // namespace enp::view
