// Git interface over GitViewModel: the explorer's Git tab (changes, staging, commit, stashes), the
// branch picker and its clean-up dialog, the History tab of the bottom panel (log and compare), the
// confirmations and problems git raises, and "Learn Git", a step-by-step guide that points at these
// controls while the view-model shows demo data.
#pragma once

#include "enp/model/Git.h"
#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/view/UiAutomation.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace enp::view {

class GitView {
public:
    GitView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    void renderChanges(); // the explorer's Git tab
    void renderHistory(); // the History tab of the bottom panel
    void renderStatus();  // the Status tab: what `git status` tells, at a glance
    void renderPopups();  // branch picker, dialogs and the guide; once per frame
    void openBranchPicker(ImVec2 anchor, bool above); // anchor: where the popup hangs from
    void openGuide();

    int changeCount() const;
    const std::string& branch() const;
    int ahead() const;
    int behind() const;
    bool merging() const;
    bool guideRunning() const { return guideOpen_; }
    bool demo() const;
    bool repository() const; // a git repository (and git found) or the demo

private:
    using Area = model::git::Area;
    using Change = model::git::Change;
    using FileChange = model::git::FileChange;
    using Branch = model::git::Branch;
    using Commit = model::git::Commit;
    using Stash = model::git::Stash;
    enum class Source { Commit, Stash, Compare };

    viewmodel::GitViewModel& git() const { return vm_.git(); }
    const viewmodel::GitRepository& repo() const { return vm_.git().repo(); }

    // Git tab
    void renderBranchStrip();
    void renderMoreMenu();
    void renderCommitBox();
    void renderSelectionBar();
    void renderGroup(Area group, const char* title);
    void renderRow(const Change& change, int index);
    void renderStashes();
    void renderNotRepository();
    // Status tab
    void renderStatusSummary();
    void renderStatusFiles();
    void renderStatusRow(const Change& change);
    void renderMergeBanner();
    void clickRow(const Change& change);
    std::vector<std::string> targets(const Change& clicked) const; // the selection if it holds the row, else the row
    bool hasEdits(const std::string& path) const;

    // Branches
    void renderBranchPicker();
    void renderCleanupDialog();
    void renderStashDialog();
    void branchMenu(const Branch& branch);
    void renderRemotesDialog();
    void renderCloneDialog();

    // History
    void renderLog();
    void renderCompare();
    void renderCommitDetails(const Commit& c);
    void commitMenuItems(const Commit& c);
    void compareWith(const std::string& ref);
    void showFileHistory(const std::string& path);

    // Confirmations, questions and git's problems
    void confirm(std::string title, std::string text, std::string button, std::function<void()> action, bool danger = true);
    void ask(std::string title, std::string label, std::string value, std::string button, std::function<void(const std::string&)> action,
             bool branchName = true);
    void renderConfirm();
    void renderInput();
    void renderProblem();
    void discardWithConfirm(const std::vector<std::string>& paths);

    // Demo data, and the beginner help around it
    void renderDemoBanner();
    void note(const std::string& whatHappened); // status bar, and "What just happened" in the guide
    const UiElement* hoveredGitElement() const;

    // Learn Git
    void renderGuide();
    void guideShowMe(int lesson);
    bool guideDone(int lesson) const;
    void drawGuideDiagram(int lesson, float height);
    void baselineGuide();
    void closeGuide();

    // Shared
    void fileRow(const FileChange& file, const std::string& ref, Source source, bool* check = nullptr);
    void fileActionItems(const FileChange& file, const std::string& ref, Source source);
    ImU32 statusColor(char status) const;
    bool iconButton(const char* id, const char* icon, const char* tooltip);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;

    // Git tab state.
    std::set<std::string> selected_; // paths; Ctrl/Shift-click to select several
    std::string anchor_;             // where a Shift-click range starts

    // History state.
    int historyMode_ = 0; // 0 = log, 1 = compare
    int selectedCommit_ = 0;
    std::string historyFilter_;
    std::string historyPath_; // "File history": the log of one file
    int historyBranch_ = 0;   // 0 = current, 1 = all
    int compareBase_ = 0;     // index into refNames()
    int compareTarget_ = 2;
    std::vector<int> pickCommits_; // checked rows in the compare view (0/1)
    std::vector<int> pickFiles_;
    std::string pickedFor_;        // compare base...target the checks belong to
    bool stageTaken_ = true;

    // Branch picker and dialogs.
    std::string branchFilter_;
    std::string newBranch_;
    std::string createFrom_; // a commit, when the picker was opened by "Create branch here"
    bool creatingBranch_ = false;
    bool switchToNew_ = true;
    bool sortByName_ = false;
    bool openPicker_ = false;
    ImVec2 pickerAnchor_;
    bool pickerAbove_ = true;
    bool openCleanup_ = false;
    std::vector<int> cleanupChecks_;
    bool cleanupRemote_ = false;
    bool openStash_ = false;
    std::string stashMessage_;
    bool stashUntracked_ = true;
    bool stashSelectedOnly_ = false;
    bool stashKeepIndex_ = false;
    struct Confirm {
        std::string title, text, button;
        bool danger = true;
        std::function<void()> action;
    } confirm_;
    bool openConfirm_ = false;
    struct Input {
        std::string title, label, value, button;
        bool branchName = true;
        std::function<void(const std::string&)> action;
    } input_;
    bool openInput_ = false;
    bool showOutput_ = false;
    bool openRemotes_ = false;
    std::string newRemoteName_, newRemoteUrl_;
    bool openClone_ = false;
    std::string cloneUrl_, cloneFolder_, cloneName_;
public:
    void openClone() { openClone_ = true; }
private:

    // Learn Git: the open lesson, what the user has done since the guide opened (for the "Try it"
    // ticks), and the review checklist.
    bool guideOpen_ = false;
    bool guideAppearing_ = false;
    int lesson_ = 0;
    bool showPlanNotes_ = true;
    std::vector<int> reviewed_;
    size_t baseCommits_ = 0, baseStashes_ = 0, baseBranches_ = 0;
    std::string baseBranch_;
    bool sawDiff_ = false;
    bool sawMerge_ = false;
    bool tookFromCompare_ = false;
    bool cleanedUp_ = false;
    std::string lastAction_;   // "What just happened"
    std::string seenStatus_;   // status text already turned into lastAction_
    std::string hoverTitle_, hoverText_; // "What's this?": the last explained control under the mouse
    std::vector<viewmodel::Subscription> subscriptions_;
};

} // namespace enp::view
