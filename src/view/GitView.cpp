#include "enp/view/GitView.h"

#include "enp/view/Fit.h"

#include "IconsFontAwesome6.h"
#include "misc/cpp/imgui_stdlib.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <ctime>

namespace enp::view {

using viewmodel::GitOperation;
using viewmodel::GitProblem;
using model::git::RefKind;

namespace {

ImVec4 col(ImU32 c) { return ImGui::ColorConvertU32ToFloat4(c); }

ImU32 withAlpha(ImU32 c, int alpha) { return (c & ~IM_COL32_A_MASK) | (ImU32(alpha) << IM_COL32_A_SHIFT); }

// Right edge of the content area in window coordinates (GetContentRegionMax() is gone from ImGui).
float contentMaxX() { return ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x; }

std::string fileName(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string folderOf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}

bool containsNoCase(const std::string& text, const std::string& needle)
{
    auto lower = [](std::string s) {
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    return needle.empty() || lower(text).find(lower(needle)) != std::string::npos;
}

std::string plural(int n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

float smallButtonWidth(const std::string& label) { return ImGui::CalcTextSize(label.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f; }

std::vector<std::string> splitRefs(const std::string& refs)
{
    std::vector<std::string> out;
    size_t from = 0;
    while (from < refs.size()) {
        const size_t comma = refs.find(", ", from);
        out.push_back(refs.substr(from, comma == std::string::npos ? std::string::npos : comma - from));
        from = comma == std::string::npos ? refs.size() : comma + 2;
    }
    return out;
}

} // namespace

GitView::GitView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale)
    : vm_(vm), fonts_(fonts), palette_(palette), s_(uiScale)
{
    subscriptions_.push_back(vm_.gitStashRequested.subscribe([this]() { openStash_ = true; }));
    subscriptions_.push_back(vm_.gitGuideRequested.subscribe([this]() { openGuide(); }));
    subscriptions_.push_back(vm_.gitShowCommitRequested.subscribe([this](const std::string& hash) {
        historyMode_ = 0;
        historyPath_.clear();
        historyFilter_ = hash.substr(0, 7);
        git().loadLog(historyBranch_ == 1, historyPath_);
        vm_.showPanelTab(viewmodel::PanelTab::History);
    }));
    subscriptions_.push_back(vm_.gitFileHistoryRequested.subscribe([this](const std::string& path) { showFileHistory(path); }));
    subscriptions_.push_back(vm_.gitDiscardAllRequested.subscribe([this]() { discardWithConfirm(git().unstagedPaths()); }));
    subscriptions_.push_back(vm_.branchPickerRequested.subscribe([this]() {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        openBranchPicker(ImVec2(vp->Pos.x + vp->Size.x * 0.5f - 220.0f * s_, vp->Pos.y + 150.0f * s_), false);
    }));
}

int GitView::changeCount() const { return git().changeCount(); }
const std::string& GitView::branch() const { return repo().status.branch; }
int GitView::ahead() const { return repo().status.ahead; }
int GitView::behind() const { return repo().status.behind; }
bool GitView::merging() const { return repo().operation != GitOperation::None; }
bool GitView::demo() const { return git().demo(); }
bool GitView::repository() const { return git().available() && repo().repository; }

ImU32 GitView::statusColor(char status) const
{
    switch (status) {
    case 'A':
    case 'U': return palette_.diffAddedText;
    case 'D': return palette_.diffRemovedText;
    case 'R': return palette_.iconBlue;
    case '!': return palette_.iconOrange;
    default: return palette_.severityWarning;
    }
}

bool GitView::iconButton(const char* id, const char* icon, const char* tooltip)
{
    ImGui::PushID(id);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.textDim));
    const float size = ImGui::GetFrameHeight();
    const bool clicked = ImGui::Button(icon, ImVec2(size, size));
    ImGui::PopStyleColor(2);
    ImGui::SetItemTooltip("%s", tooltip);
    ImGui::PopID();
    return clicked;
}

bool GitView::hasEdits(const std::string& path) const
{
    for (const Change& c : repo().status.changes)
        if (c.path == path)
            return true;
    return false;
}

// ---- Confirmations and questions ----------------------------------------------------------------

void GitView::confirm(std::string title, std::string text, std::string button, std::function<void()> action, bool danger)
{
    confirm_ = {std::move(title), std::move(text), std::move(button), danger, std::move(action)};
    openConfirm_ = true;
}

void GitView::ask(std::string title, std::string label, std::string value, std::string button,
                  std::function<void(const std::string&)> action, bool branchName)
{
    input_ = {std::move(title), std::move(label), std::move(value), std::move(button), branchName, std::move(action)};
    openInput_ = true;
}

void GitView::discardWithConfirm(const std::vector<std::string>& paths)
{
    if (paths.empty())
        return;
    const std::string what = paths.size() == 1 ? fileName(paths[0]) : plural(int(paths.size()), "file", "files");
    confirm("Discard changes?", "Discard the changes to " + what + "? This cannot be undone: the edits are gone for good.",
            "Discard", [this, paths]() {
                git().discard(paths);
                for (const std::string& p : paths)
                    selected_.erase(p);
            });
}

void GitView::renderConfirm()
{
    if (openConfirm_) {
        ImGui::OpenPopup("Please confirm##gitConfirm");
        openConfirm_ = false;
    }
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(440.0f * s_, 0.0f)));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Please confirm##gitConfirm", nullptr, ImGuiWindowFlags_NoResize))
        return;
    ImGui::PushFont(nullptr, Fonts::kUiSize * 1.1f);
    ImGui::TextColored(col(confirm_.danger ? palette_.severityWarning : palette_.text), "%s%s",
                       confirm_.danger ? ICON_FA_TRIANGLE_EXCLAMATION "  " : "", confirm_.title.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", confirm_.text.c_str());
    ImGui::Spacing();
    const float w = std::max(110.0f * s_, smallButtonWidth(confirm_.button) + 20.0f * s_);
    ImGui::SetCursorPosX(contentMaxX() - w - 100.0f * s_ - ImGui::GetStyle().ItemSpacing.x);
    if (confirm_.danger)
        ImGui::PushStyleColor(ImGuiCol_Button, col(withAlpha(palette_.severityError, 170)));
    if (ImGui::Button(confirm_.button.c_str(), ImVec2(w, 0.0f))) {
        if (confirm_.action)
            confirm_.action();
        ImGui::CloseCurrentPopup();
    }
    if (confirm_.danger)
        ImGui::PopStyleColor();
    registerLastItem("git:confirm", "button", confirm_.button, nullptr);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    registerLastItem("git:confirm-cancel", "button", "Cancel", nullptr);
    ImGui::EndPopup();
}

void GitView::renderInput()
{
    if (openInput_) {
        ImGui::OpenPopup("##gitInput");
        openInput_ = false;
    }
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(420.0f * s_, 0.0f)));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("##gitInput", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    ImGui::TextUnformatted(input_.title.c_str());
    ImGui::Spacing();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    const bool enter = ImGui::InputTextWithHint("##value", input_.label.c_str(), &input_.value, ImGuiInputTextFlags_EnterReturnsTrue);
    registerLastItem("git:input", "textbox", input_.title, nullptr);
    const bool valid = !input_.value.empty() && (!input_.branchName || model::git::validBranchName(input_.value));
    if (!input_.value.empty() && !valid)
        ImGui::TextColored(col(palette_.severityError), ICON_FA_CIRCLE_XMARK "  Not a valid name (no spaces, .., ~ ^ : ? * [ or \\)");
    ImGui::Spacing();
    const float w = 100.0f * s_;
    ImGui::SetCursorPosX(contentMaxX() - 2.0f * w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button(input_.button.c_str(), ImVec2(w, 0.0f)) || (enter && valid)) {
        if (input_.action)
            input_.action(input_.value);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void GitView::renderProblem()
{
    // Things git cannot do without the user: shown once, with the ways forward.
    const GitProblem problem = git().problem();
    if (problem.kind == GitProblem::Kind::None)
        return;
    if (!ImGui::IsPopupOpen("##gitProblem"))
        ImGui::OpenPopup("##gitProblem");
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(470.0f * s_, 0.0f)));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("##gitProblem", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoTitleBar))
        return;
    std::string title, text, primary;
    std::function<void()> action;
    switch (problem.kind) {
    case GitProblem::Kind::SwitchBlocked:
        title = "Your changes are in the way";
        text = "Switching to " + problem.subject + " would overwrite files you changed and have not committed. "
               "Put them aside in a stash first (you can take them back later), or commit them.";
        primary = "Stash and switch";
        action = [this, name = problem.subject]() { git().stashAndSwitch(name); };
        break;
    case GitProblem::Kind::BranchNotMerged:
        title = problem.subject + " is not merged";
        text = problem.detail + " Delete it anyway?";
        primary = "Delete anyway";
        action = [this, name = problem.subject]() { git().deleteBranch(name, true); };
        break;
    case GitProblem::Kind::AuthFailed:
        title = "Sign-in needed";
        text = "The server needs you to sign in, and git cannot ask for a password here. Run the same command in the "
               "terminal, where it can ask; your credential helper may remember it afterwards.";
        primary = "Run in terminal";
        action = [this]() { git().runFailedInTerminal(); };
        break;
    case GitProblem::Kind::PushRejected:
        title = "The server has commits you don't have";
        text = "Someone pushed to " + problem.subject + " since you last pulled. Pull their commits first (git combines "
               "them with yours), then push again.";
        primary = "Pull, then push";
        action = [this]() { git().sync(); };
        break;
    case GitProblem::Kind::Failed:
    case GitProblem::Kind::None:
        title = problem.subject + " did not work";
        text = problem.detail;
        break;
    }
    ImGui::PushFont(nullptr, Fonts::kUiSize * 1.1f);
    ImGui::TextColored(col(palette_.severityWarning), ICON_FA_TRIANGLE_EXCLAMATION "  %s", title.c_str());
    ImGui::PopFont();
    ImGui::Spacing();
    ImGui::TextWrapped("%s", text.c_str());
    if (!git().lastOutput().empty()) {
        if (ImGui::TreeNode("Show git's output")) {
            ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * 0.85f);
            ImGui::BeginChild("##output", ImVec2(0.0f, 120.0f * s_), ImGuiChildFlags_Borders);
            ImGui::TextUnformatted(git().lastOutput().c_str());
            ImGui::EndChild();
            ImGui::PopFont();
            ImGui::TreePop();
        }
    }
    ImGui::Spacing();
    const float w = primary.empty() ? 0.0f : std::max(120.0f * s_, smallButtonWidth(primary) + 20.0f * s_);
    ImGui::SetCursorPosX(contentMaxX() - w - 100.0f * s_ - (primary.empty() ? 0.0f : ImGui::GetStyle().ItemSpacing.x));
    if (!primary.empty()) {
        if (ImGui::Button(primary.c_str(), ImVec2(w, 0.0f))) {
            git().clearProblem();
            action();
            ImGui::CloseCurrentPopup();
        }
        registerLastItem("git:problem-action", "button", primary, nullptr);
        ImGui::SameLine();
    }
    if (ImGui::Button(primary.empty() ? "OK" : "Cancel", ImVec2(100.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        git().clearProblem();
        ImGui::CloseCurrentPopup();
    }
    registerLastItem("git:problem-close", "button", "Close", [this]() { git().clearProblem(); });
    ImGui::EndPopup();
}

// ---- Shared: file rows and the per-file actions ------------------------------------------------

void GitView::fileActionItems(const FileChange& file, const std::string& ref, Source source)
{
    // The same actions wherever a file is listed under a commit, a stash or a comparison.
    const std::string& path = file.path;
    const std::string name = fileName(path);
    const std::string where = source == Source::Commit && ref.size() > 7 ? ref.substr(0, 7) : ref;
    if (ImGui::MenuItem(ICON_FA_FILE_LINES "  Open this version", nullptr, false, file.status != 'D'))
        git().openVersion(ref, path);
    if (ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Compare with my copy"))
        git().compareWithWorkingTree(ref, path);
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Restore this file from here...", nullptr, false, file.status != 'D')) {
        auto restore = [this, ref, path]() {
            git().restoreFile(ref, path);
            tookFromCompare_ = true;
        };
        if (hasEdits(path))
            confirm("Replace your copy?", name + " has changes you have not committed. Replace it with the version in " + where +
                                              "? Your changes to it are lost.",
                    "Replace", restore);
        else
            restore();
    }
    ImGui::SetItemTooltip("Replace your copy with this version");
    if (source != Source::Compare) {
        if (ImGui::MenuItem(ICON_FA_CODE_PULL_REQUEST "  Apply only this file's change")) {
            git().applyFileChange(ref, path);
            tookFromCompare_ = true;
        }
        ImGui::SetItemTooltip("Apply this change to the file, on top of your copy");
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_CLOCK_ROTATE_LEFT "  File history"))
        showFileHistory(path);
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy path"))
        ImGui::SetClipboardText(path.c_str());
}

void GitView::fileRow(const FileChange& file, const std::string& ref, Source source, bool* check)
{
    ImGui::PushID(file.path.c_str());
    if (check) {
        ImGui::Checkbox("##pick", check);
        ImGui::SetItemTooltip("Include this file");
        ImGui::SameLine(0.0f, 4.0f * s_);
    }
    const ImVec2 start = ImGui::GetCursorScreenPos();
    auto open = [this, file, ref, source]() {
        sawDiff_ = true;
        if (source == Source::Compare)
            git().showFileBetween(repo().compareBase, ref, file);
        else
            git().showCommitFile(ref, file);
    };
    if (ImGui::Selectable("##file", false, ImGuiSelectableFlags_AllowOverlap))
        open();
    ImGui::SetItemTooltip("%s\nClick: what changed  ·  right-click: open, restore or apply this file", file.path.c_str());
    registerLastItem("git:file@" + ref + ":" + file.path, "row", file.path, open);
    if (ImGui::BeginPopupContextItem("##fileActions")) {
        fileActionItems(file, ref, source);
        ImGui::EndPopup();
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float textY = start.y + (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f;
    const char letter[2] = {file.status, 0};
    dl->AddText(ImVec2(start.x, textY), statusColor(file.status), letter);
    const std::string name = fileName(file.path);
    const float nameW = ImGui::CalcTextSize(name.c_str()).x;
    dl->AddText(ImVec2(start.x + 16.0f * s_, textY), palette_.text, name.c_str());
    const float right = ImGui::GetItemRectMax().x;
    const float countsX = right - 84.0f * s_;
    dl->PushClipRect(ImVec2(start.x, start.y), ImVec2(countsX - 6.0f * s_, start.y + 100.0f), true);
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    dl->AddText(ImVec2(start.x + 24.0f * s_ + nameW, textY + 1.0f * s_), palette_.textFaint, folderOf(file.path).c_str());
    ImGui::PopFont();
    dl->PopClipRect();
    // Line counts at the right when there is room (narrow lists keep them in the diff).
    if (start.x + 24.0f * s_ + nameW < countsX && (file.added || file.removed)) {
        const std::string plus = "+" + std::to_string(file.added), minus = "-" + std::to_string(file.removed);
        dl->AddText(ImVec2(countsX, textY), palette_.diffAddedText, plus.c_str());
        dl->AddText(ImVec2(right - 38.0f * s_, textY), palette_.diffRemovedText, minus.c_str());
    }
    ImGui::PopID();
}

// ---- Git tab ------------------------------------------------------------------------------------

void GitView::renderChanges()
{
    ImGui::PushID("git");
    git().setVisible(true);
    if (demo())
        renderDemoBanner();
    if (vm_.noFolder() && !demo()) {
        ImGui::Spacing();
        ImGui::TextDisabled("No folder open.");
        ImGui::TextWrapped("Open a folder to see its changes, branches and history.");
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_FOLDER_OPEN "  Open a folder", ImVec2(-1.0f, 0.0f)))
            vm_.execute(viewmodel::CommandId::OpenFolder);
        if (ImGui::Button(ICON_FA_CLONE "  Clone a repository...", ImVec2(-1.0f, 0.0f)))
            openClone_ = true;
        if (ImGui::Button(ICON_FA_GRADUATION_CAP "  Learn git with demo data", ImVec2(-1.0f, 0.0f)))
            openGuide();
        ImGui::PopID();
        return;
    }
    if (!git().available()) {
        ImGui::Spacing();
        ImGui::TextColored(col(palette_.severityWarning), ICON_FA_TRIANGLE_EXCLAMATION "  Git was not found.");
        ImGui::TextWrapped("Install git (git-scm.com), or set EASYNOTEPAD_GIT to its full path, then open the folder again.");
        if (!git().unavailableReason().empty())
            ImGui::TextDisabled("%s", git().unavailableReason().c_str());
        ImGui::Spacing();
        if (ImGui::Button(ICON_FA_GRADUATION_CAP "  Learn git with demo data", ImVec2(-1.0f, 0.0f)))
            openGuide();
        ImGui::PopID();
        return;
    }
    if (!repo().repository) {
        renderNotRepository();
        ImGui::PopID();
        return;
    }
    renderBranchStrip();
    if (merging())
        renderMergeBanner();
    renderCommitBox();
    ImGui::Separator();
    renderSelectionBar();

    ImGui::BeginChild("##gitLists", ImVec2(0.0f, 0.0f));
    if (merging() || std::any_of(repo().status.changes.begin(), repo().status.changes.end(), [](const Change& c) { return c.group == Area::Conflicts; }))
        renderGroup(Area::Conflicts, "CONFLICTS");
    renderGroup(Area::Staged, "STAGED");
    renderGroup(Area::Changes, "CHANGES");
    renderGroup(Area::Untracked, "UNTRACKED");
    if (repo().status.changes.empty())
        ImGui::TextDisabled(ICON_FA_CIRCLE_CHECK "  Nothing to commit, working tree clean");
    renderStashes();
    ImGui::EndChild();
    ImGui::PopID();
}

