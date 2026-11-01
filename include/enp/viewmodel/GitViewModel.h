// Source control: the repository's state and every git action, run through the git command line
// (one command at a time, no shell). While the Learn Git guide runs, or when no real folder is open,
// the same actions work on a made-up demo repository instead, so nothing real is touched.
#pragma once

#include "enp/interfaces/IAgentCliService.h"
#include "enp/interfaces/IFileService.h"
#include "enp/interfaces/IProcessService.h"
#include "enp/model/Diff.h"
#include "enp/model/Git.h"

#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace enp::viewmodel {

class NotepadViewModel;

enum class GitOperation { None, Merge, Rebase, CherryPick, Revert };

struct GitRepository {
    bool repository = false;
    std::string root;   // the repository's top folder, absolute
    std::string prefix; // the workspace folder inside it: "" or "sub/dir/"
    model::git::Status status;
    std::vector<model::git::Branch> branches; // local, remote and tags
    std::vector<model::git::Stash> stashes;
    std::vector<model::git::Commit> log;      // History (loaded when asked for)
    bool logLoaded = false;
    bool logComplete = false;
    std::vector<model::git::Commit> compareCommits;
    std::vector<model::git::FileChange> compareFiles;
    std::string compareBase, compareTarget;
    // The last commit, and when the remote was last fetched (0: never), for the Status tab.
    std::string headShort, headSubject, headAuthor;
    int64_t headTime = 0;
    int64_t fetchedAt = 0;
    GitOperation operation = GitOperation::None;
    std::string operationLabel; // "Merging main into feature"
    int operationStep = 0, operationTotal = 0;
};

struct GitRemote {
    std::string name;
    std::string fetchUrl, pushUrl;
};

// Something that needs the user before git can go on.
struct GitProblem {
    enum class Kind { None, SwitchBlocked, BranchNotMerged, AuthFailed, PushRejected, Failed };
    Kind kind = Kind::None;
    std::string subject; // the branch, or the command
    std::string detail;  // git's message
};

// The Diff tab's current git diff, so its changes can be staged, unstaged or discarded in parts.
struct GitDiffContext {
    bool active = false;
    std::string path;
    model::git::Area area = model::git::Area::Changes;
    std::string oldText, newText;
    std::vector<model::DiffRow> rows;
    std::string title;
};

// Per line of a file: what changed since the last commit (gutter markers).
enum class LineChange : char { None, Added, Modified, DeletedAbove };

enum class AfterCommitWhat { Nothing, Push, Sync };

class GitViewModel {
public:
    explicit GitViewModel(NotepadViewModel& owner);
    void setServices(interfaces::IProcessService* processes, interfaces::IFileService* files,
                     interfaces::IAgentCliService* agent = nullptr);

    // ---- Mode -----------------------------------------------------------------------------
    bool demo() const { return learning_ || owner_workspaceEmpty(); }
    void setLearning(bool learning); // the guide forces demo data
    bool learning() const { return learning_; }
    void resetDemo();
    void workspaceChanged(); // a folder was opened: find its repository
    // Demo only: show a merge in progress, or a folder that is not a repository.
    void setDemoMerge(bool on);
    void setDemoNotRepository(bool on);

    // ---- State ----------------------------------------------------------------------------
    const GitRepository& repo() const { return demo() ? demo_ : real_; }
    bool available() const { return demo() || gitFound_; }
    const std::string& unavailableReason() const { return unavailable_; }
    bool busy() const { return !demo() && (running_ || !queue_.empty()); }
    const GitProblem& problem() const { return problem_; }
    void clearProblem() { problem_ = {}; }
    const std::string& lastOutput() const { return lastOutput_; } // of the last command, for "Show output"
    // Changes counted for the tab badge (a file staged and changed again counts once).
    int changeCount() const;
    std::vector<std::string> localBranchNames() const;
    std::vector<std::string> refNames() const; // current first, then local, remote, tags

    // ---- Refresh --------------------------------------------------------------------------
    void refresh();
    void requestRefresh(); // soon, when nothing else runs
    void setVisible(bool visible) { visible_ = visible; } // the Git tab, chip or History is on screen
    void tick(double now);