void GitView::renderBranchStrip()
{
    const auto& status = repo().status;
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(palette_.iconPurple), ICON_FA_CODE_BRANCH);
    ImGui::SameLine(0.0f, 6.0f * s_);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    const std::string label = (status.detached ? "detached at " : "") + status.branch + "  " ICON_FA_CARET_DOWN;
    if (ImGui::Button(label.c_str()))
        openBranchPicker(ImVec2(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y), false);
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip(status.detached ? "You are on a commit, not a branch: switch to a branch to keep working"
                                          : "Switch, create or merge branches");
    registerLastItem("git:branch", "button", "Branches", [this]() { vm_.execute(viewmodel::CommandId::GitBranches); });

    // Right: sync (with what it would pull and push), or publish a branch the server does not
    // have yet; then a busy mark and the more menu.
    const float button = ImGui::GetFrameHeight();
    const bool publish = status.upstream.empty() && !status.detached && !status.initial;
    std::string sync = publish ? ICON_FA_CLOUD_ARROW_UP " Publish" : ICON_FA_ARROWS_ROTATE;
    if (!publish && status.behind)
        sync += " " ICON_FA_ARROW_DOWN + std::to_string(status.behind);
    if (!publish && status.ahead)
        sync += " " ICON_FA_ARROW_UP + std::to_string(status.ahead);
    const float syncW = ImGui::CalcTextSize(sync.c_str()).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 4.0f * s_, contentMaxX() - syncW - button - 2.0f * s_));
    ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.iconTeal));
    ImGui::BeginDisabled((!publish && status.ahead + status.behind == 0) || git().busy());
    if (ImGui::Button(sync.c_str()))
        publish ? git().push() : git().sync();
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    if (publish)
        ImGui::SetItemTooltip("Publish: put %s on the server so others (and other computers) can get it", status.branch.c_str());
    else
        ImGui::SetItemTooltip("Sync: pull %d, then push %d", status.behind, status.ahead);
    registerLastItem("git:sync", "button", publish ? "Publish branch" : "Sync", [this, publish]() { publish ? git().push() : git().sync(); });
    ImGui::SameLine(0.0f, 2.0f * s_);
    if (iconButton("more", git().busy() ? ICON_FA_SPINNER : ICON_FA_ELLIPSIS, git().busy() ? "Working..." : "More git actions"))
        ImGui::OpenPopup("##gitMore");
    registerLastItem("git:more", "button", "More git actions", nullptr);
    renderMoreMenu();
}

void GitView::renderMoreMenu()
{
    if (!ImGui::BeginPopup("##gitMore"))
        return;
    using C = viewmodel::CommandId;
    if (ImGui::MenuItem(ICON_FA_GRADUATION_CAP "  Learn Git..."))
        openGuide();
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_ROTATE_RIGHT "  Refresh"))
        git().refresh();
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_DOWNLOAD "  Pull"))
        vm_.execute(C::GitPull);
    if (ImGui::MenuItem(ICON_FA_UPLOAD "  Push"))
        vm_.execute(C::GitPush);
    if (ImGui::MenuItem(ICON_FA_UPLOAD "  Force push (with lease)..."))
        confirm("Force push?", "Force pushing replaces " + branch() + " on the server with yours. Commits others pushed since you "
                               "last pulled are refused (with lease), but rewritten history affects everyone who has the branch.",
                "Force push", [this]() { git().push(true); });
    if (ImGui::MenuItem(ICON_FA_CLOUD_ARROW_DOWN "  Fetch"))
        vm_.execute(C::GitFetch);
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_BOX_ARCHIVE "  Stash changes...", nullptr, false, git().changeCount() > 0))
        openStash_ = true;
    if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Pop latest stash", nullptr, false, !repo().stashes.empty()))
        git().stashApply(0, true);
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_LIST_CHECK "  Status"))
        vm_.execute(C::ShowGitStatus);
    if (ImGui::MenuItem(ICON_FA_CLOCK_ROTATE_LEFT "  History"))
        vm_.execute(C::ShowGitHistory);
    if (ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Compare branches...")) {
        const std::vector<std::string> refs = git().refNames();
        compareWith(refs.size() > 1 ? refs[1] : branch());
    }
    if (ImGui::MenuItem(ICON_FA_BROOM "  Clean up branches..."))
        openCleanup_ = true;
    if (ImGui::MenuItem(ICON_FA_SERVER "  Remotes...")) {
        git().loadRemotes();
        openRemotes_ = true;
    }
    if (ImGui::MenuItem(ICON_FA_CLONE "  Clone a repository..."))
        openClone_ = true;
    if (ImGui::MenuItem(ICON_FA_LIST "  Show git status")) {
        git().showStatusText();
        showOutput_ = true;
    }
    ImGui::SetItemTooltip("git's own summary: branch, staged, changed, untracked and submodules");
    if (ImGui::MenuItem(ICON_FA_TERMINAL "  Show git's last output", nullptr, false, !git().lastOutput().empty()))
        showOutput_ = true;
    if (demo()) {
        ImGui::Separator();
        ImGui::TextDisabled("Demo states");
        bool merge = merging();
        if (ImGui::MenuItem("Merge in progress", nullptr, &merge))
            git().setDemoMerge(merge);
        bool notRepo = !repo().repository;
        if (ImGui::MenuItem("Not a repository", nullptr, &notRepo))
            git().setDemoNotRepository(notRepo);
    }
    ImGui::EndPopup();
}

void GitView::renderMergeBanner()
{
    const viewmodel::GitRepository& r = repo();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const bool steps = r.operationTotal > 0;
    const float h = ImGui::GetTextLineHeightWithSpacing() * (steps ? 3.0f : 2.0f) + ImGui::GetFrameHeight() + 8.0f * s_;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), withAlpha(palette_.severityWarning, 34), 4.0f * s_);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8.0f * s_, p0.y + 4.0f * s_));
    ImGui::BeginGroup();
    int conflicts = 0;
    for (const Change& c : r.status.changes)
        conflicts += c.group == Area::Conflicts ? 1 : 0;
    const char* what = r.operation == GitOperation::Rebase ? "Rebasing" : r.operation == GitOperation::CherryPick ? "Cherry-picking"
                       : r.operation == GitOperation::Revert ? "Reverting" : "Merging";
    ImGui::TextColored(col(palette_.severityWarning), ICON_FA_TRIANGLE_EXCLAMATION "  %s", what);
    ImGui::SetItemTooltip("%s", r.operationLabel.c_str());
    if (steps)
        ImGui::TextColored(col(palette_.textDim), "commit %d of %d", r.operationStep, r.operationTotal);
    ImGui::TextColored(col(palette_.textDim), "%d %s left", conflicts, conflicts == 1 ? "conflict" : "conflicts");
    ImGui::BeginDisabled(conflicts > 0 || git().busy());
    if (ImGui::SmallButton(ICON_FA_CHECK " Continue")) {
        git().continueOperation();
        sawMerge_ = true;
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Available when every conflict is resolved and staged");
    registerLastItem("git:operation-continue", "button", "Continue", [this]() { git().continueOperation(); });
    ImGui::SameLine();
    if (ImGui::SmallButton(ICON_FA_XMARK " Abort"))
        confirm("Cancel the operation?", std::string(what) + " stops and every file goes back to how it was before it started. "
                                                          "Conflicts you resolved are lost.",
                "Abort", [this]() { git().abortOperation(); });
    if (r.operation == GitOperation::Rebase || r.operation == GitOperation::CherryPick) {
        ImGui::SameLine();
        if (ImGui::SmallButton(ICON_FA_FORWARD " Skip"))
            git().skipOperation();
        ImGui::SetItemTooltip("Leave this commit out and go on with the next");
    }
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 6.0f * s_));
}

void GitView::renderCommitBox()
{
    int staged = 0;
    for (const Change& c : repo().status.changes)
        staged += c.group == Area::Staged ? 1 : 0;
    std::string& message = git().commitMessage;
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::InputTextMultiline("##message", &message, ImVec2(w, ImGui::GetTextLineHeight() * 3.2f));
    const bool commitKey = ImGui::IsItemFocused() && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
    if (message.empty()) {
        const ImVec2 r = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(ImVec2(r.x + ImGui::GetStyle().FramePadding.x, r.y + ImGui::GetStyle().FramePadding.y),
                                            palette_.textFaint,
                                            git().writingMessage() ? "The agent is writing a message..."
                                            : git().amend          ? "Message (amends the last commit)"
                                                                   : "Message (Ctrl+Enter to commit)");
    }
    registerLastItem("git:message", "textbox", "Commit message", nullptr);

    const float button = ImGui::GetFrameHeight();
    const std::string label = std::string(ICON_FA_CHECK "  ") + (git().amend ? "Amend" : "Commit") +
                              (staged ? " " + plural(staged, "file", "files") : " all") + "###commit";
    const bool canCommit = !message.empty() && (git().changeCount() > 0 || git().amend) && !git().busy() && !merging();
    ImGui::PushStyleColor(ImGuiCol_Button, col(palette_.accent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, col(withAlpha(palette_.accent, 220)));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::BeginDisabled(!canCommit);
    if (ImGui::Button(label.c_str(), ImVec2(w - 2.0f * (button + 4.0f * s_), 0.0f)) || (commitKey && canCommit))
        git().commitWithMessage();
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    ImGui::SetItemTooltip(staged ? "Commit the staged changes (Ctrl+Enter)" : "Nothing staged: stage every change and commit");
    registerLastItem("git:commit", "button", "Commit", [this]() { git().commitWithMessage(); });
    ImGui::SameLine(0.0f, 4.0f * s_);
    if (ImGui::Button(ICON_FA_CARET_DOWN "##commitMenu", ImVec2(button, 0.0f)))
        ImGui::OpenPopup("##commitOptions");
    ImGui::SetItemTooltip("More ways to commit");
    if (ImGui::BeginPopup("##commitOptions")) {
        if (ImGui::MenuItem("Commit", nullptr, false, canCommit))
            git().commitWithMessage();
        if (ImGui::MenuItem("Commit and Push", nullptr, false, canCommit))
            git().commitWithMessage(viewmodel::AfterCommitWhat::Push);
        if (ImGui::MenuItem("Commit and Sync", nullptr, false, canCommit))
            git().commitWithMessage(viewmodel::AfterCommitWhat::Sync);
        ImGui::Separator();
        ImGui::MenuItem("Amend last commit", nullptr, &git().amend);
        ImGui::EndPopup();
    }
    ImGui::SameLine(0.0f, 4.0f * s_);
    ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.iconPurple));
    ImGui::BeginDisabled(git().writingMessage() || git().changeCount() == 0);
    if (ImGui::Button(ICON_FA_WAND_MAGIC_SPARKLES "##write", ImVec2(button, 0.0f)))
        git().writeMessage();
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    ImGui::SetItemTooltip("Let the agent write the message from the staged changes");
    registerLastItem("git:write-message", "button", "Write the commit message", [this]() { git().writeMessage(); });
    if (ImGui::Checkbox("Amend last commit", &git().amend) && git().amend && message.empty() && !repo().log.empty())
        message = repo().log.front().message;
    ImGui::SetItemTooltip("Change the last commit (its message and its files) instead of adding a new one.\n"
                          "Only for a commit you have not pushed yet.");
    ImGui::Spacing();
}

std::vector<std::string> GitView::targets(const Change& clicked) const
{
    if (selected_.count(clicked.path) && selected_.size() > 1)
        return {selected_.begin(), selected_.end()};
    return {clicked.path};
}

void GitView::clickRow(const Change& change)
{
    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl) {
        if (!selected_.erase(change.path))
            selected_.insert(change.path);
        anchor_ = change.path;
        return;
    }
    if (io.KeyShift && !anchor_.empty()) {
        // A range within one group, in the order the rows are shown.
        std::vector<std::string> order;
        for (const Change& c : repo().status.changes)
            if (c.group == change.group)
                order.push_back(c.path);
        auto a = std::find(order.begin(), order.end(), anchor_);
        auto b = std::find(order.begin(), order.end(), change.path);
        if (a != order.end() && b != order.end()) {
            if (a > b)
                std::swap(a, b);
            selected_.clear();
            selected_.insert(a, b + 1);
            return;
        }
    }
    selected_ = {change.path};
    anchor_ = change.path;
    sawDiff_ = true;
    git().showChange(change.path, change.group);
}

void GitView::renderSelectionBar()
{
    // Drop paths that are no longer listed (committed, discarded).
    for (auto it = selected_.begin(); it != selected_.end();)
        it = hasEdits(*it) ? std::next(it) : selected_.erase(it);
    if (selected_.size() < 2)
        return;
    // Several rows selected: one bar acts on all of them.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float button = ImGui::GetFrameHeight();
    const float h = button + 4.0f * s_;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), palette_.accentSoft, 4.0f * s_);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 6.0f * s_, p0.y + 2.0f * s_));
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d selected", int(selected_.size()));
    // Stage, unstage, stash, discard, clear: icons at the right (the names are in the tooltips).
    ImGui::SameLine(w - 5.0f * button - 4.0f * s_);
    const std::vector<std::string> paths(selected_.begin(), selected_.end());
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    if (iconButton("stageSelected", ICON_FA_PLUS, "Stage the selected files"))
        git().stage(paths);
    registerLastItem("git:selection-stage", "button", "Stage the selected files", [this, paths]() { git().stage(paths); });
    ImGui::SameLine();
    if (iconButton("unstageSelected", ICON_FA_MINUS, "Unstage the selected files"))
        git().unstage(paths);
    registerLastItem("git:selection-unstage", "button", "Unstage the selected files", [this, paths]() { git().unstage(paths); });
    ImGui::SameLine();
    if (iconButton("stashSelected", ICON_FA_BOX_ARCHIVE, "Stash the selected files...")) {
        stashSelectedOnly_ = true;
        openStash_ = true;
    }
    registerLastItem("git:selection-stash", "button", "Stash the selected files", [this]() {
        stashSelectedOnly_ = true;
        openStash_ = true;
    });
    ImGui::SameLine();
    if (iconButton("discardSelected", ICON_FA_ROTATE_LEFT, "Discard changes to the selected files..."))
        discardWithConfirm(paths);
    ImGui::SameLine();
    if (iconButton("clearSelection", ICON_FA_XMARK, "Clear the selection (Esc)"))
        selected_.clear();
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 4.0f * s_));
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) && ImGui::IsKeyPressed(ImGuiKey_Escape))
        selected_.clear();
}

void GitView::renderGroup(Area group, const char* title)
{
    std::vector<std::string> paths;
    for (const Change& c : repo().status.changes)
        if (c.group == group)
            paths.push_back(c.path);
    if (paths.empty())
        return;
    const int count = int(paths.size());
    ImGui::PushID(int(group));
    ImGui::PushStyleColor(ImGuiCol_Text, col(group == Area::Conflicts ? palette_.iconOrange : palette_.textDim));
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    const std::string header = std::string(title) + "  " + std::to_string(count) + "###group";
    const bool open = ImGui::TreeNodeEx(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth |
                                                            ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    registerLastItem(std::string("git:group:") + title, "group", title, nullptr);

    // Whole-group actions at the right of the header.
    const float button = ImGui::GetFrameHeight();
    const float right = contentMaxX();
    if (group == Area::Staged) {
        ImGui::SameLine(right - button);
        if (iconButton("unstageAll", ICON_FA_MINUS, "Unstage all"))
            git().unstage(paths);
    } else if (group != Area::Conflicts) {
        ImGui::SameLine(right - 2.0f * button);
        if (iconButton("discardAll", ICON_FA_ROTATE_LEFT, "Discard all..."))
            discardWithConfirm(paths);
        ImGui::SameLine(right - button);
        if (iconButton("stageAll", ICON_FA_PLUS, "Stage all"))
            git().stage(paths);
        registerLastItem(std::string("git:stage-all:") + title, "button", "Stage all", [this, paths]() { git().stage(paths); });
    }
    if (open) {
        const std::vector<Change> rows = repo().status.changes; // a click may change the list
        for (int i = 0; i < int(rows.size()); ++i)
            if (rows[size_t(i)].group == group)
                renderRow(rows[size_t(i)], i);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void GitView::renderRow(const Change& change, int index)
{
    ImGui::PushID(index);
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float right = contentMaxX();
    const bool selected = selected_.count(change.path) > 0;
    if (ImGui::Selectable("##row", selected, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick)) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && change.status != 'D')
            git().openFile(change.path);
        else
            clickRow(change);
    }
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem); // still true over its own buttons
    registerLastItem((change.submodule.empty() ? "git:file:" : "git:submodule:") + change.path, "row", change.path,
                     [this, change]() { clickRow(change); });
    if (!change.submodule.empty())
        ImGui::SetItemTooltip("%s\nSubmodule (a repository inside this one): %s\nClick: its new commits  ·  right-click: update it",
                              change.path.c_str(), change.submodule.c_str());
    else
        ImGui::SetItemTooltip("%s%s\nClick: what changed  ·  double-click: open  ·  Ctrl/Shift-click: select several",
                              change.path.c_str(), change.origPath.empty() ? "" : ("\nrenamed from " + change.origPath).c_str());
    if (ImGui::BeginPopupContextItem("##fileMenu")) {
        const std::vector<std::string> list = targets(change);
        const std::string count = list.size() > 1 ? " " + plural(int(list.size()), "file", "files") : std::string();
        if (change.group == Area::Staged) {
            if (ImGui::MenuItem((ICON_FA_MINUS "  Unstage" + count).c_str()))
                git().unstage(list);
        } else if (ImGui::MenuItem((ICON_FA_PLUS "  Stage" + count).c_str()))
            git().stage(list);
        if (ImGui::MenuItem((ICON_FA_BOX_ARCHIVE "  Stash" + count + "...").c_str())) {
            if (list.size() == 1)
                selected_ = {change.path};
            stashSelectedOnly_ = true;
            openStash_ = true;
        }
        if (!change.submodule.empty()) {
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_CUBE "  Update submodule"))
                confirm("Update " + change.path + "?",
                        "The submodule goes back to the commit this repository records (git submodule update). Commits and "
                        "changes inside it that are not recorded here are no longer checked out.",
                        "Update", [this, path = change.path]() { git().updateSubmodule(path); }, false);
            ImGui::SetItemTooltip("Check out the commit this repository records for it");
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Open diff"))
            clickRow(change);
        if (ImGui::MenuItem(ICON_FA_FILE "  Open file", nullptr, false, change.status != 'D'))
            git().openFile(change.path);
        if (ImGui::MenuItem(ICON_FA_CLOCK_ROTATE_LEFT "  File history"))
            showFileHistory(change.path);
        if (ImGui::BeginMenu(ICON_FA_CODE_BRANCH "  Take this file from")) {
            for (const std::string& ref : git().refNames())
                if (ref != branch() && ImGui::MenuItem(ref.c_str()))
                    confirm("Replace " + fileName(change.path) + "?",
                            "Replace your copy of " + change.path + " with its version in " + ref + "? Your uncommitted changes to it are lost.",
                            "Replace", [this, ref, path = change.path]() { git().restoreFile(ref, path); });
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy relative path"))
            ImGui::SetClipboardText(change.path.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem((ICON_FA_ROTATE_LEFT "  Discard changes" + count + "...").c_str(), nullptr, false, change.group != Area::Staged))
            discardWithConfirm(list);
        ImGui::EndPopup();
    }

    // Name, folder (dim), then the hover actions and the status letter at the right.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float textY = start.y + (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f;
    const bool submodule = !change.submodule.empty();
    const std::string name = (submodule ? ICON_FA_CUBE "  " : "") + fileName(change.path);
    const ImU32 nameColor = change.status == 'D' ? palette_.textDim : submodule ? palette_.iconPurple : palette_.text;
    float x = start.x + 4.0f * s_;
    dl->AddText(ImVec2(x, textY), nameColor, name.c_str());
    const float nameW = ImGui::CalcTextSize(name.c_str()).x;
    if (change.status == 'D')
        dl->AddLine(ImVec2(x, textY + ImGui::GetTextLineHeight() * 0.55f), ImVec2(x + nameW, textY + ImGui::GetTextLineHeight() * 0.55f),
                    palette_.textDim);
    x += nameW + 8.0f * s_;
    const float letterW = 16.0f * s_;
    const float button = ImGui::GetFrameHeight() * 0.9f;
    const bool showActions = hovered || (selected && selected_.size() == 1);
    const int actions = change.group == Area::Staged ? 2 : change.group == Area::Conflicts ? 2 : 3;
    const float rightLimit = ImGui::GetWindowPos().x + right - letterW - (showActions ? actions * button : 0.0f);
    dl->PushClipRect(ImVec2(x, start.y), ImVec2(std::max(x, rightLimit - 4.0f * s_), start.y + 100.0f), true);
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    dl->AddText(ImVec2(x, textY + 1.0f * s_), palette_.textFaint,
                submodule ? ("submodule: " + change.submodule).c_str() : folderOf(change.path).c_str());
    ImGui::PopFont();
    dl->PopClipRect();
    const char letter[2] = {change.status == '!' ? 'C' : change.status, 0};
    dl->AddText(ImVec2(ImGui::GetWindowPos().x + right - letterW + 3.0f * s_, textY), statusColor(change.status), letter);

    if (showActions) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f * s_, 0.0f));
        ImGui::SameLine(right - letterW - float(actions) * button);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        auto action = [&](const char* id, const char* icon, const char* tip) {
            const bool clicked = ImGui::Button((std::string(icon) + "##" + id).c_str(), ImVec2(button, ImGui::GetItemRectSize().y));
            ImGui::SetItemTooltip("%s", tip);
            ImGui::SameLine(0.0f, 0.0f);
            return clicked;
        };
        if (action("open", ICON_FA_FILE_LINES, "Open file") && change.status != 'D')
            git().openFile(change.path);
        if (change.group == Area::Conflicts) {
            if (action("resolve", ICON_FA_CHECK, "Mark resolved (stage it): do this when the file has no conflict markers left"))
                git().stage({change.path});
        } else if (change.group == Area::Staged) {
            if (action("unstage", ICON_FA_MINUS, "Unstage (your edits stay)"))
                git().unstage({change.path});
        } else {
            if (action("discard", change.status == 'U' ? ICON_FA_TRASH : ICON_FA_ROTATE_LEFT,
                       change.status == 'U' ? "Delete file..." : "Discard changes..."))
                discardWithConfirm({change.path});
            if (action("stage", ICON_FA_PLUS, "Stage: put it in the next commit"))
                git().stage({change.path});
        }
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        ImGui::NewLine();
    }
    ImGui::PopID();
}

void GitView::renderStashes()
{
    // Each stash opens to its files; every file has the shared per-file actions.
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.textDim));
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    const std::string title = "STASHES  " + std::to_string(repo().stashes.size()) + "###stashes";
    const bool open = ImGui::TreeNodeEx(title.c_str(), ImGuiTreeNodeFlags_SpanFullWidth | ImGuiTreeNodeFlags_FramePadding |
                                                           ImGuiTreeNodeFlags_AllowOverlap);
    ImGui::PopFont();
    ImGui::PopStyleColor();
    registerLastItem("git:stashes", "group", "Stashes", nullptr);
    ImGui::SameLine(contentMaxX() - ImGui::GetFrameHeight());
    ImGui::BeginDisabled(git().changeCount() == 0);
    if (iconButton("stashNew", ICON_FA_BOX_ARCHIVE, "Stash changes..."))
        openStash_ = true;
    ImGui::EndDisabled();
    registerLastItem("git:stash", "button", "Stash changes", [this]() { openStash_ = true; });
    if (!open)
        return;
    const std::vector<Stash> stashes = repo().stashes;
    for (int i = 0; i < int(stashes.size()); ++i) {
        const Stash& stash = stashes[size_t(i)];
        ImGui::PushID(i);
        const std::string label = std::string(ICON_FA_BOX_ARCHIVE "  ") + stash.message + "###stash";
        const bool expanded = ImGui::TreeNodeEx(label.c_str(), ImGuiTreeNodeFlags_SpanFullWidth);
        ImGui::SetItemTooltip("%s  ·  %s  ·  made on %s\nRight-click: apply, pop, branch, drop", stash.ref.c_str(), stash.when.c_str(),
                              stash.branch.c_str());
        registerLastItem("git:stash:" + std::to_string(i), "row", stash.message, nullptr);
        if (ImGui::BeginPopupContextItem("##stashMenu")) {
            if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Apply"))
                git().stashApply(i, false);
            ImGui::SetItemTooltip("Bring the changes back and keep the stash");
            if (ImGui::MenuItem(ICON_FA_BOX_OPEN "  Pop"))
                git().stashApply(i, true);
            ImGui::SetItemTooltip("Bring the changes back and remove the stash");
            if (ImGui::MenuItem(ICON_FA_CODE_BRANCH "  Apply on a new branch..."))
                ask("Apply the stash on a new branch", "Branch name", {}, "Create",
                    [this, i](const std::string& name) { git().stashBranch(i, name); });
            ImGui::Separator();
            if (ImGui::MenuItem(ICON_FA_TRASH "  Drop..."))
                confirm("Drop the stash?", "Drop \"" + stash.message + "\"? Its changes are gone for good.", "Drop",
                        [this, i]() { git().stashDrop(i); });
            ImGui::EndPopup();
        }
        if (expanded) {
            git().loadStashFiles(i);
            ImGui::TextColored(col(palette_.textFaint), "%s  ·  made on %s", stash.when.c_str(), stash.branch.c_str());
            for (const FileChange& f : stash.files)
                fileRow(f, stash.ref, Source::Stash);
            if (stash.files.empty())
                ImGui::TextDisabled("Loading...");
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
    ImGui::TreePop();
}

void GitView::renderStashDialog()
{
    if (openStash_) {
        ImGui::OpenPopup("Stash changes");
        stashMessage_.clear();
        if (selected_.empty())
            stashSelectedOnly_ = false;
        openStash_ = false;
    }
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(440.0f * s_, 0.0f)));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Stash changes", nullptr, ImGuiWindowFlags_NoResize))
        return;
    ImGui::TextWrapped("A stash puts changes aside and gives you clean files. Pop it later to bring them back.");
    ImGui::Spacing();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##stashMessage", "Name (optional), e.g. half-done ribbon tweak", &stashMessage_);
    registerLastItem("git:stash-name", "textbox", "Stash name", nullptr);
    const int selected = int(selected_.size());
    if (ImGui::RadioButton(("All changes (" + plural(git().changeCount(), "file", "files") + ")").c_str(), !stashSelectedOnly_))
        stashSelectedOnly_ = false;
    ImGui::BeginDisabled(selected == 0);
    if (ImGui::RadioButton(("Only the selected files (" + std::to_string(selected) + ")").c_str(), stashSelectedOnly_))
        stashSelectedOnly_ = true;
    ImGui::EndDisabled();
    ImGui::Checkbox("Include untracked files", &stashUntracked_);
    ImGui::Checkbox("Keep staged changes in place", &stashKeepIndex_);

    // What will be stashed.
    std::vector<std::string> picked;
    for (const Change& c : repo().status.changes) {
        if (c.group == Area::Conflicts || (!stashUntracked_ && c.group == Area::Untracked) || (stashKeepIndex_ && c.group == Area::Staged))
            continue;
        if (stashSelectedOnly_ && !selected_.count(c.path))
            continue;
        if (std::find(picked.begin(), picked.end(), c.path) == picked.end())
            picked.push_back(c.path);
    }
    ImGui::Spacing();
    ImGui::TextColored(col(palette_.textFaint), "%s will be stashed:", plural(int(picked.size()), "file", "files").c_str());
    ImGui::BeginChild("##stashList", ImVec2(0.0f, float(std::min(6, std::max(1, int(picked.size())))) * ImGui::GetTextLineHeightWithSpacing() + 8.0f * s_),
                      ImGuiChildFlags_Borders);
    for (const std::string& p : picked)
        ImGui::TextUnformatted(p.c_str());
    ImGui::EndChild();
    ImGui::Spacing();
    const float w = 100.0f * s_;
    ImGui::SetCursorPosX(contentMaxX() - 2.0f * w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(picked.empty());
    if (ImGui::Button(ICON_FA_BOX_ARCHIVE "  Stash", ImVec2(w, 0.0f))) {
        git().stashPush(stashMessage_, stashSelectedOnly_ ? picked : std::vector<std::string>{}, stashUntracked_, stashKeepIndex_);
        for (const std::string& p : picked)
            selected_.erase(p);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    registerLastItem("git:stash-confirm", "button", "Stash", nullptr);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void GitView::renderNotRepository()
{
    ImGui::Spacing();
    ImGui::TextColored(col(palette_.textDim), ICON_FA_CODE_BRANCH "  This folder is not a git repository.");
    ImGui::Spacing();
    ImGui::TextWrapped("Initialize one to start tracking changes here. Nothing is uploaded anywhere.");
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_PLUS "  Initialize repository", ImVec2(-1.0f, 0.0f)))
        git().init();
    registerLastItem("git:init", "button", "Initialize repository", [this]() { git().init(); });
    if (ImGui::Button(ICON_FA_CLONE "  Clone a repository...", ImVec2(-1.0f, 0.0f)))
        openClone_ = true;
    registerLastItem("git:clone", "button", "Clone a repository", [this]() { openClone_ = true; });
    ImGui::Spacing();
    if (ImGui::Button(ICON_FA_GRADUATION_CAP "  New to git? Learn the basics", ImVec2(-1.0f, 0.0f)))
        openGuide();
}