    // ---- Changes and commit ---------------------------------------------------------------
    std::string commitMessage; // the Git tab's message box
    bool amend = false;
    void commitWithMessage(AfterCommitWhat after = AfterCommitWhat::Nothing); // commitMessage, then clears it
    std::vector<std::string> unstagedPaths() const;
    // A message from the staged files, used when no agent is connected.
    std::string suggestMessage() const;
    // Fills commitMessage: the agent describes the staged diff, or suggestMessage() without one.
    void writeMessage();
    bool writingMessage() const { return writingMessage_; }
    void stage(const std::vector<std::string>& paths);
    void unstage(const std::vector<std::string>& paths);
    void discard(const std::vector<std::string>& paths); // tracked: restore; untracked: delete
    using AfterCommit = AfterCommitWhat;
    void commit(const std::string& message, bool amend, AfterCommit after = AfterCommit::Nothing);
    void init();
    void showChange(const std::string& path, model::git::Area area); // in the Diff tab
    void updateSubmodule(const std::string& path);                     // checks out the recorded commit
    void showStatusText();                                             // `git status` in lastOutput()
    void openFile(const std::string& path);
    std::string workspacePath(const std::string& repoPath) const;
    std::string repoPath(const std::string& workspacePath) const;

    // ---- Branches -------------------------------------------------------------------------
    void switchTo(const std::string& name);
    void stashAndSwitch(const std::string& name);
    void createBranch(const std::string& name, const std::string& from, bool switchTo);
    void renameBranch(const std::string& from, const std::string& to);
    void deleteBranch(const std::string& name, bool force);
    void deleteRemoteBranch(const std::string& name); // "origin/feature"
    void deleteBranches(const std::vector<std::string>& names, bool onRemoteToo);
    void merge(const std::string& name);
    void rebase(const std::string& onto);
    void continueOperation();
    void abortOperation();
    void skipOperation();

    // ---- Sync -----------------------------------------------------------------------------
    void fetch();
    void pull();
    void push(bool force = false);
    void sync();
    void runFailedInTerminal(); // the command that needed a password, in the terminal

    // ---- Remotes and clone ----------------------------------------------------------------
    const std::vector<GitRemote>& remotes() const { return demo() ? demoRemotes_ : remotes_; }
    void loadRemotes();
    void addRemote(const std::string& name, const std::string& url);
    void removeRemote(const std::string& name);
    void renameRemote(const std::string& from, const std::string& to);
    void setRemoteUrl(const std::string& name, const std::string& url);
    // Clones `url` into parentFolder/name and opens that folder.
    void clone(const std::string& url, const std::string& parentFolder, const std::string& name);
    static std::string cloneName(const std::string& url); // "https://x/y/repo.git" -> "repo"
    // Git status letter of a workspace file ('M', 'A', 'U', ...), or 0; and whether a folder holds changes.
    char statusOf(const std::string& workspacePath) const;
    bool folderChanged(const std::string& workspaceFolder) const;

    // ---- Stash ----------------------------------------------------------------------------
    void stashPush(const std::string& message, const std::vector<std::string>& paths, bool untracked, bool keepIndex);
    void stashApply(int index, bool pop);
    void stashDrop(int index);
    void stashBranch(int index, const std::string& branch);
    void loadStashFiles(int index);

    // ---- History and compare --------------------------------------------------------------
    void loadLog(bool allBranches, const std::string& path, bool more = false);
    void loadCommitFiles(int index);
    void showCommitFile(const std::string& hash, const model::git::FileChange& file);
    // The file as it is in `oldRef` and in `newRef`, side by side in the Diff tab.
    void showFileBetween(const std::string& oldRef, const std::string& newRef, const model::git::FileChange& file);
    void cherryPick(const std::vector<std::string>& hashes); // oldest first
    void revert(const std::string& hash);
    enum class ResetMode { Soft, Mixed, Hard };
    void reset(const std::string& hash, ResetMode mode);
    void tag(const std::string& name, const std::string& hash);
    void checkoutDetached(const std::string& hash);
    void compare(const std::string& base, const std::string& target);
    void takeFiles(const std::string& ref, const std::vector<std::string>& paths, bool stage);
    void restoreFile(const std::string& ref, const std::string& path);
    void applyFileChange(const std::string& commit, const std::string& path);
    void openVersion(const std::string& ref, const std::string& path);
    void compareWithWorkingTree(const std::string& ref, const std::string& path);