void GitView::renderRemotesDialog()
{
    if (openRemotes_) {
        ImGui::OpenPopup("Remotes##gitRemotes");
        newRemoteName_.clear();
        newRemoteUrl_.clear();
        openRemotes_ = false;
    }
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(620.0f * s_, 0.0f)));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Remotes##gitRemotes", nullptr, ImGuiWindowFlags_NoResize))
        return;
    ImGui::TextWrapped("A remote is a copy of this repository on a server (GitHub, GitLab, your company's server). "
                       "Push sends your commits there; pull and fetch bring other people's commits here.");
    ImGui::Spacing();
    const std::vector<viewmodel::GitRemote> remotes = git().remotes();
    if (ImGui::BeginTable("##remotes", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 110.0f * s_);
        ImGui::TableSetupColumn("Address");
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 3.0f * ImGui::GetFrameHeight() + 8.0f * s_);
        for (int i = 0; i < int(remotes.size()); ++i) {
            const viewmodel::GitRemote& r = remotes[size_t(i)];
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(r.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(r.fetchUrl.c_str());
            if (!r.pushUrl.empty() && r.pushUrl != r.fetchUrl)
                ImGui::TextDisabled("push: %s", r.pushUrl.c_str());
            ImGui::TableNextColumn();
            if (iconButton("url", ICON_FA_PEN, "Change the address"))
                ask("New address for " + r.name, "https://... or git@...", r.fetchUrl, "Change",
                    [this, name = r.name](const std::string& url) { git().setRemoteUrl(name, url); }, false);
            ImGui::SameLine(0.0f, 2.0f * s_);
            if (iconButton("rename", ICON_FA_I_CURSOR, "Rename"))
                ask("Rename the remote " + r.name, "New name", r.name, "Rename",
                    [this, name = r.name](const std::string& to) { git().renameRemote(name, to); });
            ImGui::SameLine(0.0f, 2.0f * s_);
            if (iconButton("remove", ICON_FA_TRASH, "Remove (nothing on the server changes)"))
                confirm("Remove " + r.name + "?", "This repository forgets the remote " + r.name + " and its branches here. "
                                                  "Nothing on the server changes; you can add it again later.",
                        "Remove", [this, name = r.name]() { git().removeRemote(name); });
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (remotes.empty())
        ImGui::TextDisabled("No remotes yet: this repository lives only on this computer.");
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(col(palette_.textDim), "Add a remote");
    ImGui::SetNextItemWidth(110.0f * s_);
    ImGui::InputTextWithHint("##name", remotes.empty() ? "origin" : "name", &newRemoteName_);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 80.0f * s_);
    ImGui::InputTextWithHint("##url", "https://github.com/you/project.git", &newRemoteUrl_);
    ImGui::SameLine();
    const std::string name = newRemoteName_.empty() && remotes.empty() ? "origin" : newRemoteName_;
    ImGui::BeginDisabled(newRemoteUrl_.empty() || !model::git::validBranchName(name));
    if (ImGui::Button("Add", ImVec2(-1.0f, 0.0f))) {
        git().addRemote(name, newRemoteUrl_);
        newRemoteName_.clear();
        newRemoteUrl_.clear();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(100.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void GitView::renderCloneDialog()
{
    if (openClone_) {
        ImGui::OpenPopup("Clone a repository##gitClone");
        cloneUrl_.clear();
        cloneName_.clear();
        if (cloneFolder_.empty()) {
            const std::string& root = vm_.workspaceRoot();
            const size_t slash = root.find_last_of('/');
            cloneFolder_ = !root.empty() && slash != std::string::npos ? root.substr(0, slash) : vm_.homeDirectory();
        }
        openClone_ = false;
    }
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(560.0f * s_, 0.0f)));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Clone a repository##gitClone", nullptr, ImGuiWindowFlags_NoResize))
        return;
    ImGui::TextWrapped("Cloning copies a repository from a server to a new folder on this computer, with its whole history, "
                       "and opens it here.");
    ImGui::Spacing();
    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::InputTextWithHint("##url", "Address, e.g. https://github.com/owner/project.git", &cloneUrl_))
        cloneName_ = viewmodel::GitViewModel::cloneName(cloneUrl_);
    registerLastItem("git:clone-url", "textbox", "Repository address", nullptr);
    ImGui::TextColored(col(palette_.textDim), "Into the folder");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##parent", &cloneFolder_);
    ImGui::TextColored(col(palette_.textDim), "As");
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##name", &cloneName_);
    if (!cloneFolder_.empty() && !cloneName_.empty())
        ImGui::TextDisabled("%s/%s", cloneFolder_.c_str(), cloneName_.c_str());
    ImGui::TextDisabled("A private repository may need you to sign in: you will be offered the terminal.");
    ImGui::Spacing();
    const float w = 100.0f * s_;
    ImGui::SetCursorPosX(contentMaxX() - 2.0f * w - ImGui::GetStyle().ItemSpacing.x);
    ImGui::BeginDisabled(cloneUrl_.empty() || cloneFolder_.empty() || cloneName_.empty());
    if (ImGui::Button(ICON_FA_CLONE "  Clone", ImVec2(w, 0.0f))) {
        git().clone(cloneUrl_, cloneFolder_, cloneName_);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(w, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---- Branches -----------------------------------------------------------------------------------

void GitView::openBranchPicker(ImVec2 anchor, bool above)
{
    openPicker_ = true;
    pickerAnchor_ = anchor;
    pickerAbove_ = above;
}

void GitView::renderPopups()
{
    ImGui::PushID("gitPopups");
    renderBranchPicker();
    renderCleanupDialog();
    renderStashDialog();
    renderRemotesDialog();
    renderCloneDialog();
    renderConfirm();
    renderInput();
    renderProblem();
    if (showOutput_) {
        ImGui::OpenPopup("Git output##gitOutput");
        showOutput_ = false;
    }
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(620.0f * s_, 360.0f * s_)), ImGuiCond_Appearing);
    fit::constrainNextWindow();
    if (ImGui::BeginPopupModal("Git output##gitOutput", nullptr)) {
        ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * 0.9f);
        ImGui::BeginChild("##text", ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing()), ImGuiChildFlags_Borders);
        ImGui::TextUnformatted(git().lastOutput().c_str());
        ImGui::EndChild();
        ImGui::PopFont();
        if (ImGui::Button("Close") || ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    renderGuide();
    ImGui::PopID();
}

void GitView::branchMenu(const Branch& b)
{
    if (!ImGui::BeginPopupContextItem("##branchMenu"))
        return;
    const bool current = b.kind == RefKind::Local && b.name == branch();
    const std::string name = b.name;
    if (ImGui::MenuItem(ICON_FA_RIGHT_LEFT "  Switch", nullptr, false, !current))
        git().switchTo(name);
    if (ImGui::MenuItem(ICON_FA_CODE_MERGE "  Merge into current", nullptr, false, !current)) {
        git().merge(name);
        sawMerge_ = true;
    }
    ImGui::SetItemTooltip("Bring %s's commits into %s", name.c_str(), branch().c_str());
    if (ImGui::MenuItem(ICON_FA_LAYER_GROUP "  Rebase current onto this", nullptr, false, !current))
        confirm("Rebase " + branch() + "?", "Your commits on " + branch() + " are replayed on top of " + name +
                                            ". This rewrites them: do it only for commits you have not pushed.",
                "Rebase", [this, name]() { git().rebase(name); }, false);
    if (ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Compare with current", nullptr, false, !current)) {
        compareWith(name);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SetItemTooltip("Commits and files that differ; cherry-pick commits or take files");
    ImGui::Separator();
    if (b.kind == RefKind::Local) {
        if (ImGui::MenuItem(ICON_FA_PEN "  Rename..."))
            ask("Rename " + name, "New name", name, "Rename", [this, name](const std::string& to) { git().renameBranch(name, to); });
        if (current && ImGui::MenuItem(ICON_FA_UPLOAD "  Push"))
            git().push();
    }
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy name"))
        ImGui::SetClipboardText(name.c_str());
    ImGui::Separator();
    if (b.kind == RefKind::Local && ImGui::MenuItem(ICON_FA_TRASH "  Delete...", nullptr, false, !current))
        confirm("Delete " + name + "?", b.merged ? name + " is merged, so its commits stay in " + branch() + "."
                                                 : "Deleting a branch with commits that are on no other branch loses them; git asks again if so.",
                "Delete", [this, name]() { git().deleteBranch(name, false); });
    if (b.kind == RefKind::Remote && ImGui::MenuItem(ICON_FA_TRASH "  Delete on the server..."))
        confirm("Delete " + name + " on the server?", "Everyone loses the branch " + name + " (local copies are kept).", "Delete",
                [this, name]() { git().deleteRemoteBranch(name); });
    ImGui::EndPopup();
}

void GitView::renderBranchPicker()
{
    if (openPicker_) {
        ImGui::OpenPopup("##branchPicker");
        branchFilter_.clear();
        creatingBranch_ = !createFrom_.empty();
        openPicker_ = false;
    }
    ImGui::SetNextWindowPos(pickerAnchor_, ImGuiCond_Appearing, ImVec2(0.0f, pickerAbove_ ? 1.0f : 0.0f));
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(460.0f * s_, 0.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s_, 10.0f * s_));
    if (!ImGui::BeginPopup("##branchPicker")) {
        ImGui::PopStyleVar();
        return;
    }
    ImGui::PopStyleVar();

    if (ImGui::IsWindowAppearing())
        ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Switch to branch or tag...", &branchFilter_);
    registerLastItem("git:branch-filter", "textbox", "Filter branches", nullptr);

    // Actions.
    if (ImGui::Button(ICON_FA_PLUS "  New branch"))
        creatingBranch_ = !creatingBranch_;
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.iconTeal));
    if (ImGui::Button(ICON_FA_ARROWS_ROTATE "  Sync"))
        git().sync();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_CLOUD_ARROW_DOWN "  Fetch"))
        git().fetch();
    ImGui::PopStyleColor();
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_BROOM "  Clean up..."))
        openCleanup_ = true;
    ImGui::SetItemTooltip("Delete branches that are merged or whose server copy is gone");
    registerLastItem("git:cleanup", "button", "Clean up branches", [this]() { openCleanup_ = true; });
    if (creatingBranch_) {
        ImGui::Spacing();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##newBranch", "Branch name, e.g. fix/login-button", &newBranch_);
        registerLastItem("git:new-branch", "textbox", "New branch name", nullptr);
        const bool valid = model::git::validBranchName(newBranch_);
        if (!newBranch_.empty() && !valid)
            ImGui::TextColored(col(palette_.severityError), ICON_FA_CIRCLE_XMARK "  Not a valid name (no spaces, .., ~ ^ : ? * [ or \\)");
        const std::string from = createFrom_.empty() ? branch() : createFrom_.substr(0, 7);
        ImGui::TextColored(col(palette_.textDim), "From %s", from.c_str());
        ImGui::SameLine();
        ImGui::Checkbox("Switch to it", &switchToNew_);
        ImGui::SameLine(contentMaxX() - 70.0f * s_);
        ImGui::BeginDisabled(!valid);
        if (ImGui::Button("Create", ImVec2(70.0f * s_, 0.0f))) {
            git().createBranch(newBranch_, createFrom_, switchToNew_);
            newBranch_.clear();
            createFrom_.clear();
            creatingBranch_ = false;
        }
        ImGui::EndDisabled();
        registerLastItem("git:create-branch", "button", "Create branch", nullptr);
    }
    ImGui::Separator();

    ImGui::BeginChild("##branchList", ImVec2(0.0f, 330.0f * s_));
    auto section = [&](const char* title) {
        ImGui::PushFont(nullptr, Fonts::kUiSize * 0.8f);
        ImGui::TextColored(col(palette_.textFaint), "%s", title);
        ImGui::PopFont();
    };
    const std::vector<Branch> branches = repo().branches;
    // Local branches, most recently used first (or by name).
    section("LOCAL");
    ImGui::SameLine(contentMaxX() - 120.0f * s_);
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.8f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
    if (ImGui::SmallButton(sortByName_ ? "Sort: name " ICON_FA_CARET_DOWN : "Sort: recent " ICON_FA_CARET_DOWN))
        sortByName_ = !sortByName_;
    ImGui::PopStyleColor();
    ImGui::PopFont();
    std::vector<int> order;
    for (int i = 0; i < int(branches.size()); ++i)
        if (branches[size_t(i)].kind == RefKind::Local)
            order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        const Branch& x = branches[size_t(a)];
        const Branch& y = branches[size_t(b)];
        if (x.current != y.current)
            return x.current;
        return sortByName_ ? x.name < y.name : x.lastUsed > y.lastUsed;
    });
    for (int i : order) {
        const Branch& b = branches[size_t(i)];
        if (!containsNoCase(b.name, branchFilter_))
            continue;
        ImGui::PushID(i);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##branch", b.current, ImGuiSelectableFlags_NoAutoClosePopups) && !b.current) {
            git().switchTo(b.name);
            ImGui::CloseCurrentPopup();
        }
        registerLastItem("git:branch:" + b.name, "row", b.name, [this, name = b.name]() { git().switchTo(name); });
        branchMenu(b);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float textY = start.y + (ImGui::GetItemRectSize().y - ImGui::GetTextLineHeight()) * 0.5f;
        dl->AddText(ImVec2(start.x + 2.0f * s_, textY), b.current ? palette_.accent : palette_.textFaint,
                    b.current ? ICON_FA_CIRCLE_CHECK : ICON_FA_CODE_BRANCH);
        dl->AddText(ImVec2(start.x + 24.0f * s_, textY), palette_.text, b.name.c_str());
        // Badges after the name: merged, remote gone.
        float x = start.x + 32.0f * s_ + ImGui::CalcTextSize(b.name.c_str()).x;
        ImGui::PushFont(nullptr, Fonts::kUiSize * 0.75f);
        auto badge = [&](const char* text, ImU32 tint) {
            const ImVec2 size = ImGui::CalcTextSize(text);
            const float y = start.y + (ImGui::GetItemRectSize().y - size.y) * 0.5f;
            dl->AddRectFilled(ImVec2(x, y - 1.0f * s_), ImVec2(x + size.x + 8.0f * s_, y + size.y + 1.0f * s_), withAlpha(tint, 40), 3.0f * s_);
            dl->AddText(ImVec2(x + 4.0f * s_, y), tint, text);
            x += size.x + 12.0f * s_;
        };
        if (b.merged)
            badge("merged", palette_.diffAddedText);
        if (b.gone)
            badge("remote gone", palette_.diffRemovedText);
        ImGui::PopFont();
        std::string track = b.upstream.empty() ? "no upstream" : b.gone ? "" : ICON_FA_CHECK "  " + b.upstream;
        if (!b.upstream.empty() && !b.gone && (b.ahead || b.behind))
            track = ICON_FA_ARROW_UP + std::to_string(b.ahead) + " " ICON_FA_ARROW_DOWN + std::to_string(b.behind) + "  " + b.upstream;
        const float trackW = ImGui::CalcTextSize(track.c_str()).x;
        dl->AddText(ImVec2(std::max(x, ImGui::GetItemRectMax().x - trackW - 4.0f * s_), textY), palette_.textFaint, track.c_str());
        ImGui::PopID();
    }
    for (const auto& [kind, title, icon] : {std::tuple{RefKind::Remote, "REMOTE", ICON_FA_CLOUD}, std::tuple{RefKind::Tag, "TAGS", ICON_FA_TAG}}) {
        ImGui::Spacing();
        section(title);
        for (int i = 0; i < int(branches.size()); ++i) {
            const Branch& b = branches[size_t(i)];
            if (b.kind != kind || !containsNoCase(b.name, branchFilter_))
                continue;
            ImGui::PushID(1000 + i);
            if (ImGui::Selectable((std::string(icon) + "  " + b.name).c_str())) {
                if (kind == RefKind::Tag)
                    confirm("Check out " + b.name + "?", "You will look at the files as they were at " + b.name +
                                                          ", outside any branch (\"detached\"). Switch back to a branch to keep working.",
                            "Check out", [this, name = b.name]() { git().switchTo(name); }, false);
                else
                    git().switchTo(b.name);
            }
            ImGui::SetItemTooltip(kind == RefKind::Remote ? "Click: a local branch that tracks %s" : "Click: look at the files at %s", b.name.c_str());
            branchMenu(b);
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::TextDisabled("Click to switch  ·  right-click: merge, rebase, compare, rename, delete");
    ImGui::EndPopup();
}

void GitView::renderCleanupDialog()
{
    // Candidates: merged into the current branch, or their remote branch is gone. Never the current
    // branch or main/master, never one with unmerged work (those need Delete and its warning).
    std::vector<Branch> candidates;
    for (const Branch& b : repo().branches)
        if (b.kind == RefKind::Local && !b.current && b.name != "main" && b.name != "master" && (b.merged || b.gone))
            candidates.push_back(b);
    if (openCleanup_) {
        ImGui::OpenPopup("Clean up branches");
        cleanupChecks_.assign(candidates.size(), 1);
        openCleanup_ = false;
    }
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(460.0f * s_, 0.0f)));
    fit::constrainNextWindow();
    if (!ImGui::BeginPopupModal("Clean up branches", nullptr, ImGuiWindowFlags_NoResize))
        return;
    cleanupChecks_.resize(candidates.size(), 1);
    if (candidates.empty())
        ImGui::TextWrapped("Nothing to clean up: every other branch has work that is not merged into %s.", branch().c_str());
    else
        ImGui::TextWrapped("These branches are merged into %s or their server copy was deleted. Deleting them loses no work.",
                           branch().c_str());
    ImGui::Spacing();
    int count = 0;
    std::vector<std::string> names;
    for (size_t k = 0; k < candidates.size(); ++k) {
        const Branch& b = candidates[k];
        ImGui::PushID(int(k));
        bool on = cleanupChecks_[k] != 0;
        if (ImGui::Checkbox(b.name.c_str(), &on))
            cleanupChecks_[k] = on ? 1 : 0;
        ImGui::SameLine();
        ImGui::TextColored(col(palette_.textFaint), "%s", b.gone ? "remote gone" : "merged");
        ImGui::PopID();
        if (cleanupChecks_[k]) {
            ++count;
            names.push_back(b.name);
        }
    }
    ImGui::Spacing();
    ImGui::Checkbox("Also delete them on the server", &cleanupRemote_);
    ImGui::TextDisabled("Branches with unmerged commits are never listed here.");
    ImGui::Spacing();
    const float w = 150.0f * s_;
    ImGui::SetCursorPosX(contentMaxX() - w - 100.0f * s_ - ImGui::GetStyle().ItemSpacing.x);
    ImGui::PushStyleColor(ImGuiCol_Button, col(withAlpha(palette_.severityError, 170)));
    ImGui::BeginDisabled(count == 0);
    if (ImGui::Button(("Delete " + plural(count, "branch", "branches")).c_str(), ImVec2(w, 0.0f))) {
        git().deleteBranches(names, cleanupRemote_);
        cleanedUp_ = true;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::PopStyleColor();
    registerLastItem("git:cleanup-confirm", "button", "Delete branches", nullptr);
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100.0f * s_, 0.0f)) || ImGui::IsKeyPressed(ImGuiKey_Escape))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

// ---- History ------------------------------------------------------------------------------------

void GitView::compareWith(const std::string& ref)
{
    const std::vector<std::string> list = git().refNames();
    compareBase_ = 0;
    const auto it = std::find(list.begin(), list.end(), ref);
    compareTarget_ = it == list.end() ? (list.size() > 1 ? 1 : 0) : int(it - list.begin());
    historyMode_ = 1;
    vm_.showPanelTab(viewmodel::PanelTab::History);
}

void GitView::showFileHistory(const std::string& path)
{
    historyMode_ = 0;
    historyPath_ = path;
    historyFilter_.clear();
    git().loadLog(historyBranch_ == 1, historyPath_);
    vm_.showPanelTab(viewmodel::PanelTab::History);
}

void GitView::renderHistory()
{
    ImGui::PushID("gitHistory");
    git().setVisible(true);
    if (!repository()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(vm_.noFolder()     ? "No folder open: open one to see its history."
                            : git().available() ? "This folder is not a git repository: the Git tab in the Explorer can create one."
                                                : "Git was not found: install it, or set EASYNOTEPAD_GIT to its path.");
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        return;
    }
    // Log | Compare, as two joined toggle buttons.
    auto modeButton = [&](const char* label, int mode, const char* id) {
        ImGui::PushStyleColor(ImGuiCol_Button, col(historyMode_ == mode ? palette_.accentSoft : palette_.panelHeaderBg));
        if (ImGui::Button(label))
            historyMode_ = mode;
        ImGui::PopStyleColor();
        registerLastItem(id, "tab", label, [this, mode]() { historyMode_ = mode; });
    };
    modeButton(ICON_FA_CLOCK_ROTATE_LEFT "  Log", 0, "git:history-log");
    ImGui::SameLine(0.0f, 2.0f * s_);
    modeButton(ICON_FA_CODE_COMPARE "  Compare", 1, "git:history-compare");
    if (demo()) {
        fit::sameLineIfFits(ImGui::CalcTextSize(ICON_FA_FLASK " Demo data").x, 10.0f * s_);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(col(guideOpen_ ? palette_.iconPurple : palette_.iconOrange), ICON_FA_FLASK " Demo data");
        ImGui::SetItemTooltip("Made-up commits, so you can try everything safely");
    }
    fit::sameLineIfFits(150.0f * s_, 12.0f * s_);
    if (historyMode_ == 0)
        renderLog();
    else
        renderCompare();
    ImGui::PopID();
}

// ---- Status tab ------------------------------------------------------------------------------
// Everything `git status` says, at a glance: the branch and whether it is in step with the remote
// on the left, the changed files grouped as git groups them on the right (stacked on a phone).

void GitView::renderStatus()
{
    ImGui::PushID("gitStatus");
    git().setVisible(true);
    if (!repository()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled(vm_.noFolder()     ? "No folder open: open one to see its status."
                            : git().available() ? "This folder is not a git repository: the Git tab in the Explorer can create one."
                                                : "Git was not found: install it, or set EASYNOTEPAD_GIT to its path.");
        ImGui::PopTextWrapPos();
        ImGui::PopID();
        return;
    }
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail >= 620.0f * s_) {
        const float left = std::clamp(avail * 0.38f, 280.0f * s_, 420.0f * s_);
        ImGui::BeginChild("##summary", ImVec2(left, 0.0f));
        renderStatusSummary();
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + 6.0f * s_, p.y), ImVec2(p.x + 6.0f * s_, p.y + ImGui::GetContentRegionAvail().y),
                                            palette_.border);
        ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f * s_, p.y));
        ImGui::BeginChild("##files", ImVec2(0.0f, 0.0f));
        renderStatusFiles();
        ImGui::EndChild();
    } else {
        ImGui::BeginChild("##status", ImVec2(0.0f, 0.0f));
        renderStatusSummary();
        ImGui::Spacing();
        renderStatusFiles();
        ImGui::EndChild();
    }
    ImGui::PopID();
}

void GitView::renderStatusSummary()
{
    const viewmodel::GitRepository& r = repo();
    const auto& st = r.status;
    const int64_t now = int64_t(std::time(nullptr));
    ImGui::PushTextWrapPos(0.0f);

    // The branch, and what it follows on the remote.
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(palette_.iconPurple), ICON_FA_CODE_BRANCH);
    ImGui::SameLine();
    ImGui::PushFont(nullptr, Fonts::kUiSize * 1.15f);
    ImGui::TextUnformatted(st.detached ? ("Detached at " + st.branch).c_str() : st.branch.c_str());
    ImGui::PopFont();
    if (demo()) {
        fit::sameLineIfFits(ImGui::CalcTextSize(ICON_FA_FLASK " Demo").x, 10.0f * s_);
        ImGui::TextColored(col(palette_.iconOrange), ICON_FA_FLASK " Demo");
        ImGui::SetItemTooltip("Made-up data, so you can try everything safely");
    }
    if (!st.upstream.empty())
        ImGui::TextDisabled("Tracks %s", st.upstream.c_str());

    // In step with the remote? One line, coloured by what it asks of you.
    bool gone = false;
    for (const Branch& b : r.branches)
        gone = gone || (b.current && b.gone);
    const std::string up = ICON_FA_ARROW_UP + std::to_string(st.ahead), down = ICON_FA_ARROW_DOWN + std::to_string(st.behind);
    ImU32 colour = palette_.iconGreen;
    std::string line, tip;
    if (st.initial) {
        colour = palette_.textDim;
        line = ICON_FA_CIRCLE_INFO "  No commits yet";
    } else if (st.detached) {
        colour = palette_.iconOrange;
        line = ICON_FA_TRIANGLE_EXCLAMATION "  Not on a branch";
        tip = "New commits here belong to no branch: switch to a branch, or create one, to keep them.";
    } else if (gone) {
        colour = palette_.iconRed;
        line = ICON_FA_TRIANGLE_EXCLAMATION "  Its remote branch was deleted";
        tip = "Its branch on the server is gone (often after a merge). Push to create it again, or delete this branch.";
    } else if (st.upstream.empty()) {
        colour = palette_.textDim;
        line = ICON_FA_CLOUD_ARROW_UP "  Not on the remote yet";
        tip = "This branch was never pushed: Publish puts it on the server.";
    } else if (st.ahead && st.behind) {
        colour = palette_.iconRed;
        line = ICON_FA_CODE_BRANCH "  Diverged: " + up + " to push, " + down + " to pull";
        tip = "Both you and the remote have new commits. Sync pulls theirs (merging), then pushes yours.";
    } else if (st.ahead) {
        colour = palette_.iconOrange;
        line = ICON_FA_ARROW_UP "  " + plural(st.ahead, "commit", "commits") + " not pushed";
        tip = "Committed here, not yet on " + st.upstream + ".";
    } else if (st.behind) {
        colour = palette_.iconBlue;
        line = ICON_FA_ARROW_DOWN "  " + plural(st.behind, "commit", "commits") + " to pull";
        tip = st.upstream + " has commits you do not have yet.";
    } else {
        line = ICON_FA_CIRCLE_CHECK "  Up to date with " + st.upstream;
        tip = "Everything committed here is on the remote, and the remote has nothing new (as of the last fetch).";
    }
    ImGui::TextColored(col(colour), "%s", line.c_str());
    if (!tip.empty())
        ImGui::SetItemTooltip("%s", tip.c_str());
    // As of when? Ahead and behind are only as fresh as the last fetch.
    if (!st.upstream.empty())
        ImGui::TextDisabled(r.fetchedAt ? "Checked with the remote %s" : "Never fetched: the remote may have more",
                            model::git::relativeTime(r.fetchedAt, now).c_str());

    // The working tree, counted as git groups it.
    int counts[4] = {0, 0, 0, 0};
    for (const Change& c : st.changes)
        ++counts[int(c.group)];
    if (st.changes.empty()) {
        ImGui::TextColored(col(palette_.iconGreen), ICON_FA_CIRCLE_CHECK "  Working tree clean: nothing to commit");
    } else {
        struct Count { int n; const char* one; const char* many; ImU32 colour; };
        const Count shown[] = {{counts[int(Area::Conflicts)], "conflict", "conflicts", palette_.iconOrange},
                               {counts[int(Area::Staged)], "staged", "staged", palette_.diffAddedText},
                               {counts[int(Area::Changes)], "not staged", "not staged", palette_.severityWarning},
                               {counts[int(Area::Untracked)], "untracked", "untracked", palette_.textDim}};
        bool first = true;
        for (const Count& c : shown) {
            if (!c.n)
                continue;
            const std::string text = std::to_string(c.n) + " " + (c.n == 1 ? c.one : c.many);
            if (!first)
                fit::sameLineIfFits(ImGui::CalcTextSize(text.c_str()).x + 12.0f * s_, 14.0f * s_);
            ImGui::TextColored(col(c.colour), "%s", text.c_str());
            first = false;
        }
    }

    // Only the actions that do something now.
    const bool busy = git().busy();
    const bool publish = st.upstream.empty() && !st.detached && !st.initial;
    ImGui::BeginDisabled(busy);
    auto action = [&](const char* label, const char* id, const char* tooltip, std::function<void()> run, bool first) {
        if (!first)
            fit::sameLineIfFitsButton(label);
        if (ImGui::Button(label))
            run();
        ImGui::SetItemTooltip("%s", tooltip);
        registerLastItem(id, "button", tooltip, run, !busy);
    };
    action(ICON_FA_ROTATE "  Fetch", "git-status:fetch", "Ask the remote what is new (changes nothing here)", [this]() { git().fetch(); }, true);
    if (publish)
        action(ICON_FA_CLOUD_ARROW_UP "  Publish", "git-status:publish", "Put this branch on the server", [this]() { git().push(); }, false);
    else if (st.ahead && st.behind)
        action(ICON_FA_ARROWS_ROTATE "  Sync", "git-status:sync", "Pull, then push", [this]() { git().sync(); }, false);
    else if (st.behind)
        action(ICON_FA_ARROW_DOWN "  Pull", "git-status:pull", "Get the new commits", [this]() { git().pull(); }, false);
    else if (st.ahead)
        action(ICON_FA_ARROW_UP "  Push", "git-status:push", "Send your commits", [this]() { git().push(); }, false);
    ImGui::EndDisabled();
    if (busy) {
        fit::sameLineIfFits(ImGui::CalcTextSize("Working...").x + 20.0f * s_);
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled(ICON_FA_SPINNER " Working...");
    }

    if (r.operation != GitOperation::None) {
        ImGui::Spacing();
        ImGui::TextColored(col(palette_.iconOrange), ICON_FA_TRIANGLE_EXCLAMATION "  %s", r.operationLabel.c_str());
        if (r.operationTotal > 0)
            ImGui::TextDisabled("Step %d of %d. Continue or abort it in the Git tab.", r.operationStep, r.operationTotal);
        else
            ImGui::TextDisabled("Finish or abort it in the Git tab.");
    }

    // The last commit.
    ImGui::Spacing();
    if (!r.headShort.empty()) {
        ImGui::TextDisabled("Last commit");
        ImGui::PushFont(fonts_.mono, 0.0f);
        ImGui::TextColored(col(palette_.textDim), "%s", r.headShort.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextUnformatted(r.headSubject.c_str());
        ImGui::TextDisabled("%s, %s", r.headAuthor.c_str(), model::git::relativeTime(r.headTime, now).c_str());
    }

    if (!r.stashes.empty())
        ImGui::TextDisabled(ICON_FA_BOX_ARCHIVE "  %s", plural(int(r.stashes.size()), "stash", "stashes").c_str());
    ImGui::PopTextWrapPos();
}

void GitView::renderStatusFiles()
{
    const auto& changes = repo().status.changes;
    if (changes.empty()) {
        ImGui::TextDisabled("No changed files.");
        return;
    }
    struct Group { Area area; const char* title; const char* hint; };
    const Group groups[] = {{Area::Conflicts, "Conflicts", "Changed on both sides: fix the file, then stage it"},
                            {Area::Staged, "Staged", "Going into the next commit"},
                            {Area::Changes, "Not staged", "Changed, but not in the next commit yet"},
                            {Area::Untracked, "Untracked", "New files git does not track yet"}};
    for (const Group& g : groups) {
        const int n = int(std::count_if(changes.begin(), changes.end(), [&](const Change& c) { return c.group == g.area; }));
        if (n == 0)
            continue;
        const std::string label = std::string(g.title) + "  " + std::to_string(n) + "###group-" + g.title;
        ImGui::PushStyleColor(ImGuiCol_Header, col(palette_.panelHeaderBg));
        const bool open = ImGui::CollapsingHeader(label.c_str(), ImGuiTreeNodeFlags_DefaultOpen);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("%s", g.hint);
        registerLastItem(std::string("git-status:group:") + g.title, "button", g.title, nullptr);
        if (!open)
            continue;
        for (const Change& c : changes)
            if (c.group == g.area)
                renderStatusRow(c);
    }
}

void GitView::renderStatusRow(const Change& change)
{
    ImGui::PushID(change.path.c_str());
    ImGui::PushID(int(change.group));
    const ImVec2 start = ImGui::GetCursorScreenPos();
    const float rowH = ImGui::GetFrameHeight();
    if (ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap | ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0.0f, rowH))) {
        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && change.status != 'D')
            git().openFile(change.path);
        else
            git().showChange(change.path, change.group);
    }
    const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem);
    const ImVec2 end = ImGui::GetItemRectMax();
    registerLastItem("git-status:file:" + change.path, "row", change.path,
                     [this, change]() { git().showChange(change.path, change.group); });
    ImGui::SetItemTooltip("%s%s\nClick: what changed  ·  double-click: open", change.path.c_str(),
                          change.origPath.empty() ? "" : ("\nrenamed from " + change.origPath).c_str());
    if (ImGui::BeginPopupContextItem("##menu")) {
        if (change.group == Area::Staged) {
            if (ImGui::MenuItem(ICON_FA_MINUS "  Unstage"))
                git().unstage({change.path});
        } else if (ImGui::MenuItem(change.group == Area::Conflicts ? ICON_FA_CHECK "  Mark resolved (stage)" : ICON_FA_PLUS "  Stage")) {
            git().stage({change.path});
        }
        if (ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Open diff"))
            git().showChange(change.path, change.group);
        if (ImGui::MenuItem(ICON_FA_FILE "  Open file", nullptr, false, change.status != 'D'))
            git().openFile(change.path);
        if (ImGui::MenuItem(ICON_FA_COPY "  Copy relative path"))
            ImGui::SetClipboardText(change.path.c_str());
        ImGui::Separator();
        if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Discard changes...", nullptr, false, change.group != Area::Staged))
            discardWithConfirm({change.path});
        ImGui::EndPopup();
    }

    // Letter, name, folder (dim, cut short to fit), and stage/unstage at the right when hovered.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float textY = start.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
    const char letter[2] = {change.status == '!' ? 'C' : change.status, 0};
    dl->AddText(ImVec2(start.x + 4.0f * s_, textY), statusColor(change.status), letter);
    const float button = rowH;
    const bool showAction = hovered && !git().busy();
    const float right = end.x - (showAction ? button + 4.0f * s_ : 4.0f * s_);
    float x = start.x + 22.0f * s_;
    const std::string name = fileName(change.path);
    const ImU32 nameColour = change.status == 'D' ? palette_.textDim : palette_.text;
    x = fit::text(dl, ImVec2(x, textY), right, nameColour, name) + 8.0f * s_;
    if (x < right) {
        ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
        const std::string folder = change.origPath.empty() ? folderOf(change.path) : ("from " + change.origPath);
        fit::text(dl, ImVec2(x, textY + 1.0f * s_), right, palette_.textFaint, folder);
        ImGui::PopFont();
    }
    if (showAction) {
        ImGui::SetCursorScreenPos(ImVec2(end.x - button, start.y));
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        const bool staged = change.group == Area::Staged;
        const char* icon = staged ? ICON_FA_MINUS : change.group == Area::Conflicts ? ICON_FA_CHECK : ICON_FA_PLUS;
        if (ImGui::Button(icon, ImVec2(button, rowH)))
            staged ? git().unstage({change.path}) : git().stage({change.path});
        ImGui::SetItemTooltip(staged ? "Unstage (your edits stay)" : change.group == Area::Conflicts ? "Mark resolved (stage it)" : "Stage");
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    ImGui::PopID();
}

void GitView::renderLog()
{
    if (!repo().logLoaded)
        git().loadLog(historyBranch_ == 1, historyPath_);
    ImGui::SetNextItemWidth(fit::itemWidth(170.0f * s_, 120.0f * s_));
    const std::string current = ICON_FA_CODE_BRANCH "  " + branch();
    const char* choices[] = {current.c_str(), ICON_FA_CODE_BRANCH "  All branches"};
    if (ImGui::Combo("##branch", &historyBranch_, choices, 2))
        git().loadLog(historyBranch_ == 1, historyPath_);
    if (!historyPath_.empty()) {
        const std::string pathLabel = ICON_FA_FILE "  " + fileName(historyPath_) + "  " ICON_FA_XMARK;
        fit::sameLineIfFitsButton(pathLabel.c_str());
        if (ImGui::Button(pathLabel.c_str())) {
            historyPath_.clear();
            git().loadLog(historyBranch_ == 1, historyPath_);
        }
        ImGui::SetItemTooltip("History of %s only: click to show every file", historyPath_.c_str());
    }
    fit::sameLineIfFits(140.0f * s_);
    ImGui::SetNextItemWidth(fit::itemWidth(260.0f * s_, 140.0f * s_));
    ImGui::InputTextWithHint("##filter", ICON_FA_MAGNIFYING_GLASS "  Message, author or hash", &historyFilter_);
    registerLastItem("git:history-filter", "textbox", "Filter history", nullptr);
    const std::vector<Commit>& commits = repo().log;
    const std::string count = std::to_string(commits.size()) + " commits";
    fit::sameLineIfFits(ImGui::CalcTextSize(count.c_str()).x);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(palette_.textFaint), "%s", count.c_str());

    // Wide: the details beside the list. Narrow (a phone): below it, the full width.
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const bool stacked = room.x < 640.0f * s_;
    const float detailsW = stacked ? 0.0f : std::min(460.0f * s_, room.x * 0.42f);
    const ImVec2 listSize = stacked ? ImVec2(0.0f, std::max(150.0f * s_, room.y * 0.5f)) : ImVec2(room.x - detailsW - 8.0f * s_, 0.0f);
    ImGui::BeginChild("##commits", listSize);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowH = ImGui::GetTextLineHeightWithSpacing() + 4.0f * s_;
    const float laneW = 14.0f * s_;
    int lanes = 1;
    for (const Commit& c : commits) {
        lanes = std::max(lanes, c.lane + 1);
        for (const auto& [from, to] : c.edges)
            lanes = std::max(lanes, std::max(from, to) + 1);
    }
    const float graphW = laneW * float(lanes) + 12.0f * s_;
    const ImU32 laneColors[] = {palette_.iconBlue, palette_.iconPurple, palette_.iconTeal, palette_.iconOrange,
                                palette_.iconGreen, palette_.iconRed, palette_.iconGray, palette_.accent};
    auto laneColor = [&](int lane) { return laneColors[size_t(lane) % 8]; };
    std::vector<float> centers;
    std::vector<int> shown;
    const bool filtered = !historyFilter_.empty();
    for (int i = 0; i < int(commits.size()); ++i) {
        const Commit& c = commits[size_t(i)];
        if (!containsNoCase(c.message + " " + c.author + " " + c.hash, historyFilter_))
            continue;
        ImGui::PushID(i);
        const ImVec2 start = ImGui::GetCursorScreenPos();
        if (ImGui::Selectable("##commit", selectedCommit_ == i, ImGuiSelectableFlags_AllowOverlap, ImVec2(0.0f, rowH))) {
            selectedCommit_ = i;
            git().loadCommitFiles(i);
        }
        registerLastItem("git:commit:" + c.shortHash, "row", c.message, [this, i]() {
            selectedCommit_ = i;
            git().loadCommitFiles(i);
        });
        if (ImGui::BeginPopupContextItem("##commitContext")) {
            commitMenuItems(c);
            ImGui::EndPopup();
        }
        const float textY = start.y + (rowH - ImGui::GetTextLineHeight()) * 0.5f;
        centers.push_back(start.y + rowH * 0.5f);
        shown.push_back(i);
        float x = start.x + (filtered ? 4.0f * s_ : graphW);
        // Columns from the right, dropped by priority when the row is narrow: the author first, then
        // the hash; the time stays longest. The message (with its ref badges) gets the rest.
        const float right = ImGui::GetItemRectMax().x - 4.0f * s_;
        const float hashW = 70.0f * s_, whenW = 100.0f * s_, authorW = 120.0f * s_, messageMin = 140.0f * s_;
        const float room = right - x;
        const bool showWhen = room - whenW >= messageMin * 0.7f;
        const bool showHash = showWhen && room - whenW - hashW >= messageMin;
        const bool showAuthor = showHash && room - whenW - hashW - authorW >= messageMin + 60.0f * s_;
        const float hashX = showHash ? right - hashW + 8.0f * s_ : right;
        const float whenX = showWhen ? (showHash ? right - hashW : right) - whenW + 8.0f * s_ : right;
        const float authorX = showAuthor ? whenX - authorW : whenX;
        const float messageEnd = (showAuthor ? authorX : showWhen ? whenX : hashX) - 10.0f * s_;
        // Ref badges, then the message.
        const std::vector<std::string> refs = splitRefs(c.refs);
        for (size_t r = 0; r < refs.size(); ++r) {
            std::string ref = refs[r];
            const bool tag = ref.rfind("tag: ", 0) == 0;
            if (tag)
                ref = ref.substr(5);
            const bool head = ref.rfind("HEAD", 0) == 0;
            const bool remote = ref.find('/') != std::string::npos && !head &&
                                std::none_of(repo().branches.begin(), repo().branches.end(),
                                             [&](const Branch& b) { return b.kind == RefKind::Local && b.name == ref; });
            const ImU32 tint = head ? palette_.accent : tag ? palette_.iconOrange : remote ? palette_.iconTeal : palette_.iconPurple;
            const std::string text = std::string(tag ? ICON_FA_TAG " " : remote ? ICON_FA_CLOUD " " : "") + ref;
            // Badges leave the message some room; the last one that fits is cut short, the rest are counted.
            const float badgeEnd = messageEnd - 60.0f * s_;
            ImGui::PushFont(nullptr, Fonts::kUiSize * 0.8f);
            const ImVec2 size = ImGui::CalcTextSize(text.c_str());
            const float y = start.y + (rowH - size.y) * 0.5f;
            if (x + 40.0f * s_ > badgeEnd) {
                const std::string more = "+" + std::to_string(refs.size() - r);
                dl->AddText(ImVec2(x, y), palette_.textFaint, more.c_str());
                x += ImGui::CalcTextSize(more.c_str()).x + 8.0f * s_;
                ImGui::PopFont();
                break;
            }
            const float w = std::min(size.x + 10.0f * s_, badgeEnd - x);
            dl->AddRectFilled(ImVec2(x, y - 1.0f * s_), ImVec2(x + w, y + size.y + 1.0f * s_), withAlpha(tint, 45), 3.0f * s_);
            fit::text(dl, ImVec2(x + 5.0f * s_, y), x + w - 4.0f * s_, tint, text);
            ImGui::PopFont();
            x += w + 6.0f * s_;
        }
        fit::text(dl, ImVec2(x, textY), messageEnd, palette_.text, c.message);
        if (showAuthor)
            fit::text(dl, ImVec2(authorX, textY), whenX - 10.0f * s_, palette_.textDim, c.author);
        if (showWhen)
            fit::text(dl, ImVec2(whenX, textY), hashX - 8.0f * s_, palette_.textFaint, c.when);
        if (showHash) {
            ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * 0.9f);
            dl->AddText(ImVec2(hashX, textY), palette_.textFaint, c.shortHash.c_str());
            ImGui::PopFont();
        }
        ImGui::PopID();
    }
    // Graph: lane lines between rows, then the commit dots over them (not while filtering: the
    // rows in between are hidden, so the lines would lie).
    if (!filtered) {
        const float gx = ImGui::GetWindowPos().x + ImGui::GetStyle().WindowPadding.x + 10.0f * s_;
        for (size_t k = 0; k + 1 < shown.size(); ++k)
            for (const auto& [from, to] : commits[size_t(shown[k])].edges)
                dl->AddBezierCubic(ImVec2(gx + float(from) * laneW, centers[k]), ImVec2(gx + float(from) * laneW, centers[k] + rowH * 0.6f),
                                   ImVec2(gx + float(to) * laneW, centers[k + 1] - rowH * 0.6f), ImVec2(gx + float(to) * laneW, centers[k + 1]),
                                   laneColor(std::max(from, to)), 2.0f * s_);
        for (size_t k = 0; k < shown.size(); ++k) {
            const Commit& c = commits[size_t(shown[k])];
            const ImVec2 center(gx + float(c.lane) * laneW, centers[k]);
            dl->AddCircleFilled(center, 4.5f * s_, palette_.panelBg);
            dl->AddCircle(center, 4.5f * s_, laneColor(c.lane), 0, 2.0f * s_);
            if (c.refs.rfind("HEAD", 0) == 0)
                dl->AddCircleFilled(center, 3.0f * s_, laneColor(c.lane));
        }
    }
    if (shown.empty())
        ImGui::TextDisabled(commits.empty() ? (repo().status.initial ? "No commits yet: make the first one in the Git tab." : "Loading...")
                                            : "No commits match the filter");
    if (!repo().logComplete && !commits.empty() && !filtered) {
        if (ImGui::Button("Load more"))
            git().loadLog(historyBranch_ == 1, historyPath_, true);
    }
    ImGui::EndChild();

    if (!stacked)
        ImGui::SameLine(0.0f, 8.0f * s_);
    ImGui::BeginChild("##details", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    if (selectedCommit_ >= 0 && selectedCommit_ < int(commits.size())) {
        git().loadCommitFiles(selectedCommit_);
        renderCommitDetails(commits[size_t(selectedCommit_)]);
    }
    ImGui::EndChild();
}