    // ---- Line level -----------------------------------------------------------------------
    const GitDiffContext& diffContext() const { return diff_; }
    enum class RowsAction { Stage, Unstage, Discard };
    void applyRows(const std::vector<bool>& chosen, RowsAction action); // rows of diffContext()
    // Gutter markers for an open document (computed against HEAD, cached per text version; `text`
    // is only asked for when the version changed).
    const std::vector<LineChange>& lineChanges(const std::string& workspacePath, long long version,
                                               const std::function<std::string()>& text);
    // The change around `line` (from lineChanges): its lines now [first, last] (last < first: a
    // deletion just above `first`) and what they were at HEAD.
    bool changeAt(const std::string& workspacePath, int line, int& first, int& last, std::vector<std::string>& headLines) const;
    // A document was saved: a conflicted file without markers left is marked resolved (staged).
    void documentSaved(const std::string& workspacePath, const std::string& text);
    // Blame for an open document; empty until loaded.
    const std::vector<model::git::BlameLine>& blame(const std::string& workspacePath) const;
    void loadBlame(const std::string& workspacePath, const std::string& text);
    bool blameShown(const std::string& workspacePath) const;
    void toggleBlame(const std::string& workspacePath, const std::string& text);

private:
    struct Job {
        std::vector<std::string> args;
        std::string input;
        std::function<void(const interfaces::ProcessResult&)> done;
        bool quiet = false; // read-only: no lock taken, errors not shown
        std::string directory; // empty: the repository
    };
    void run(std::vector<std::string> args, std::function<void(const interfaces::ProcessResult&)> done = {},
             std::string input = {}, bool quiet = false);
    void runNext();
    // Runs a command that changes the repository, reports `success` or the error, then refreshes.
    void act(std::vector<std::string> args, std::string success, std::function<void()> after = {}, std::string input = {});
    bool failed(const interfaces::ProcessResult& r, const std::vector<std::string>& args);
    void note(const std::string& text);
    bool owner_workspaceEmpty() const;
    void detectOperation();
    void reloadOpenDocuments();
    void loadDemo();
    model::git::Change* demoChange(const std::string& path, model::git::Area area);

    NotepadViewModel& owner_;
    interfaces::IProcessService* processes_ = nullptr;
    interfaces::IFileService* files_ = nullptr;
    interfaces::IAgentCliService* agent_ = nullptr;
    bool writingMessage_ = false;
    GitRepository real_;
    GitRepository demo_;
    bool learning_ = false;
    bool gitFound_ = false;
    std::string unavailable_;
    std::string gitDir_;
    std::string defaultRemote_ = "origin";
    bool pullRebase_ = false;
    std::deque<Job> queue_;
    bool running_ = false;
    bool refreshWanted_ = false;
    bool refreshing_ = false;
    bool visible_ = false;
    double now_ = 0.0, lastRefresh_ = 0.0;
    bool logAll_ = false;
    std::string logPath_;
    GitProblem problem_;
    std::vector<std::string> failedArgs_;
    std::string lastOutput_;
    GitDiffContext diff_;
    struct LineCache {
        std::string path;
        std::string head;
        long long version = -1;
        std::string headText;
        bool headLoaded = false;
        std::vector<LineChange> changes;
        std::vector<model::DiffRow> rows;
    };
    std::vector<LineCache> lineCache_;
    std::vector<std::pair<std::string, std::vector<model::git::BlameLine>>> blames_;
    std::vector<std::string> blameShown_;
    std::vector<GitRemote> remotes_, demoRemotes_{{"origin", "https://github.com/example/easynotepad.git", "https://github.com/example/easynotepad.git"}};
};

} // namespace enp::viewmodel