void GitView::renderCommitDetails(const Commit& c)
{
    ImGui::TextWrapped("%s", c.message.c_str());
    ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * 0.9f);
    ImGui::TextColored(col(palette_.textFaint), "%s", c.shortHash.c_str());
    ImGui::PopFont();
    const std::string byline = c.author + "  ·  " + c.when;
    fit::sameLineIfFits(ImGui::CalcTextSize(byline.c_str()).x);
    ImGui::TextColored(col(palette_.textDim), "%s", byline.c_str());
    ImGui::Spacing();
    if (ImGui::SmallButton(ICON_FA_RIGHT_LEFT "  Checkout"))
        confirm("Check out " + c.shortHash + "?", "You will look at the files as they were at this commit, outside any branch "
                                                  "(\"detached\"). Switch back to a branch to keep working.",
                "Check out", [this, hash = c.hash]() { git().checkoutDetached(hash); }, false);
    fit::sameLineIfFitsButton(ICON_FA_CODE_PULL_REQUEST "  Cherry-pick", true);
    if (ImGui::SmallButton(ICON_FA_CODE_PULL_REQUEST "  Cherry-pick"))
        git().cherryPick({c.hash});
    ImGui::SetItemTooltip("Copy this commit onto %s", branch().c_str());
    fit::sameLineIfFitsButton(ICON_FA_ROTATE_LEFT "  Revert", true);
    if (ImGui::SmallButton(ICON_FA_ROTATE_LEFT "  Revert"))
        git().revert(c.hash);
    ImGui::SetItemTooltip("Undo this commit with a new commit (safe even after pushing)");
    fit::sameLineIfFitsButton(ICON_FA_ELLIPSIS "##more", true);
    if (ImGui::SmallButton(ICON_FA_ELLIPSIS "##more"))
        ImGui::OpenPopup("##commitMenu");
    if (ImGui::BeginPopup("##commitMenu")) {
        commitMenuItems(c);
        ImGui::EndPopup();
    }
    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(col(palette_.textFaint), "%s  ·  right-click a file for more",
                       plural(int(c.files.size()), "changed file", "changed files").c_str());
    ImGui::PopTextWrapPos();
    for (const FileChange& f : c.files)
        fileRow(f, c.hash, Source::Commit);
}

void GitView::commitMenuItems(const Commit& c)
{
    if (ImGui::MenuItem(ICON_FA_RIGHT_LEFT "  Checkout (detached)..."))
        confirm("Check out " + c.shortHash + "?", "You will look at the files as they were at this commit, outside any branch.",
                "Check out", [this, hash = c.hash]() { git().checkoutDetached(hash); }, false);
    if (ImGui::MenuItem(ICON_FA_CODE_BRANCH "  Create branch here...")) {
        createFrom_ = c.hash;
        openBranchPicker(ImGui::GetMousePos(), false);
    }
    if (ImGui::MenuItem(ICON_FA_CODE_PULL_REQUEST "  Cherry-pick"))
        git().cherryPick({c.hash});
    if (ImGui::MenuItem(ICON_FA_ROTATE_LEFT "  Revert"))
        git().revert(c.hash);
    if (ImGui::BeginMenu(ICON_FA_BACKWARD_STEP "  Reset current branch to here")) {
        if (ImGui::MenuItem("Soft (keep the changes staged)"))
            git().reset(c.hash, viewmodel::GitViewModel::ResetMode::Soft);
        if (ImGui::MenuItem("Mixed (keep the changes as edits)"))
            git().reset(c.hash, viewmodel::GitViewModel::ResetMode::Mixed);
        if (ImGui::MenuItem("Hard (throw the changes away)..."))
            confirm("Reset hard?", "Moves " + branch() + " back to " + c.shortHash + " and throws away every change after it, "
                                   "committed or not" + (git().changeCount() ? " (including your " + plural(git().changeCount(), "changed file", "changed files") + ")" : std::string()) +
                                   ". This cannot be undone here.",
                    "Reset hard", [this, hash = c.hash]() { git().reset(hash, viewmodel::GitViewModel::ResetMode::Hard); });
        ImGui::EndMenu();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(ICON_FA_TAG "  Create tag..."))
        ask("Tag " + c.shortHash, "Tag name, e.g. v1.2.0", {}, "Create tag", [this, hash = c.hash](const std::string& name) { git().tag(name, hash); });
    if (ImGui::MenuItem(ICON_FA_CODE_COMPARE "  Compare with your files"))
        compareWith(c.hash);
    if (ImGui::MenuItem(ICON_FA_COPY "  Copy hash"))
        ImGui::SetClipboardText(c.hash.c_str());
}

void GitView::renderCompare()
{
    // Two refs; what the second has that the first lacks: commits to cherry-pick, files to take.
    const std::vector<std::string> list = git().refNames();
    if (list.empty())
        return;
    compareBase_ = std::clamp(compareBase_, 0, int(list.size()) - 1);
    compareTarget_ = std::clamp(compareTarget_, 0, int(list.size()) - 1);
    auto refCombo = [&](const char* id, int& value) {
        ImGui::SetNextItemWidth(fit::itemWidth(190.0f * s_, 120.0f * s_));
        if (ImGui::BeginCombo(id, list[size_t(value)].c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < int(list.size()); ++i)
                if (ImGui::Selectable(list[size_t(i)].c_str(), i == value))
                    value = i;
            ImGui::EndCombo();
        }
    };
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(palette_.textDim), "Into");
    ImGui::SameLine();
    refCombo("##base", compareBase_);
    fit::sameLineIfFitsButton(ICON_FA_RIGHT_LEFT "##swap");
    if (ImGui::Button(ICON_FA_RIGHT_LEFT "##swap"))
        std::swap(compareBase_, compareTarget_);
    ImGui::SetItemTooltip("Swap");
    fit::sameLineIfFits(ImGui::CalcTextSize("take from").x + 130.0f * s_);
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(palette_.textDim), "take from");
    ImGui::SameLine();
    refCombo("##target", compareTarget_);
    const std::string& base = list[size_t(compareBase_)];
    const std::string& target = list[size_t(compareTarget_)];
    if (compareBase_ == compareTarget_) {
        ImGui::Spacing();
        ImGui::TextDisabled("Pick two different branches, tags or commits.");
        return;
    }
    git().compare(base, target);
    const std::vector<Commit>& commits = repo().compareCommits;
    const std::vector<FileChange>& files = repo().compareFiles;
    if (pickedFor_ != base + "..." + target || pickCommits_.size() != commits.size() || pickFiles_.size() != files.size()) {
        pickedFor_ = base + "..." + target;
        pickCommits_.assign(commits.size(), 1);
        pickFiles_.assign(files.size(), 0);
    }
    const bool intoCurrent = compareBase_ == 0; // refNames() lists the current branch first

    // A heading with controls at its right; on a narrow pane the controls go under it.
    auto controlsAtRight = [&](float width) {
        const float headingEnd = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX();
        if (headingEnd + 12.0f * s_ + width <= contentMaxX())
            ImGui::SameLine(contentMaxX() - width);
    };
    // Wide: commits and files side by side. Narrow (a phone): one above the other.
    const ImVec2 room = ImGui::GetContentRegionAvail();
    const bool stacked = room.x < 640.0f * s_;
    const ImVec2 commitsSize = stacked ? ImVec2(0.0f, std::max(140.0f * s_, (room.y - 8.0f * s_) * 0.5f))
                                       : ImVec2((room.x - 8.0f * s_) * 0.5f, 0.0f);
    // Commits on the target that the base lacks.
    ImGui::BeginChild("##compareCommits", commitsSize, ImGuiChildFlags_Borders);
    int commitsPicked = 0;
    for (int v : pickCommits_)
        commitsPicked += v;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(col(palette_.textFaint), "%s on %s, not on %s", plural(int(commits.size()), "COMMIT", "COMMITS").c_str(),
                       target.c_str(), base.c_str());
    ImGui::PopTextWrapPos();
    const std::string pickLabel = ICON_FA_CODE_PULL_REQUEST "  Cherry-pick " + std::to_string(commitsPicked);
    controlsAtRight(smallButtonWidth(pickLabel));
    ImGui::BeginDisabled(commitsPicked == 0 || !intoCurrent || git().busy());
    if (ImGui::SmallButton(pickLabel.c_str())) {
        std::vector<std::string> hashes;
        for (int i = int(commits.size()) - 1; i >= 0; --i) // oldest first
            if (pickCommits_[size_t(i)])
                hashes.push_back(commits[size_t(i)].hash);
        git().cherryPick(hashes);
        tookFromCompare_ = true;
    }
    ImGui::EndDisabled();
    if (!intoCurrent)
        ImGui::SetItemTooltip("Cherry-picking goes into the current branch: choose it under Into");
    else
        ImGui::SetItemTooltip("Copy the checked commits onto %s, oldest first; a conflict stops in the merge-conflict flow", base.c_str());
    registerLastItem("git:compare-cherry-pick", "button", "Cherry-pick selected commits", nullptr);
    ImGui::Separator();
    for (int i = 0; i < int(commits.size()); ++i) {
        const Commit& c = commits[size_t(i)];
        ImGui::PushID(i);
        bool on = pickCommits_[size_t(i)] != 0;
        if (ImGui::Checkbox("##pick", &on))
            pickCommits_[size_t(i)] = on ? 1 : 0;
        ImGui::SameLine();
        ImGui::PushFont(fonts_.mono, Fonts::kEditorSize * 0.9f);
        ImGui::TextColored(col(palette_.textFaint), "%s", c.shortHash.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        // The message is cut short before the time; a narrow pane leaves the time out.
        const float rowRight = ImGui::GetWindowPos().x - ImGui::GetScrollX() + contentMaxX();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const bool showWhen = rowRight - at.x > 220.0f * s_;
        const float whenX = rowRight - 90.0f * s_;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        fit::text(dl, at, (showWhen ? whenX : rowRight) - 8.0f * s_, palette_.text, c.message);
        if (showWhen)
            fit::text(dl, ImVec2(whenX, at.y), rowRight, palette_.textFaint, c.when);
        ImGui::Dummy(ImVec2(std::max(1.0f, rowRight - at.x), ImGui::GetTextLineHeight()));
        ImGui::PopID();
    }
    if (commits.empty())
        ImGui::TextDisabled("None: %s has nothing that %s lacks.", target.c_str(), base.c_str());
    ImGui::EndChild();

    // Files that differ; take the checked ones into the working tree.
    if (!stacked)
        ImGui::SameLine(0.0f, 8.0f * s_);
    ImGui::BeginChild("##compareFiles", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders);
    int filesPicked = 0;
    for (int v : pickFiles_)
        filesPicked += v;
    ImGui::TextColored(col(palette_.textFaint), "%s THAT DIFFER", plural(int(files.size()), "FILE", "FILES").c_str());
    const std::string takeLabel = ICON_FA_FILE_ARROW_DOWN "  Take " + plural(filesPicked, "file", "files");
    const float stageW = ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x + ImGui::CalcTextSize("Stage").x;
    controlsAtRight(smallButtonWidth(takeLabel) + stageW + ImGui::GetStyle().ItemSpacing.x);
    ImGui::Checkbox("Stage", &stageTaken_);
    ImGui::SetItemTooltip("Stage the files after taking them");
    ImGui::SameLine();
    ImGui::BeginDisabled(filesPicked == 0 || !intoCurrent || git().busy());
    if (ImGui::SmallButton(takeLabel.c_str())) {
        std::vector<std::string> paths, edited;
        for (size_t k = 0; k < files.size(); ++k)
            if (pickFiles_[k]) {
                paths.push_back(files[k].path);
                if (hasEdits(files[k].path))
                    edited.push_back(fileName(files[k].path));
            }
        auto take = [this, target, paths, stage = stageTaken_]() {
            git().takeFiles(target, paths, stage);
            tookFromCompare_ = true;
            pickFiles_.assign(pickFiles_.size(), 0);
        };
        if (edited.empty())
            take();
        else {
            std::string names = edited[0];
            for (size_t k = 1; k < edited.size(); ++k)
                names += ", " + edited[k];
            confirm("Replace your copies?", "You have uncommitted changes to " + names + ". Taking the files from " + target +
                                                " replaces them, and those changes are lost.",
                    "Take files", take);
        }
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("Copy these files' versions from %s into your folder", target.c_str());
    registerLastItem("git:compare-take", "button", "Take selected files", nullptr);
    ImGui::Separator();
    for (size_t k = 0; k < files.size(); ++k) {
        ImGui::PushID(int(k));
        bool on = pickFiles_[k] != 0;
        fileRow(files[k], target, Source::Compare, &on);
        pickFiles_[k] = on ? 1 : 0;
        ImGui::PopID();
    }
    if (files.empty())
        ImGui::TextDisabled("No files differ.");
    ImGui::EndChild();
}

// ---- Demo data and beginner help ----------------------------------------------------------------

namespace {

// What a control does, in plain words, for the guide's "What's this?" box. Matched by id prefix.
struct Explanation {
    const char* prefix;
    const char* title;
    const char* text;
};

const Explanation kExplanations[] = {
    {"explorer-tab:Git", "Git tab", "Everything about saving versions of your files: what changed, choosing what to save, and saving it."},
    {"status:branch", "Current branch",
     "The line of work you are on. Up-arrow: commits to upload (push). Down-arrow: commits to download (pull). "
     "A yellow dot: you have changes not committed yet. Click to switch branch."},
    {"git:branch-filter", "Find a branch", "Type part of a branch name to find it quickly."},
    {"git:branch:", "A branch", "Click to switch to it: your files change to match. Right-click to merge it, compare it, rename or delete it."},
    {"git:branch", "Branch menu", "Shows all branches. Switch to another line of work, or start a new one."},
    {"git:sync", "Sync", "Downloads new commits from the server (pull), then uploads yours (push). The arrows say how many each way."},
    {"git:more", "More actions", "Pull, push, fetch, stash, history, clean-up, and this guide."},
    {"git:message", "Commit message", "One short line saying what you changed and why, for example \"Fix typo in the README\"."},
    {"git:commit:", "A commit", "One saved snapshot. Click to see its files; right-click to undo it (revert), copy it (cherry-pick) or start a branch from it."},
    {"git:commit", "Commit", "Saves the staged files as a new snapshot in the history. Nothing staged? It offers to save all changes."},
    {"git:write-message", "Write the message for me", "Asks the agent to describe the staged changes in a commit message."},
    {"git:group:STAGED", "Staged", "Files in the box: they go into your next commit. The - button takes them back out (your edits stay)."},
    {"git:group:CHANGES", "Changes", "Files you edited since the last commit that are not staged yet. + stages them; the arrow discards edits (asks first)."},
    {"git:group:UNTRACKED", "Untracked", "New files git is not tracking yet. Stage them to add them to the project's history."},
    {"git:group:CONFLICTS", "Conflicts", "Both sides changed the same lines. Open the file, keep what you want, then mark it resolved."},
    {"git:file@", "A file in a commit, stash or comparison",
     "Click to see what changed. Right-click to open that version, compare it with yours, or bring just this file over."},
    {"git:submodule:", "A submodule",
     "Another repository kept inside this one (for example a library), pinned to one of its commits. \"New commits\": it "
     "points to a newer commit than the one recorded; stage it to record that. \"Modified files\" / \"untracked files\": "
     "its own files changed. Click for its commits; right-click > Update submodule goes back to the recorded commit."},
    {"git:file:", "A changed file",
     "Click to see what changed. Hover for buttons: + stage, arrow discard. Ctrl-click or Shift-click to select several."},
    {"git:selection-", "Selected files", "These buttons act on every selected file at once."},
    {"git:stashes", "Stashes", "Changes you put aside for later. Open one to see its files."},
    {"git:stash:", "A stash", "A set of changes put aside. Right-click: Apply (bring back, keep the stash) or Pop (bring back and remove it)."},
    {"git:stash", "Stash", "Puts your unfinished changes aside so your folder is clean. Take them back later."},
    {"git:cleanup", "Clean up", "Deletes branches that are finished (merged) or whose server copy is gone. You tick which ones."},
    {"panel-tab:History", "History", "Every commit, newest first, with a graph of the branches."},
    {"git:history-log", "Log", "The list of commits on a branch."},
    {"git:history-compare", "Compare", "What another branch has that yours does not: pick commits or files to take."},
    {"git:history-filter", "Search history", "Find commits by message, author, hash or file name."},
    {"git:compare-cherry-pick", "Cherry-pick", "Copies the ticked commits onto your branch, oldest first."},
    {"git:compare-take", "Take files", "Copies the ticked files' versions into your folder. Review them, then commit."},
    {"git:init", "Initialize repository", "Starts tracking this folder with git. Nothing is uploaded anywhere."},
    {"git:demo-reset", "Reset demo data", "Puts the made-up files, branches and commits back the way they started."},
    {"command:GitCommit", "Commit", "Saves the staged files as a snapshot."},
    {"command:GitStageAll", "Stage all", "Puts every changed file in the box for the next commit."},
    {"command:GitDiscardAll", "Discard", "Throws away edits that are not staged. Always asks first."},
    {"command:GitBranches", "Branch", "Switch to another branch or start a new one."},
    {"command:GitMerge", "Merge", "Brings another branch's commits into yours."},
    {"command:GitSync", "Sync", "Pull, then push."},
    {"command:GitPull", "Pull", "Downloads other people's commits into your branch."},
    {"command:GitPush", "Push", "Uploads your commits to the server."},
    {"command:GitFetch", "Fetch", "Checks the server for news without changing your files."},
    {"command:ShowGitStatus", "Status", "Shows the branch, whether it is pushed, and every changed file."},
    {"command:ShowGitHistory", "History", "Opens the list of commits."},
    {"command:GitBlame", "Blame", "Shows who last changed each line, and when."},
    {"command:GitStash", "Stash", "Puts unfinished changes aside."},
    {"command:GitStashPop", "Pop", "Brings the latest stashed changes back."},
    {"command:GitWriteMessage", "Message", "The agent writes the commit message."},
    {"command:GitGuide", "Learn Git", "This guide."},
};

const Explanation* explainFor(const std::string& id)
{
    for (const Explanation& e : kExplanations)
        if (id.rfind(e.prefix, 0) == 0)
            return &e;
    return nullptr;
}

} // namespace

void GitView::note(const std::string& whatHappened) { vm_.statusText.set((demo() ? "Demo: " : "Git: ") + whatHappened); }

const UiElement* GitView::hoveredGitElement() const
{
    // The smallest explained element under the mouse (a row beats the list around it).
    const ImVec2 m = ImGui::GetMousePos();
    const UiElement* best = nullptr;
    float bestArea = 0.0f;
    for (const UiElement& e : UiAutomation::instance().elements()) {
        if (m.x < e.min.x || m.y < e.min.y || m.x >= e.max.x || m.y >= e.max.y || !explainFor(e.id))
            continue;
        const float area = (e.max.x - e.min.x) * (e.max.y - e.min.y);
        if (!best || area < bestArea) {
            best = &e;
            bestArea = area;
        }
    }
    return best;
}

void GitView::renderDemoBanner()
{
    // Always say clearly that this is made-up data, and whether the guide is running.
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight() + 6.0f * s_;
    const ImU32 tint = guideOpen_ ? palette_.iconPurple : palette_.iconOrange;
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + h), withAlpha(tint, 40), 4.0f * s_);
    dl->AddRect(p0, ImVec2(p0.x + w, p0.y + h), withAlpha(tint, 120), 4.0f * s_);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 8.0f * s_, p0.y + 3.0f * s_));
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(col(tint), "%s", guideOpen_ ? ICON_FA_GRADUATION_CAP "  Learning · demo data" : ICON_FA_FLASK "  Demo data");
    ImGui::SetItemTooltip("These files, branches and commits are made up so you can try everything safely.\n"
                          "Nothing here touches your real project.");
    const float button = ImGui::GetFrameHeight();
    ImGui::SameLine(contentMaxX() - 2.0f * button - 6.0f * s_);
    if (iconButton("demoReset", ICON_FA_ARROW_ROTATE_LEFT, "Reset demo data: start again with the original files and branches")) {
        git().resetDemo();
        baselineGuide();
    }
    registerLastItem("git:demo-reset", "button", "Reset demo data", [this]() {
        git().resetDemo();
        baselineGuide();
    });
    ImGui::SameLine(0.0f, 2.0f * s_);
    if (iconButton("demoGuide", guideOpen_ ? ICON_FA_XMARK : ICON_FA_GRADUATION_CAP, guideOpen_ ? "Close the guide" : "Learn Git: a step-by-step guide")) {
        if (guideOpen_)
            closeGuide();
        else
            openGuide();
    }
    registerLastItem("git:guide-toggle", "button", guideOpen_ ? "Close the guide" : "Learn Git", [this]() {
        if (guideOpen_)
            closeGuide();
        else
            openGuide();
    });
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + h + 6.0f * s_));
}

// ---- Learn Git ----------------------------------------------------------------------------------

namespace {

enum class Diagram { Flow, Stash, Branches, Merge, Pick, Tidy };
constexpr unsigned kFiles = 1, kStaging = 2, kCommits = 4, kRemote = 8;

struct Lesson {
    const char* icon;
    const char* title;
    const char* body;  // the idea, in plain words
    const char* where; // where it is in this app
    const char* task;  // "Try it"; empty: nothing to do
    Diagram diagram;
    unsigned boxes;    // Flow and Stash: which of files / staging / commits / remote to light up
    // For checking the plan: which item builds it, what the preview shows, what is still to build.
    const char* planItem;
    const char* preview;
    const char* toBuild;
};

const Lesson kLessons[] = {
    {ICON_FA_CODE_BRANCH, "What is git?",
     "Git keeps a history of your project folder. Whenever you reach a point worth keeping, you save a snapshot "
     "called a commit. You can look back at any snapshot, compare it with today, or return to it, so nothing you "
     "committed is lost by accident.",
     "The Git tab in the Explorer, next to Files and Open. The status bar shows the branch you are on.",
     "Open the Git tab.", Diagram::Flow, kFiles | kCommits, "14",
     "The Git tab, the Git ribbon tab and the branch chip, over the real repository (git status --porcelain=v2, "
     "refreshed after saves, after every action and every few seconds while a git view is shown).",
     ""},
    {ICON_FA_PEN, "Changes",
     "Git compares your files with the last commit. Anything different is a change: M modified, A added, "
     "D deleted, U untracked (a new file git is not tracking yet). A cube is a submodule: a repository inside this "
     "one. Changes are only edits on your disk; nothing is saved in git yet.",
     "Git tab: the Changes and Untracked groups. Click a file to see exactly what changed in the Diff tab.",
     "Click a changed file to see its diff.", Diagram::Flow, kFiles, "14",
     "The groups, the coloured letters (also in the Files tree and on tabs), and the real diff in the Diff tab: index "
     "against the file (unsaved edits included), HEAD against the index, ours against theirs for a conflict.",
     ""},
    {ICON_FA_BOX_OPEN, "Staging",
     "Before you commit, you choose which changes go into the snapshot. That is staging: like putting things in "
     "a box before you seal it. Stage a whole file, several at once (Ctrl- or Shift-click them), or later just "
     "some lines of a file.",
     "Hover a file and press +, or the + on a group's header to stage the whole group. Staged files move to "
     "Staged; the - button takes them back out.",
     "Stage Main.cpp.", Diagram::Flow, kFiles | kStaging, "14",
     "Stage and unstage per file, per group and for a multi-selection (git add, git restore --staged); single lines from "
     "the Diff tab (git apply --cached).",
     ""},
    {ICON_FA_CODE_COMMIT, "Commit",
     "A commit seals the box: it saves the staged changes as a snapshot, with a message that says why. Good "
     "messages are short and say what the change does. The magic-wand button asks the agent to write one from "
     "the staged changes.",
     "The message box and Commit button at the top of the Git tab (Ctrl+Enter). Amend corrects the last commit "
     "instead of adding a new one.",
     "Write a message and commit.", Diagram::Flow, kStaging | kCommits, "14",
     "The commit box (git commit -F -, so any message is safe), Amend, Commit and Push / Sync, a failing hook's output, "
     "and the agent writing the message from the staged diff (a plain list of files without an agent).",
     ""},
    {ICON_FA_CLOCK_ROTATE_LEFT, "History",
     "Every commit is kept, newest at the top. HEAD marks where you are now. Select a commit to see who made it, "
     "when, and which files it changed. Right-click it to revert it (undo it with a new commit) or to check it out.",
     "The History tab in the bottom panel, Log view.",
     "Open History.", Diagram::Flow, kCommits, "17",
     "git log in pages of 300 with the lanes laid out from the parents, filter, file history, commit details and "
     "actions (checkout, cherry-pick, revert, reset, tag), and blame in the editor.",
     ""},
    {ICON_FA_CODE_BRANCH, "Branches",
     "A branch is a separate line of commits, so you can try something without disturbing the main line. You "
     "can switch branches at any time and your files change to match. Most work happens on a short branch that "
     "is merged back into main when it is ready.",
     "Click the branch name in the status bar or at the top of the Git tab: switch, or press New branch.",
     "Create a branch, or switch to another one.", Diagram::Branches, 0, "15",
     "The branch list from for-each-ref, git switch (remote branches get a tracking branch, tags check out detached), "
     "new, rename, delete, and Stash and switch when changes are in the way.",
     ""},
    {ICON_FA_CODE_MERGE, "Merging and conflicts",
     "Merging brings another branch's commits into yours. Usually git combines them by itself. When both "
     "branches changed the same lines, git cannot decide: that is a conflict, and you choose what to keep. A "
     "yellow banner stays until every conflict is resolved, and Abort always puts things back.",
     "Right-click a branch and choose Merge into current, or use Merge on the Git ribbon tab.",
     "Open the branch list, right-click main and choose Merge into current. Mark the conflict resolved with its tick, "
     "then press Continue (or Abort).",
     Diagram::Merge, 0, "16",
     "Merge, rebase, cherry-pick and revert states read from the repository; the banner with Continue, Abort and Skip; "
     "conflict blocks in the editor with Accept current / incoming / both, F8, and Mark resolved on save.",
     ""},
    {ICON_FA_CLOUD, "Remotes: push and pull",
     "A remote is a copy of the repository on a server such as GitHub, usually called origin. Push sends your "
     "commits there; pull brings other people's commits here. Up-arrow 2 means two commits to push, down-arrow "
     "1 means one to pull. Sync does both.",
     "The Sync button at the top of the Git tab, Pull and Push on the Git ribbon tab, and the arrows in the "
     "status bar.",
     "Press Sync.", Diagram::Flow, kCommits | kRemote, "15",
     "Fetch, pull, push and sync; Publish for a branch without upstream; sign-in through the terminal; a rejected push "
     "offers Pull then push; the Remotes dialog and Clone.",
     "Fetching on a timer; creating the repository on GitHub from here; choosing the branch a local one tracks."},
    {ICON_FA_BOX_ARCHIVE, "Stash",
     "A stash is a drawer: it puts unfinished changes aside and leaves your folder clean, for example to switch "
     "branch or pull quickly. Later you take them back out (Pop). You can stash only some files, and take a "
     "single file back out.",
     "Stash on the Git ribbon tab, the box icon on the Stashes header, or select files and use the selection bar.",
     "Stash some changes.", Diagram::Stash, kFiles, "17",
     "The stash dialog (name, selected files only, untracked, keep staged) and the stash list with each stash's files, "
     "apply, pop, branch and drop; one file can be taken out of a stash.",
     ""},
    {ICON_FA_CODE_PULL_REQUEST, "Taking work from elsewhere",
     "Sometimes you want only part of another branch: one fixed file, or two of its commits. Compare shows what "
     "the other branch has that yours lacks. Tick commits and Cherry-pick them (copies them onto your branch), "
     "or tick files and Take them (copies those versions into your folder for you to review).",
     "History tab > Compare, or right-click a branch > Compare with current.",
     "Cherry-pick commits or take a file.", Diagram::Pick, 0, "18",
     "The Compare view (git log --cherry-pick, git diff base...target), Cherry-pick N, Take N files (git restore "
     "--source), and the per-file actions: open, compare, restore, apply only this file's change (git apply --3way).",
     ""},
    {ICON_FA_ROTATE_LEFT, "Undoing safely",
     "Most git mistakes can be undone. Unstage only takes a file out of the box; your edits stay. Discard throws "
     "edits away, so the app always asks first. Revert undoes a commit by adding a new one, which is safe even "
     "after pushing. Restore brings back a file as it was in any commit.",
     "Right-click a file or a commit. Anything that loses work asks first and says what will be lost.",
     "", Diagram::Flow, kFiles | kStaging | kCommits, "14 and 17",
     "Unstage, discard and delete (confirmed, naming what is lost), revert, restore from any commit, reset soft / mixed / "
     "hard (hard confirmed), and Revert this change from the change bar in the editor.",
     ""},
    {ICON_FA_BROOM, "Tidying up branches",
     "Once a branch is merged, it has done its job. The merged and remote gone labels in the branch list show "
     "which branches are safe to delete. Clean up lists only those, with checkboxes; a branch with unmerged work "
     "is never listed.",
     "Branch list > Clean up..., or the Git tab's ... menu.",
     "Clean up merged branches.", Diagram::Tidy, 0, "15",
     "Merged (git branch --merged) and remote gone labels, sort by recent use, and the Clean up dialog that deletes "
     "here and, if asked, on the server.",
     ""},
};
constexpr int kLessonCount = int(sizeof(kLessons) / sizeof(kLessons[0]));

} // namespace

void GitView::baselineGuide()
{
    // "Try it" ticks count what is done from now on.
    baseCommits_ = repo().log.size();
    baseStashes_ = repo().stashes.size();
    baseBranches_ = git().localBranchNames().size();
    baseBranch_ = branch();
    sawDiff_ = sawMerge_ = tookFromCompare_ = cleanedUp_ = false;
}

void GitView::closeGuide()
{
    guideOpen_ = false;
    git().setLearning(false);
    selected_.clear();
    vm_.hidePointer();
}

void GitView::openGuide()
{
    // The guide always practises on demo data: the user's repository is never touched.
    if (!guideOpen_) {
        git().setLearning(true);
        selected_.clear();
        baselineGuide();
    }
    guideOpen_ = true;
    guideAppearing_ = true;
    lastAction_.clear();
    hoverTitle_.clear();
    hoverText_.clear();
    seenStatus_ = vm_.statusText.get();
    reviewed_.resize(size_t(kLessonCount), 0);
}

bool GitView::guideDone(int lesson) const
{
    switch (lesson) {
    case 0: return UiAutomation::instance().find("git:message") != nullptr;
    case 1: return sawDiff_;
    case 2:
        return std::any_of(repo().status.changes.begin(), repo().status.changes.end(),
                           [](const Change& c) { return c.group == Area::Staged && c.path == "src/app/Main.cpp"; });
    case 3: return repo().log.size() > baseCommits_;
    case 4: return vm_.showPanel.get() && vm_.panelTab.get() == viewmodel::PanelTab::History && historyMode_ == 0;
    case 5: return branch() != baseBranch_ || git().localBranchNames().size() > baseBranches_;
    case 6: return sawMerge_ && !merging();
    case 7: return ahead() == 0 && behind() == 0;
    case 8: return repo().stashes.size() > baseStashes_;
    case 9: return tookFromCompare_;
    case 11: return cleanedUp_;
    default: return true;
    }
}

void GitView::guideShowMe(int lesson)
{
    // Bring the control on screen, then point the agent cursor at it.
    using C = viewmodel::CommandId;
    auto gitTab = [this]() { vm_.execute(C::ShowGit); };
    std::string target;
    std::string message;
    switch (lesson) {
    case 0:
        gitTab();
        target = "explorer-tab:Git";
        message = "Your Git tab: what changed, and the commit box.";
        break;
    case 1:
        gitTab();
        target = "git:group:CHANGES";
        message = "Files changed since the last commit. Click one to see its diff.";
        break;
    case 2: {
        gitTab();
        const bool listed = std::any_of(repo().status.changes.begin(), repo().status.changes.end(), [](const Change& c) {
            return c.path == "src/app/Main.cpp" && c.group == Area::Changes;
        });
        target = listed ? "git:file:src/app/Main.cpp" : "git:group:CHANGES";
        message = "Hover a file and press + to stage it.";
        break;
    }
    case 3:
        gitTab();
        target = "git:commit";
        message = "Write a message above, then Commit (Ctrl+Enter).";
        break;
    case 4:
        historyMode_ = 0;
        vm_.showPanelTab(viewmodel::PanelTab::History);
        target = "panel-tab:History";
        message = "Every commit, newest first. Select one to see its files.";
        break;
    case 5:
        target = "status:branch";
        message = "Your current branch. Click it to switch or to create one.";
        break;
    case 6:
        target = "command:GitMerge";
        message = "Merge another branch into yours from here, or right-click a branch.";
        break;
    case 7:
        gitTab();
        target = "git:sync";
        message = "Sync: pull what is new, then push your commits.";
        break;
    case 8:
        gitTab();
        target = "git:stash";
        message = "Put changes aside in a stash; take them back later.";
        break;
    case 9:
        historyMode_ = 1;
        vm_.showPanelTab(viewmodel::PanelTab::History);
        target = "git:history-compare";
        message = "Compare: tick commits to cherry-pick, or files to take.";
        break;
    case 10:
        gitTab();
        target = "git:group:CHANGES";
        message = "Right-click a file: Unstage, Discard (asks first), Take this file from...";
        break;
    case 11: {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        openBranchPicker(ImVec2(vp->Pos.x + vp->Size.x * 0.5f - 210.0f * s_, vp->Pos.y + 150.0f * s_), false);
        target = "git:cleanup";
        message = "Clean up lists only merged branches and ones whose remote is gone.";
        break;
    }
    default: return;
    }
    vm_.pointAt(target, message, false, 8.0);
}

void GitView::drawGuideDiagram(int lesson, float height)
{
    const Lesson& l = kLessons[size_t(lesson)];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, height));
    dl->AddRectFilled(p0, ImVec2(p0.x + w, p0.y + height), palette_.editorBg, 6.0f * s_);
    ImGui::PushFont(nullptr, Fonts::kUiSize * 0.85f);
    auto label = [&](ImVec2 center, const char* text, ImU32 c) {
        const ImVec2 size = ImGui::CalcTextSize(text);
        dl->AddText(ImVec2(center.x - size.x * 0.5f, center.y - size.y * 0.5f), c, text);
    };
    auto arrow = [&](ImVec2 from, ImVec2 to, ImU32 c) {
        dl->AddLine(from, to, c, 1.5f * s_);
        const float dx = to.x - from.x, dy = to.y - from.y;
        const float len = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
        const ImVec2 d(dx / len, dy / len), n(-d.y, d.x);
        const float a = 6.0f * s_;
        dl->AddTriangleFilled(to, ImVec2(to.x - d.x * a + n.x * a * 0.6f, to.y - d.y * a + n.y * a * 0.6f),
                              ImVec2(to.x - d.x * a - n.x * a * 0.6f, to.y - d.y * a - n.y * a * 0.6f), c);
    };
    auto box = [&](ImVec2 a, ImVec2 b, const char* text, bool lit) {
        dl->AddRectFilled(a, b, lit ? withAlpha(palette_.accent, 60) : palette_.panelHeaderBg, 4.0f * s_);
        dl->AddRect(a, b, lit ? palette_.accent : palette_.border, 4.0f * s_, lit ? 1.5f * s_ : 1.0f);
        label(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), text, lit ? palette_.text : palette_.textDim);
    };
    const float pad = 10.0f * s_;

    if (l.diagram == Diagram::Flow || l.diagram == Diagram::Stash) {
        // Your files -> Staging -> Commits -> Remote, and pull coming back.
        const char* names[] = {"Your files", "Staging", "Commits", "Remote"};
        const char* verbs[] = {"stage", "commit", "push"};
        const float gap = 30.0f * s_;
        const float bw = (w - 2.0f * pad - 3.0f * gap) / 4.0f;
        const float bh = 30.0f * s_;
        const float top = p0.y + pad;
        float cx[4];
        for (int i = 0; i < 4; ++i) {
            const ImVec2 a(p0.x + pad + float(i) * (bw + gap), top), b(a.x + bw, top + bh);
            box(a, b, names[i], (l.boxes & (1u << i)) != 0);
            cx[i] = a.x + bw * 0.5f;
            if (i < 3) {
                arrow(ImVec2(b.x + 3.0f * s_, top + bh * 0.5f), ImVec2(b.x + gap - 3.0f * s_, top + bh * 0.5f), palette_.textDim);
                label(ImVec2(b.x + gap * 0.5f, top + bh + 9.0f * s_), verbs[i], palette_.textFaint);
            }
        }
        if (l.diagram == Diagram::Flow) {
            const float y = top + bh + 28.0f * s_;
            const ImU32 c = (l.boxes & kRemote) ? palette_.iconTeal : palette_.textFaint;
            dl->AddLine(ImVec2(cx[3], top + bh + 2.0f * s_), ImVec2(cx[3], y), c, 1.5f * s_);
            dl->AddLine(ImVec2(cx[3], y), ImVec2(cx[0], y), c, 1.5f * s_);
            arrow(ImVec2(cx[0], y), ImVec2(cx[0], top + bh + 3.0f * s_), c);
            const char* pull = "pull (other people's commits)";
            const ImVec2 size = ImGui::CalcTextSize(pull);
            const ImVec2 mid((cx[0] + cx[3]) * 0.5f, y);
            dl->AddRectFilled(ImVec2(mid.x - size.x * 0.5f - 4.0f * s_, mid.y - size.y * 0.5f),
                              ImVec2(mid.x + size.x * 0.5f + 4.0f * s_, mid.y + size.y * 0.5f), palette_.editorBg);
            label(mid, pull, c);
        } else {
            // The stash drawer under the files.
            const ImVec2 a(p0.x + pad, top + bh + 26.0f * s_), b(a.x + bw, a.y + bh * 0.9f);
            box(a, b, "Stash", true);
            arrow(ImVec2(cx[0] - 10.0f * s_, top + bh + 2.0f * s_), ImVec2(cx[0] - 10.0f * s_, a.y - 2.0f * s_), palette_.iconOrange);
            arrow(ImVec2(cx[0] + 10.0f * s_, a.y - 2.0f * s_), ImVec2(cx[0] + 10.0f * s_, top + bh + 2.0f * s_), palette_.diffAddedText);
            dl->AddText(ImVec2(b.x + 10.0f * s_, a.y + 2.0f * s_), palette_.iconOrange, "stash: put aside");
            dl->AddText(ImVec2(b.x + 10.0f * s_, a.y + ImGui::GetTextLineHeight() + 2.0f * s_), palette_.diffAddedText,
                        "pop: take back out");
        }
    } else {
        // Two lines of commits: main, and a feature branch.
        const float x0 = p0.x + pad + 64.0f * s_, x1 = p0.x + w - pad - 16.0f * s_;
        const float yMain = p0.y + height * 0.32f, yFeat = p0.y + height * 0.74f;
        const float step = (x1 - x0) / 5.0f;
        const bool tidy = l.diagram == Diagram::Tidy;
        const ImU32 mainC = palette_.iconBlue;
        const ImU32 featC = tidy ? withAlpha(palette_.iconPurple, 110) : palette_.iconPurple;
        const float r = 5.0f * s_;
        auto px = [&](int i) { return x0 + step * float(i); };
        const bool joined = l.diagram == Diagram::Merge || tidy;
        const bool picked = l.diagram == Diagram::Pick;
        const int mainDots = joined || picked ? 6 : 5;
        dl->AddText(ImVec2(p0.x + pad, yMain - ImGui::GetTextLineHeight() * 0.5f), mainC, "main");
        dl->AddText(ImVec2(p0.x + pad, yFeat - ImGui::GetTextLineHeight() * 0.5f), featC, "feature");
        dl->AddLine(ImVec2(px(0), yMain), ImVec2(px(mainDots - 1), yMain), mainC, 2.0f * s_);
        dl->AddBezierCubic(ImVec2(px(1), yMain), ImVec2(px(1) + step * 0.5f, yMain), ImVec2(px(2) - step * 0.5f, yFeat),
                           ImVec2(px(2), yFeat), featC, 2.0f * s_);
        dl->AddLine(ImVec2(px(2), yFeat), ImVec2(px(4), yFeat), featC, 2.0f * s_);
        if (joined)
            dl->AddBezierCubic(ImVec2(px(4), yFeat), ImVec2(px(4) + step * 0.5f, yFeat), ImVec2(px(5) - step * 0.5f, yMain),
                               ImVec2(px(5), yMain), featC, 2.0f * s_);
        for (int i = 0; i < mainDots; ++i) {
            const bool special = i == 5;
            dl->AddCircleFilled(ImVec2(px(i), yMain), special ? r * 1.3f : r, special && picked ? palette_.accent : mainC);
        }
        for (int i = 2; i <= 4; ++i) {
            dl->AddCircleFilled(ImVec2(px(i), yFeat), r, featC);
            if (picked && i == 3)
                dl->AddCircle(ImVec2(px(i), yFeat), r * 2.0f, palette_.accent, 0, 2.0f * s_);
        }
        if (l.diagram == Diagram::Branches)
            dl->AddText(ImVec2(px(4) + 10.0f * s_, yFeat - ImGui::GetTextLineHeight() * 0.5f), palette_.text, "you are here");
        if (l.diagram == Diagram::Merge)
            dl->AddText(ImVec2(px(5) - 120.0f * s_, yMain - ImGui::GetTextLineHeight() - 8.0f * s_), palette_.severityWarning,
                        "merge: same lines changed? you choose");
        if (picked) {
            arrow(ImVec2(px(3) + r * 2.0f, yFeat - r), ImVec2(px(5) - r * 1.6f, yMain + r), palette_.accent);
            dl->AddText(ImVec2(px(4) - 10.0f * s_, (yMain + yFeat) * 0.5f - ImGui::GetTextLineHeight()), palette_.accent, "cherry-pick");
        }
        if (tidy)
            dl->AddText(ImVec2(px(2), yFeat + 10.0f * s_), palette_.diffAddedText, "merged: safe to delete");
    }
    ImGui::PopFont();
}

void GitView::renderGuide()
{
    if (!guideOpen_)
        return;
    if (merging())
        sawMerge_ = true;
    reviewed_.resize(size_t(kLessonCount), 0);
    lesson_ = std::clamp(lesson_, 0, kLessonCount - 1);
    // Every demo action reports itself in the status bar; the guide repeats it in plain words.
    const std::string& status = vm_.statusText.get();
    if (status != seenStatus_) {
        seenStatus_ = status;
        if (status.rfind("Demo: ", 0) == 0 || status.rfind("Git: ", 0) == 0)
            lastAction_ = status.substr(status.find(' ') + 1);
    }

    // A floating window over the right of the workspace: it never blocks the controls it explains.
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    if (guideAppearing_) {
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x - 14.0f * s_, vp->Pos.y + 156.0f * s_), ImGuiCond_Always,
                                ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowFocus();
        guideAppearing_ = false;
    }
    // Fits its content, but never taller than the app window (it scrolls instead): a window that
    // pokes out of the main viewport would become a separate OS window.
    ImGui::SetNextWindowSize(fit::windowSize(ImVec2(470.0f * s_, 0.0f)), ImGuiCond_Always);
    ImGui::SetNextWindowViewport(vp->ID);
    bool open = true;
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                   ImGuiWindowFlags_NoResize;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f * s_, 12.0f * s_));
    const bool visible = ImGui::Begin(ICON_FA_GRADUATION_CAP "  Learn Git###gitGuide", &open, flags);
    ImGui::PopStyleVar();
    if (registerWindowClose("git-guide:close", "Close Learn Git"))
        open = false;
    if (visible) {
        const Lesson& l = kLessons[size_t(lesson_)];
        auto go = [&](int lesson) {
            lesson_ = std::clamp(lesson, 0, kLessonCount - 1);
            vm_.hidePointer();
        };

        // The lesson scrolls when the app window is short; Back / Next stay in view below it.
        const float footer = ImGui::GetFrameHeightWithSpacing() + ImGui::GetStyle().ItemSpacing.y * 2.0f + 4.0f * s_;
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(FLT_MAX, std::max(200.0f * s_, vp->Size.y - 250.0f * s_ - footer)));
        ImGui::BeginChild("##lesson", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY);

        // Say plainly that this is practice on made-up data, and how to start again.
        ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.iconOrange));
        ImGui::TextWrapped(ICON_FA_FLASK "  You are practising on demo data. Nothing you do here touches your real files.");
        ImGui::PopStyleColor();
        if (ImGui::SmallButton(ICON_FA_ARROW_ROTATE_LEFT "  Reset demo data")) {
            git().resetDemo();
            baselineGuide();
        }
        ImGui::SetItemTooltip("Start again with the original made-up files, branches and commits");
        registerLastItem("git-guide:reset", "button", "Reset demo data", [this]() {
            git().resetDemo();
            baselineGuide();
        });
        ImGui::Spacing();

        // Contents, and one dot per lesson (filled once its "Try it" is done).
        const std::string current = std::to_string(lesson_ + 1) + " of " + std::to_string(kLessonCount) + ":  " + l.title;
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##lessons", current.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < kLessonCount; ++i) {
                const std::string item = std::string(guideDone(i) ? ICON_FA_CIRCLE_CHECK : ICON_FA_CIRCLE) + "  " +
                                         std::to_string(i + 1) + ".  " + kLessons[size_t(i)].title +
                                         (reviewed_[size_t(i)] ? "   (reviewed)" : "");
                if (ImGui::Selectable(item.c_str(), i == lesson_))
                    go(i);
            }
            ImGui::EndCombo();
        }
        registerLastItem("git-guide:contents", "button", "Lessons", nullptr);
        {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float spacing = 14.0f * s_;
            for (int i = 0; i < kLessonCount; ++i) {
                const ImVec2 c(p.x + 5.0f * s_ + float(i) * spacing, p.y + 7.0f * s_);
                if (i == lesson_)
                    dl->AddCircle(c, 5.0f * s_, palette_.accent, 0, 2.0f * s_);
                dl->AddCircleFilled(c, 3.0f * s_, guideDone(i) ? palette_.diffAddedText : palette_.border);
            }
            ImGui::Dummy(ImVec2(0.0f, 14.0f * s_));
        }

        // The lesson.
        ImGui::PushFont(nullptr, Fonts::kUiSize * 1.3f);
        ImGui::TextColored(col(palette_.iconPurple), "%s", l.icon);
        ImGui::SameLine(0.0f, 10.0f * s_);
        ImGui::TextUnformatted(l.title);
        ImGui::PopFont();
        ImGui::Spacing();
        drawGuideDiagram(lesson_, 92.0f * s_);
        ImGui::Spacing();
        ImGui::TextWrapped("%s", l.body);
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col(palette_.panelHeaderBg));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f * s_);
        ImGui::BeginChild("##where", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::TextColored(col(palette_.textDim), ICON_FA_LOCATION_DOT "  In this app");
        ImGui::TextWrapped("%s", l.where);
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::Spacing();

        // Show me, and the task.
        ImGui::PushStyleColor(ImGuiCol_Button, col(withAlpha(palette_.iconPurple, 70)));
        if (ImGui::Button(ICON_FA_HAND_POINTER "  Show me"))
            guideShowMe(lesson_);
        ImGui::PopStyleColor();
        ImGui::SetItemTooltip("Point at it in the app (opens what is needed)");
        registerLastItem("git-guide:show-me", "button", "Show me", [this]() { guideShowMe(lesson_); });
        if (*l.task) {
            const bool done = guideDone(lesson_);
            ImGui::SameLine(0.0f, 12.0f * s_);
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(col(done ? palette_.diffAddedText : palette_.textDim), "%s", done ? ICON_FA_CIRCLE_CHECK "  Done" : ICON_FA_CIRCLE "  Try it:");
            ImGui::PushStyleColor(ImGuiCol_Text, col(done ? palette_.textDim : palette_.text));
            ImGui::TextWrapped("%s", l.task);
            ImGui::PopStyleColor();
        }

        // What's this? (whatever the mouse is on) and What just happened? (the last action).
        if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows)) {
            if (const UiElement* e = hoveredGitElement()) {
                const Explanation* x = explainFor(e->id);
                hoverTitle_ = x->title;
                hoverText_ = x->text;
                ImGui::GetForegroundDrawList()->AddRect(ImVec2(e->min.x - 2.0f * s_, e->min.y - 2.0f * s_),
                                                        ImVec2(e->max.x + 2.0f * s_, e->max.y + 2.0f * s_), palette_.iconPurple,
                                                        4.0f * s_, 2.0f * s_);
            }
        }
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_ChildBg, col(withAlpha(palette_.iconPurple, 28)));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 4.0f * s_);
        ImGui::BeginChild("##coach", ImVec2(0.0f, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::TextColored(col(palette_.iconPurple), ICON_FA_CIRCLE_QUESTION "  What's this?");
        if (hoverTitle_.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.textDim));
            ImGui::TextWrapped("Point at anything in the Git tab, the branch list or History: it is explained here, and outlined.");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextUnformatted(hoverTitle_.c_str());
            ImGui::PushStyleColor(ImGuiCol_Text, col(palette_.textDim));
            ImGui::TextWrapped("%s", hoverText_.c_str());
            ImGui::PopStyleColor();
        }
        if (!lastAction_.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(col(palette_.diffAddedText), ICON_FA_BOLT "  What just happened");
            ImGui::TextWrapped("%s", lastAction_.c_str());
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // Plan notes: what this lesson's part of the plan is, for checking the implementation.
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::SetNextItemOpen(showPlanNotes_, ImGuiCond_Always);
        int reviewedCount = 0;
        for (int v : reviewed_)
            reviewedCount += v;
        const std::string notes = std::string(ICON_FA_CLIPBOARD_CHECK "  Plan notes: item ") + l.planItem + "    " +
                                  std::to_string(reviewedCount) + "/" + std::to_string(kLessonCount) + " reviewed###planNotes";
        showPlanNotes_ = ImGui::CollapsingHeader(notes.c_str());
        registerLastItem("git-guide:plan-notes", "button", "Plan notes", [this]() { showPlanNotes_ = !showPlanNotes_; });
        if (showPlanNotes_) {
            ImGui::TextColored(col(palette_.diffAddedText), "Built");
            ImGui::TextWrapped("%s", l.preview);
            if (*l.toBuild) {
                ImGui::TextColored(col(palette_.severityWarning), "Later");
                ImGui::TextWrapped("%s", l.toBuild);
            }
            bool reviewed = reviewed_[size_t(lesson_)] != 0;
            if (ImGui::Checkbox("I have checked this part", &reviewed))
                reviewed_[size_t(lesson_)] = reviewed ? 1 : 0;
            registerLastItem("git-guide:reviewed", "checkbox", "Reviewed", nullptr);
        }

        ImGui::EndChild();

        // Back / Next.
        ImGui::Separator();
        ImGui::BeginDisabled(lesson_ == 0);
        if (ImGui::Button(ICON_FA_ARROW_LEFT "  Back"))
            go(lesson_ - 1);
        ImGui::EndDisabled();
        registerLastItem("git-guide:back", "button", "Previous lesson", [this]() { lesson_ = std::max(0, lesson_ - 1); });
        const bool last = lesson_ == kLessonCount - 1;
        const char* next = last ? "Finish  " ICON_FA_CHECK : "Next  " ICON_FA_ARROW_RIGHT;
        const float nextW = ImGui::CalcTextSize(next).x + ImGui::GetStyle().FramePadding.x * 2.0f;
        ImGui::SameLine(contentMaxX() - nextW);
        const bool ready = guideDone(lesson_);
        if (ready) {
            ImGui::PushStyleColor(ImGuiCol_Button, col(palette_.accent));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
        }
        if (ImGui::Button(next)) {
            if (last)
                open = false;
            else
                go(lesson_ + 1);
        }
        if (ready)
            ImGui::PopStyleColor(2);
        registerLastItem("git-guide:next", "button", "Next lesson", [this]() { lesson_ = std::min(kLessonCount - 1, lesson_ + 1); });
    }
    ImGui::End();
    if (!open)
        closeGuide();
}

} // namespace enp::view
