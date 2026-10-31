#include "enp/viewmodel/GitViewModel.h"

#include "enp/viewmodel/NotepadViewModel.h"

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <tuple>

namespace enp::viewmodel {

using model::git::Area;
using model::git::Branch;
using model::git::Change;
using model::git::Commit;
using model::git::FileChange;
using model::git::RefKind;
using model::git::Stash;

namespace {

int64_t nowSeconds() { return int64_t(std::time(nullptr)); }

std::string fileName(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string plural(size_t n, const char* one, const char* many) { return std::to_string(n) + " " + (n == 1 ? one : many); }

// The first line of git's complaint, without the "error: " / "fatal: " prefix.
std::string firstLine(const interfaces::ProcessResult& r)
{
    const std::string& text = !r.errorOutput.empty() ? r.errorOutput : !r.output.empty() ? r.output : r.error;
    for (const std::string& line : model::git::lines(text)) {
        std::string s = line;
        for (const char* prefix : {"error: ", "fatal: ", "hint: "})
            if (s.rfind(prefix, 0) == 0)
                s = s.substr(std::char_traits<char>::length(prefix));
        if (!s.empty() && line.rfind("hint: ", 0) != 0)
            return s;
    }
    return r.error.empty() ? "git failed" : r.error;
}

bool contains(const std::string& text, const char* what) { return text.find(what) != std::string::npos; }

void addTimes(std::vector<Commit>& commits)
{
    const int64_t now = nowSeconds();
    for (Commit& c : commits)
        c.when = model::git::relativeTime(c.time, now);
}

} // namespace

GitViewModel::GitViewModel(NotepadViewModel& owner) : owner_(owner)
{
    loadDemo();
}

void GitViewModel::setServices(interfaces::IProcessService* processes, interfaces::IFileService* files,
                               interfaces::IAgentCliService* agent)
{
    processes_ = processes;
    files_ = files;
    agent_ = agent;
}

// Demo data stands in for the sample workspace only; after Close Folder there is simply no repository.
bool GitViewModel::owner_workspaceEmpty() const { return owner_.workspaceRoot().empty() && !owner_.noFolder(); }

void GitViewModel::note(const std::string& text) { owner_.statusText.set((demo() ? "Demo: " : "Git: ") + text); }

int GitViewModel::changeCount() const
{
    std::vector<std::string> seen;
    for (const Change& c : repo().status.changes)
        if (std::find(seen.begin(), seen.end(), c.path) == seen.end())
            seen.push_back(c.path);
    return int(seen.size());
}

std::vector<std::string> GitViewModel::localBranchNames() const
{
    std::vector<std::string> names;
    for (const Branch& b : repo().branches)
        if (b.kind == RefKind::Local)
            names.push_back(b.name);
    return names;
}

std::vector<std::string> GitViewModel::refNames() const
{
    const GitRepository& r = repo();
    std::vector<std::string> names{r.status.branch};
    for (RefKind kind : {RefKind::Local, RefKind::Remote, RefKind::Tag})
        for (const Branch& b : r.branches)
            if (b.kind == kind && b.name != r.status.branch)
                names.push_back(b.name);
    return names;
}

std::string GitViewModel::workspacePath(const std::string& repoPath) const
{
    const GitRepository& r = repo();
    if (demo() || r.prefix.empty())
        return repoPath;
    if (repoPath.rfind(r.prefix, 0) == 0)
        return repoPath.substr(r.prefix.size());
    return r.root + "/" + repoPath; // outside the opened folder: absolute
}

std::string GitViewModel::repoPath(const std::string& workspacePath) const { return repo().prefix + workspacePath; }

// ---- Running git --------------------------------------------------------------------------------

void GitViewModel::run(std::vector<std::string> args, std::function<void(const interfaces::ProcessResult&)> done, std::string input, bool quiet)
{
    queue_.push_back({std::move(args), std::move(input), std::move(done), quiet, {}});
    if (!running_)
        runNext();
}

void GitViewModel::runNext()
{
    if (queue_.empty() || !processes_) {
        running_ = false;
        queue_.clear();
        return;
    }
    running_ = true;
    Job job = std::move(queue_.front());
    queue_.pop_front();
    interfaces::ProcessRequest request;
    std::string program = "git";
    if (std::optional<std::string> custom = processes_->setting("EASYNOTEPAD_GIT"))
        program = *custom;
    request.arguments = {program, "-c", "core.quotepath=off", "-c", "color.ui=false", "-c", "advice.detachedHead=false"};
    request.arguments.insert(request.arguments.end(), job.args.begin(), job.args.end());
    // Never wait for a password nobody can type; never open an editor.
    request.environment = {{"GIT_TERMINAL_PROMPT", "0"}, {"GIT_EDITOR", "true"}, {"GIT_SEQUENCE_EDITOR", "true"},
                           {"GCM_INTERACTIVE", "never"}};
    if (job.quiet)
        request.environment.push_back({"GIT_OPTIONAL_LOCKS", "0"});
    request.input = std::move(job.input);
    request.workingDirectory = !job.directory.empty() ? job.directory : real_.root.empty() ? owner_.workspaceRoot() : real_.root;
    processes_->run(request, [this, done = std::move(job.done)](const interfaces::ProcessResult& r) {
        if (done)
            done(r);
        runNext();
    });
}

bool GitViewModel::failed(const interfaces::ProcessResult& r, const std::vector<std::string>& args)
{
    lastOutput_ = r.output + r.errorOutput;
    if (r.started && r.exitCode == 0)
        return false;
    const std::string all = r.output + "\n" + r.errorOutput;
    problem_ = {};
    if (contains(all, "terminal prompts disabled") || contains(all, "could not read Username") ||
        contains(all, "Authentication failed") || contains(all, "Permission denied (publickey)") ||
        contains(all, "could not read Password")) {
        problem_ = {GitProblem::Kind::AuthFailed, "git " + args.front(), "The server needs you to sign in."};
        failedArgs_ = args;
    } else if (contains(all, "[rejected]") || contains(all, "fetch first") || contains(all, "non-fast-forward")) {
        problem_ = {GitProblem::Kind::PushRejected, real_.status.branch, firstLine(r)};
    } else
        problem_ = {GitProblem::Kind::Failed, "git " + args.front(), firstLine(r)};
    owner_.statusText.set("Git: " + firstLine(r));
    return true;
}

void GitViewModel::act(std::vector<std::string> args, std::string success, std::function<void()> after, std::string input)
{
    run(args, [this, args, success, after](const interfaces::ProcessResult& r) {
        if (!failed(r, args)) {
            if (!success.empty())
                note(success);
            if (after)
                after();
        }
        reloadOpenDocuments();
        refresh();
    }, std::move(input));
}

void GitViewModel::reloadOpenDocuments() { owner_.checkDiskChanges(); }

// ---- Repository and refresh ---------------------------------------------------------------------

void GitViewModel::workspaceChanged()
{
    real_ = GitRepository{};
    gitFound_ = false;
    unavailable_.clear();
    diff_ = {};
    lineCache_.clear();
    blames_.clear();
    if (owner_.workspaceRoot().empty() || !processes_)
        return;
    run({"--version"}, [this](const interfaces::ProcessResult& r) {
        gitFound_ = r.started && r.exitCode == 0;
        if (!gitFound_)
            unavailable_ = r.error.empty() ? "git did not run" : r.error;
    }, {}, true);
    run({"rev-parse", "--show-toplevel", "--absolute-git-dir", "--show-prefix"}, [this](const interfaces::ProcessResult& r) {
        if (!gitFound_)
            return;
        const std::vector<std::string> out = model::git::lines(r.output);
        real_.repository = r.exitCode == 0 && out.size() >= 2;
        if (!real_.repository)
            return;
        real_.root = out[0];
        gitDir_ = out[1];
        real_.prefix = out.size() >= 3 ? out[2] : std::string();
        refresh();
    }, {}, true);
    run({"config", "--get", "pull.rebase"}, [this](const interfaces::ProcessResult& r) {
        pullRebase_ = r.exitCode == 0 && model::git::lines(r.output).size() > 0 && model::git::lines(r.output)[0] != "false";
    }, {}, true);
    run({"remote"}, [this](const interfaces::ProcessResult& r) {
        const std::vector<std::string> remotes = model::git::lines(r.output);
        if (!remotes.empty() && std::find(remotes.begin(), remotes.end(), "origin") == remotes.end())
            defaultRemote_ = remotes.front();
    }, {}, true);
}

void GitViewModel::requestRefresh() { refreshWanted_ = true; }

void GitViewModel::tick(double now)
{
    now_ = now;
    if (demo() || !real_.repository || refreshing_ || running_)
        return;
    // After a save or an action, soon; while a git view is on screen, every few seconds (another
    // program, or the terminal, may have changed things).
    if ((refreshWanted_ && now - lastRefresh_ > 0.3) || (visible_ && now - lastRefresh_ > 3.0))
        refresh();
}

void GitViewModel::refresh()
{
    if (demo() || !real_.repository || refreshing_)
        return;
    refreshing_ = true;
    refreshWanted_ = false;
    lastRefresh_ = now_;
    run({"status", "--porcelain=v2", "--branch", "-z", "--untracked-files=all"}, [this](const interfaces::ProcessResult& r) {
        if (r.exitCode == 0)
            real_.status = model::git::parseStatus(r.output);
    }, {}, true);
    run({"for-each-ref", std::string("--format=") + model::git::branchFormat(), "refs/heads", "refs/remotes", "refs/tags"},
        [this](const interfaces::ProcessResult& r) {
            if (r.exitCode == 0)
                real_.branches = model::git::parseBranches(r.output);
        }, {}, true);
    run({"branch", "--merged", "HEAD", "--format=%(refname:short)"}, [this](const interfaces::ProcessResult& r) {
        const std::vector<std::string> merged = model::git::lines(r.output);
        for (Branch& b : real_.branches)
            b.merged = b.kind == RefKind::Local && !b.current && std::find(merged.begin(), merged.end(), b.name) != merged.end();
    }, {}, true);
    run({"stash", "list", "--format=%gd%x1f%gs%x1f%ct"}, [this](const interfaces::ProcessResult& r) {
        std::vector<Stash> stashes = model::git::parseStashList(r.output);
        // Keep the files of stashes already opened.
        for (Stash& s : stashes) {
            s.when = model::git::relativeTime(s.time, nowSeconds());
            for (const Stash& old : real_.stashes)
                if (old.ref == s.ref && old.time == s.time)
                    s.files = old.files;
        }
        real_.stashes = std::move(stashes);
    }, {}, true);
    run({"rev-parse", "--verify", "-q", "HEAD"}, [this](const interfaces::ProcessResult&) {
        detectOperation();
        refreshing_ = false;
        if (real_.logLoaded)
            loadLog(logAll_, logPath_);
    }, {}, true);
}

void GitViewModel::detectOperation()
{
    GitRepository& r = real_;
    r.operation = GitOperation::None;
    r.operationLabel.clear();
    r.operationStep = r.operationTotal = 0;
    if (!files_ || gitDir_.empty())
        return;
    auto exists = [this](const std::string& name) {
        const std::optional<interfaces::FileStamp> s = files_->stamp(gitDir_ + "/" + name);
        return (s && s->exists) || files_->isDirectory(gitDir_ + "/" + name);
    };
    auto readNumber = [this](const std::string& name) {
        const std::optional<std::string> text = files_->read(gitDir_ + "/" + name);
        return text ? std::atoi(text->c_str()) : 0;
    };
    if (exists("rebase-merge") || exists("rebase-apply")) {
        r.operation = GitOperation::Rebase;
        const std::string dir = exists("rebase-merge") ? "rebase-merge/" : "rebase-apply/";
        r.operationStep = readNumber(dir + (dir == "rebase-merge/" ? "msgnum" : "next"));
        r.operationTotal = readNumber(dir + (dir == "rebase-merge/" ? "end" : "last"));
        r.operationLabel = "Rebasing " + r.status.branch;
    } else if (exists("MERGE_HEAD")) {
        r.operation = GitOperation::Merge;
        std::string from = "another branch";
        if (std::optional<std::string> msg = files_->read(gitDir_ + "/MERGE_MSG")) {
            const size_t q = msg->find('\'');
            const size_t q2 = q == std::string::npos ? q : msg->find('\'', q + 1);
            if (q2 != std::string::npos)
                from = msg->substr(q + 1, q2 - q - 1);
        }
        r.operationLabel = "Merging " + from + " into " + r.status.branch;
    } else if (exists("CHERRY_PICK_HEAD")) {
        r.operation = GitOperation::CherryPick;
        r.operationLabel = "Cherry-picking onto " + r.status.branch;
    } else if (exists("REVERT_HEAD")) {
        r.operation = GitOperation::Revert;
        r.operationLabel = "Reverting on " + r.status.branch;
    }
}

// ---- Demo repository ----------------------------------------------------------------------------

void GitViewModel::loadDemo()
{
    // A small made-up repository, so every control has something to show. Reset puts it back.
    const int64_t now = nowSeconds();
    GitRepository& d = demo_;
    d = GitRepository{};
    d.repository = true;
    d.status.branch = "feature/git-ui";
    d.status.upstream = "origin/feature/git-ui";
    d.status.ahead = 2;
    d.status.changes = {
        {"include/enp/model/Git.h", 'A', Area::Staged, {}, {}},   {"src/app/Main.cpp", 'M', Area::Changes, {}, {}},
        {"src/view/MainView.cpp", 'M', Area::Changes, {}, {}},    {"docs/notes.txt", 'D', Area::Changes, {}, {}},
        {"src/model/Git.cpp", 'U', Area::Untracked, {}, {}},      {"src/view/GitView.cpp", 'U', Area::Untracked, {}, {}},
        {"3rdparty/imgui", 'M', Area::Changes, {}, "new commits"},
    };
    auto local = [&](const char* name, const char* upstream, int ahead, int behind, bool merged, bool gone, int minutes) {
        Branch b;
        b.name = name;
        b.kind = RefKind::Local;
        b.upstream = upstream;
        b.ahead = ahead;
        b.behind = behind;
        b.merged = merged;
        b.gone = gone;
        b.current = b.name == d.status.branch;
        b.lastUsed = now - minutes * 60;
        d.branches.push_back(b);
    };
    local("feature/git-ui", "origin/feature/git-ui", 2, 0, false, false, 0);
    local("main", "origin/main", 0, 0, false, false, 30);
    local("fix/tab-overflow", "origin/fix/tab-overflow", 0, 3, false, false, 300);
    local("fix/ribbon-caption", "origin/fix/ribbon-caption", 0, 0, true, false, 2900);
    local("feature/tabs", "origin/feature/tabs", 0, 0, true, true, 8000);
    local("spike/old-layout", "", 0, 0, false, false, 20000);
    for (const char* name : {"origin/main", "origin/feature/git-ui", "origin/fix/tab-overflow", "origin/fix/ribbon-caption", "origin/release-1.2"}) {
        Branch b;
        b.name = name;
        b.kind = RefKind::Remote;
        d.branches.push_back(b);
    }
    for (const char* name : {"v0.1.0", "v0.0.9"}) {
        Branch b;
        b.name = name;
        b.kind = RefKind::Tag;
        d.branches.push_back(b);
    }
    auto stash = [&](const char* message, const char* when, const char* branch, std::vector<FileChange> files) {
        Stash s;
        s.ref = "stash@{" + std::to_string(d.stashes.size()) + "}";
        s.message = message;
        s.when = when;
        s.branch = branch;
        s.files = std::move(files);
        d.stashes.push_back(s);
    };
    stash("tidy ribbon captions", "yesterday", "main", {{"src/view/RibbonView.cpp", 'M', 14, 6}, {"src/view/Theme.cpp", 'M', 3, 3}});
    stash("experiment: wider explorer", "3 days ago", "feature/git-ui", {{"src/view/MainView.cpp", 'M', 9, 2}});
    auto commit = [&](const char* hash, const char* message, const char* author, const char* when, const char* refs, int lane,
                      std::vector<std::pair<int, int>> edges, std::vector<FileChange> files) {
        Commit c;
        c.hash = c.shortHash = hash;
        c.message = message;
        c.author = author;
        c.when = when;
        c.refs = refs;
        c.lane = lane;
        c.edges = std::move(edges);
        c.files = std::move(files);
        d.log.push_back(c);
    };
    commit("a1b2c3d", "Add the Git tab preview", "Mei Chong", "12 min ago", "HEAD -> feature/git-ui", 0, {{0, 0}},
           {{"src/view/GitView.cpp", 'A', 412, 0}, {"src/view/MainView.cpp", 'M', 38, 6}, {"CMakeLists.txt", 'M', 1, 0}});
    commit("9f8e7d6", "Add git status model", "Mei Chong", "2 hours ago", "origin/feature/git-ui", 0, {{0, 0}},
           {{"include/enp/model/Git.h", 'A', 64, 0}, {"src/model/Git.cpp", 'A', 180, 0}});
    commit("77c1e02", "Merge main into feature/git-ui", "Mei Chong", "5 hours ago", "", 0, {{0, 0}, {0, 1}}, {{"src/view/RibbonView.cpp", 'M', 12, 4}});
    commit("4455aa1", "Fix ribbon overflow on narrow windows", "Sam Rivera", "yesterday", "origin/main, main", 1, {{0, 0}, {1, 1}},
           {{"src/view/RibbonView.cpp", 'M', 12, 4}});
    commit("3e9d0b7", "Parse porcelain v2 status", "Mei Chong", "yesterday", "", 0, {{0, 0}, {1, 1}},
           {{"src/model/Git.cpp", 'M', 96, 12}, {"tests/TestMain.cpp", 'M', 48, 0}});
    commit("c0ffee1", "Tabbed explorer: Files and Open", "Sam Rivera", "2 days ago", "", 1, {{0, 0}, {1, 1}}, {{"src/view/MainView.cpp", 'M', 57, 31}});
    commit("5a5a5a5", "Start the git UI branch", "Mei Chong", "2 days ago", "", 0, {{0, 1}, {1, 1}},
           {{"docs/plan/14-git-status-and-commit.md", 'A', 120, 0}});
    commit("b16b00b", "Smart indent for YAML lists", "Sam Rivera", "3 days ago", "tag: v0.1.0", 1, {{1, 1}}, {{"src/model/Indent.cpp", 'M', 22, 3}});
    commit("1234abc", "Pop-out terminal window", "Mei Chong", "4 days ago", "", 1, {{1, 1}},
           {{"src/view/MainView.cpp", 'M', 88, 10}, {"src/app/MainGlfw.cpp", 'M', 40, 2}});
    commit("0badf00", "Idle loop: wait for events", "Mei Chong", "5 days ago", "", 1, {{1, 1}}, {{"src/app/MainGlfw.cpp", 'M', 35, 9}});
    commit("d00d1e5", "Rounded panel corners done right", "Sam Rivera", "6 days ago", "tag: v0.0.9", 1, {{1, 1}}, {{"src/view/PanelFrame.cpp", 'A', 70, 0}});
    commit("feedbee", "Initial commit", "Mei Chong", "2 weeks ago", "", 1, {}, {{"README.md", 'A', 135, 0}});
    d.logLoaded = d.logComplete = true;
}

void GitViewModel::resetDemo()
{
    loadDemo();
    note("the demo data is back to how it started");
}

void GitViewModel::setLearning(bool learning)
{
    learning_ = learning;
    diff_ = {};
    if (!learning)
        requestRefresh();
}

void GitViewModel::setDemoMerge(bool on)
{
    if (!demo())
        return;
    demo_.operation = on ? GitOperation::Merge : GitOperation::None;
    demo_.operationLabel = on ? "Merging main into " + demo_.status.branch : std::string();
    auto& changes = demo_.status.changes;
    changes.erase(std::remove_if(changes.begin(), changes.end(), [](const Change& c) { return c.group == Area::Conflicts; }), changes.end());
    if (on) {
        changes.push_back({"src/view/RibbonView.cpp", '!', Area::Conflicts, {}, {}});
        note("merging main into " + demo_.status.branch + ": RibbonView.cpp changed on both sides, so it needs you (see Conflicts)");
    }
}

void GitViewModel::setDemoNotRepository(bool on)
{
    if (demo())
        demo_.repository = !on;
}

Change* GitViewModel::demoChange(const std::string& path, Area area)
{
    for (Change& c : demo_.status.changes)
        if (c.path == path && c.group == area)
            return &c;
    return nullptr;
}

// ---- Changes and commit -------------------------------------------------------------------------

void GitViewModel::stage(const std::vector<std::string>& paths)
{
    if (paths.empty())
        return;
    if (demo()) {
        for (Change& c : demo_.status.changes)
            if (std::find(paths.begin(), paths.end(), c.path) != paths.end() && c.group != Area::Staged) {
                if (c.group == Area::Untracked)
                    c.status = 'A';
                c.group = Area::Staged;
            }
        // A file staged twice (staged + changed) becomes one staged entry.
        auto& v = demo_.status.changes;
        for (size_t i = 0; i < v.size(); ++i)
            for (size_t j = i + 1; j < v.size();)
                if (v[j].path == v[i].path && v[j].group == v[i].group)
                    v.erase(v.begin() + long(j));
                else
                    ++j;
        if (demo_.operation != GitOperation::None)
            for (const std::string& p : paths)
                if (p == "src/view/RibbonView.cpp")
                    for (Change& c : v)
                        if (c.path == p)
                            c.status = 'M';
        note(paths.size() == 1 ? fileName(paths[0]) + " is staged: it will go into your next commit"
                               : plural(paths.size(), "file is", "files are") + " staged for the next commit");
        return;
    }
    std::vector<std::string> args{"add", "-A", "--"};
    args.insert(args.end(), paths.begin(), paths.end());
    act(args, paths.size() == 1 ? fileName(paths[0]) + " is staged" : plural(paths.size(), "file is", "files are") + " staged");
}

void GitViewModel::unstage(const std::vector<std::string>& paths)
{
    if (paths.empty())
        return;
    if (demo()) {
        for (Change& c : demo_.status.changes)
            if (std::find(paths.begin(), paths.end(), c.path) != paths.end() && c.group == Area::Staged) {
                c.group = c.status == 'A' ? Area::Untracked : Area::Changes;
                if (c.group == Area::Untracked)
                    c.status = 'U';
            }
        note(paths.size() == 1 ? fileName(paths[0]) + " is out of the box again; your edits are still there"
                               : "unstaged; your edits are still there");
        return;
    }
    std::vector<std::string> args{"restore", "--staged", "--"};
    if (real_.status.initial)
        args = {"rm", "--cached", "-r", "-q", "--"};
    args.insert(args.end(), paths.begin(), paths.end());
    act(args, "unstaged; your edits are still there");
}

void GitViewModel::discard(const std::vector<std::string>& paths)
{
    if (paths.empty())
        return;
    if (demo()) {
        auto& v = demo_.status.changes;
        v.erase(std::remove_if(v.begin(), v.end(),
                               [&](const Change& c) {
                                   return c.group != Area::Staged && std::find(paths.begin(), paths.end(), c.path) != paths.end();
                               }),
                v.end());
        note("discarded the changes to " + (paths.size() == 1 ? fileName(paths[0]) : plural(paths.size(), "file", "files")));
        return;
    }
    std::vector<std::string> tracked, untracked;
    for (const std::string& p : paths) {
        const bool isUntracked = std::any_of(real_.status.changes.begin(), real_.status.changes.end(),
                                             [&](const Change& c) { return c.path == p && c.group == Area::Untracked; });
        (isUntracked ? untracked : tracked).push_back(p);
    }
    if (!tracked.empty()) {
        std::vector<std::string> args{"restore", "--worktree", "--"};
        args.insert(args.end(), tracked.begin(), tracked.end());
        act(args, "discarded the changes to " + plural(tracked.size(), "file", "files"));
    }
    if (!untracked.empty()) {
        std::vector<std::string> args{"clean", "-f", "-q", "--"};
        args.insert(args.end(), untracked.begin(), untracked.end());
        act(args, "deleted " + plural(untracked.size(), "untracked file", "untracked files"));
    }
}

void GitViewModel::commit(const std::string& message, bool amend, AfterCommit after)
{
    if (message.empty())
        return;
    const std::string subject = message.substr(0, message.find('\n'));
    const GitRepository& r = repo();
    const bool anyStaged = std::any_of(r.status.changes.begin(), r.status.changes.end(), [](const Change& c) { return c.group == Area::Staged; });
    if (demo()) {
        Commit c;
        c.hash = c.shortHash = "e" + std::to_string(1000000 + int(demo_.log.size()) * 7919).substr(1);
        c.message = subject;
        c.author = "Mei Chong";
        c.when = "just now";
        c.refs = "HEAD -> " + demo_.status.branch;
        c.edges = {{0, 0}};
        auto& v = demo_.status.changes;
        for (auto it = v.begin(); it != v.end();) {
            if (it->group != Area::Conflicts && (it->group == Area::Staged || !anyStaged)) {
                c.files.push_back({it->path, it->status == 'U' ? 'A' : it->status, it->status == 'D' ? 0 : 20,
                                   it->status == 'A' || it->status == 'U' ? 0 : 5});
                it = v.erase(it);
            } else
                ++it;
        }
        if (!demo_.log.empty())
            demo_.log.front().refs.clear();
        if (amend && !demo_.log.empty()) {
            demo_.log.front() = c;
            note("replaced the last commit with " + c.hash + " (\"" + subject + "\"), including the staged changes.");
        } else {
            demo_.log.insert(demo_.log.begin(), c);
            ++demo_.status.ahead;
            for (Branch& b : demo_.branches)
                if (b.current)
                    b.ahead = demo_.status.ahead;
            note("saved your staged changes as a snapshot (commit " + c.hash + ", \"" + subject +
                 "\"). It is now the newest entry in History, and the up-arrow counts it as one more commit to push.");
        }
        if (after == AfterCommit::Push)
            push();
        else if (after == AfterCommit::Sync)
            sync();
        return;
    }
    if (!anyStaged)
        run({"add", "-A"}, [this](const interfaces::ProcessResult& res) { failed(res, {"add"}); });
    std::vector<std::string> args{"commit", "-F", "-"};
    if (amend)
        args.push_back("--amend");
    act(args, std::string(amend ? "amended the last commit: \"" : "committed \"") + subject + "\"",
        [this, after]() {
            if (after == AfterCommit::Push)
                push();
            else if (after == AfterCommit::Sync)
                sync();
        },
        message);
}

void GitViewModel::commitWithMessage(AfterCommitWhat after)
{
    if (commitMessage.empty()) {
        note("write a message first: one line saying what you changed");
        return;
    }
    commit(commitMessage, amend, after);
    commitMessage.clear();
    amend = false;
}

std::vector<std::string> GitViewModel::unstagedPaths() const
{
    std::vector<std::string> paths;
    for (const Change& c : repo().status.changes)
        if ((c.group == Area::Changes || c.group == Area::Untracked) && std::find(paths.begin(), paths.end(), c.path) == paths.end())
            paths.push_back(c.path);
    return paths;
}

std::string GitViewModel::suggestMessage() const
{
    std::vector<std::string> names;
    bool staged = false;
    for (const Change& c : repo().status.changes)
        staged = staged || c.group == Area::Staged;
    for (const Change& c : repo().status.changes)
        if ((c.group == Area::Staged || !staged) && c.group != Area::Conflicts) {
            const std::string name = fileName(c.path);
            if (std::find(names.begin(), names.end(), name) == names.end())
                names.push_back(name);
        }
    if (names.empty())
        return {};
    std::string list = names[0];
    for (size_t i = 1; i < std::min<size_t>(names.size(), 3); ++i)
        list += (i + 1 == std::min<size_t>(names.size(), 3) && names.size() <= 3 ? " and " : ", ") + names[i];
    if (names.size() > 3)
        list += " and " + std::to_string(names.size() - 3) + " more";
    return "Update " + list;
}

void GitViewModel::writeMessage()
{
    if (demo()) {
        commitMessage = "Show git changes in the explorer\n\nAdds a Git tab with staged, changed and untracked files,\n"
                        "a commit box and the branch picker.";
        note("the agent wrote a message from the staged changes; edit it if you like");
        return;
    }
    if (!agent_ || !agent_->available()) {
        commitMessage = suggestMessage();
        note("no agent is connected, so this message only lists the files; say why you changed them");
        return;
    }
    const bool anyStaged = std::any_of(real_.status.changes.begin(), real_.status.changes.end(),
                                       [](const Change& c) { return c.group == Area::Staged; });
    writingMessage_ = true;
    run({"diff", anyStaged ? "--cached" : "HEAD", "--stat", "--patch"}, [this](const interfaces::ProcessResult& r) {
        std::string diff = r.output.substr(0, 24000);
        const std::string prompt =
            "Write a git commit message for this change: a summary line under 72 characters in the imperative mood, "
            "then, if useful, a blank line and a short body. Reply with the message only.\n\n" + diff;
        agent_->run(prompt, real_.root, [this](bool ok, const std::string& output) {
            writingMessage_ = false;
            if (!ok) {
                commitMessage = suggestMessage();
                note("the agent could not write a message: " + output.substr(0, 120));
                return;
            }
            std::string message = output;
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r' || message.back() == ' '))
                message.pop_back();
            commitMessage = message;
            note("the agent wrote a message from the changes; edit it if you like");
        });
    }, {}, true);
}

void GitViewModel::init()
{
    if (demo()) {
        demo_.repository = true;
        note("this folder is now a git repository");
        return;
    }
    run({"init"}, [this](const interfaces::ProcessResult& r) {
        if (failed(r, {"init"}))
            return;
        note("this folder is now a git repository. Nothing is uploaded anywhere.");
        workspaceChanged();
    });
}

void GitViewModel::updateSubmodule(const std::string& path)
{
    if (demo()) {
        note("updated the submodule " + path + " to the commit this repository records");
        return;
    }
    act({"submodule", "update", "--init", "--recursive", "--", path},
        "updated the submodule " + path + " to the commit this repository records");
}

void GitViewModel::showStatusText()
{
    if (demo()) {
        lastOutput_ = "On branch " + demo_.status.branch + "\n(demo data)\n";
        return;
    }
    run({"status"}, [this](const interfaces::ProcessResult& r) { lastOutput_ = r.output + r.errorOutput; }, {}, true);
}

void GitViewModel::openFile(const std::string& path)
{
    if (demo()) {
        note("opens " + path + " (a demo file: there is nothing on disk)");
        return;
    }
    owner_.openFile(workspacePath(path));
}

void GitViewModel::showChange(const std::string& path, Area area)
{
    // Old and new text of the change, shown side by side in the Diff tab.
    auto show = [this, path, area](std::string oldText, std::string newText, std::string sides) {
        diff_ = {};
        diff_.active = true;
        diff_.path = path;
        diff_.area = area;
        diff_.oldText = std::move(oldText);
        diff_.newText = std::move(newText);
        diff_.rows = model::diffLines(model::splitLines(diff_.oldText), model::splitLines(diff_.newText));
        diff_.title = path + ": " + sides;
        owner_.review().setDiff(diff_.title, diff_.rows);
        owner_.showPanelTab(PanelTab::Diff);
    };
    char status = 'M';
    std::string submodule;
    for (const Change& c : repo().status.changes)
        if (c.path == path && c.group == area) {
            status = c.status;
            submodule = c.submodule;
        }
    if (!submodule.empty() && !demo()) {
        // A submodule: the commits between the recorded one and its current one, and its own changes.
        std::vector<std::string> args{"diff", "--submodule=log"};
        if (area == Area::Staged)
            args.push_back("--cached");
        args.insert(args.end(), {"--", path});
        run(args, [this, path, area, submodule](const interfaces::ProcessResult& r) {
            std::string text = "Submodule " + path + ": " + submodule + "\n\n" + r.output;
            if (r.output.empty())
                text += "(no commit change; its own files changed: open the submodule's folder to see them)\n";
            diff_ = {};
            owner_.review().setDiff(path + ": submodule", model::diffLines({}, model::splitLines(text)));
            owner_.showPanelTab(PanelTab::Diff);
        }, {}, true);
        return;
    }
    if (demo()) {
        const std::string before = "int main(int argc, char** argv)\n{\n    enp::app::Application app(argc, argv);\n"
                                   "    if (!app.start(1280, 800))\n        return 1;\n    app.shutdown();\n    return app.run();\n}\n";
        const std::string after = "int main(int argc, char** argv)\n{\n    enp::app::Application app(argc, argv);\n"
                                  "    app.services().enableGit(true);\n    app.restoreSession();\n"
                                  "    if (!app.start(1440, 932))\n        return 1;\n    return app.run();\n}\n";
        const bool added = status == 'A' || status == 'U';
        show(added ? std::string() : before, status == 'D' ? std::string() : after,
             area == Area::Staged ? "HEAD vs staged" : added ? "new file" : "HEAD vs working tree");
        return;
    }
    // The working file: the open document's text (unsaved edits included) or the file on disk.
    const std::string working = status == 'D' ? std::string() : owner_.readWorkspaceFile(workspacePath(path));
    if (area == Area::Untracked) {
        show({}, working, "new file");
        return;
    }
    if (area == Area::Conflicts) {
        run({"show", ":2:" + path}, [this, path, show](const interfaces::ProcessResult& ours) {
            run({"show", ":3:" + path}, [ours, show](const interfaces::ProcessResult& theirs) {
                show(ours.exitCode == 0 ? ours.output : std::string(), theirs.exitCode == 0 ? theirs.output : std::string(),
                     "current vs incoming");
            }, {}, true);
        }, {}, true);
        return;
    }
    const bool staged = area == Area::Staged;
    run({"show", (staged ? "HEAD:" : ":") + path}, [this, path, staged, status, working, show](const interfaces::ProcessResult& old) {
        const std::string oldText = old.exitCode == 0 ? old.output : std::string();
        if (!staged) {
            show(oldText, working, "staged vs working tree");
            return;
        }
        run({"show", ":" + path}, [oldText, status, show](const interfaces::ProcessResult& index) {
            show(oldText, status == 'D' || index.exitCode != 0 ? std::string() : index.output, "HEAD vs staged");
        }, {}, true);
    }, {}, true);
}

// ---- Branches -----------------------------------------------------------------------------------

void GitViewModel::switchTo(const std::string& name)
{
    if (demo()) {
        for (Branch& b : demo_.branches) {
            if (b.kind == RefKind::Local)
                b.current = b.name == name;
            if (b.name == name && b.kind == RefKind::Local) {
                demo_.status.branch = name;
                demo_.status.ahead = b.ahead;
                demo_.status.behind = b.behind;
                demo_.status.upstream = b.upstream;
            }
        }
        note("switched to " + name + ": your files now match that branch");
        return;
    }
    const auto it = std::find_if(real_.branches.begin(), real_.branches.end(), [&](const Branch& b) { return b.name == name; });
    std::vector<std::string> args{"switch", name};
    if (it != real_.branches.end() && it->kind == RefKind::Remote) {
        const std::string local = name.substr(name.find('/') + 1);
        const bool exists = std::any_of(real_.branches.begin(), real_.branches.end(),
                                        [&](const Branch& b) { return b.kind == RefKind::Local && b.name == local; });
        args = exists ? std::vector<std::string>{"switch", local} : std::vector<std::string>{"switch", "--track", name};
    } else if (it != real_.branches.end() && it->kind == RefKind::Tag)
        args = {"switch", "--detach", name};
    run(args, [this, args, name](const interfaces::ProcessResult& r) {
        if (r.exitCode != 0 && (contains(r.errorOutput, "would be overwritten") || contains(r.errorOutput, "commit your changes or stash"))) {
            lastOutput_ = r.errorOutput;
            problem_ = {GitProblem::Kind::SwitchBlocked, name, "Your uncommitted changes would be overwritten by switching."};
        } else if (!failed(r, args))
            note("switched to " + args.back() + ": your files now match that branch");
        reloadOpenDocuments();
        refresh();
    });
}

void GitViewModel::stashAndSwitch(const std::string& name)
{
    problem_ = {};
    if (demo()) {
        stashPush("WIP before switching to " + name, {}, true, false);
        switchTo(name);
        return;
    }
    act({"stash", "push", "-u", "-m", "WIP before switching to " + name}, "your changes are in a stash", [this, name]() { switchTo(name); });
}

void GitViewModel::createBranch(const std::string& name, const std::string& from, bool switchTo)
{
    if (!model::git::validBranchName(name))
        return;
    if (demo()) {
        Branch b;
        b.name = name;
        b.kind = RefKind::Local;
        b.lastUsed = nowSeconds();
        demo_.branches.insert(demo_.branches.begin() + 1, b);
        if (switchTo)
            this->switchTo(name);
        note("created " + name + (switchTo ? " and switched to it" : ""));
        return;
    }
    std::vector<std::string> args = switchTo ? std::vector<std::string>{"switch", "-c", name} : std::vector<std::string>{"branch", name};
    if (!from.empty())
        args.push_back(from);
    act(args, "created " + name + (switchTo ? " and switched to it" : ""));
}

void GitViewModel::renameBranch(const std::string& from, const std::string& to)
{
    if (!model::git::validBranchName(to))
        return;
    if (demo()) {
        for (Branch& b : demo_.branches)
            if (b.kind == RefKind::Local && b.name == from)
                b.name = to;
        if (demo_.status.branch == from)
            demo_.status.branch = to;
        note("renamed " + from + " to " + to);
        return;
    }
    act({"branch", "-m", from, to}, "renamed " + from + " to " + to);
}

void GitViewModel::deleteBranch(const std::string& name, bool force)
{
    if (demo()) {
        auto& v = demo_.branches;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Branch& b) { return b.kind == RefKind::Local && b.name == name && !b.current; }), v.end());
        note("deleted the branch " + name);
        return;
    }
    const std::vector<std::string> args{"branch", force ? "-D" : "-d", name};
    run(args, [this, args, name](const interfaces::ProcessResult& r) {
        if (r.exitCode != 0 && contains(r.errorOutput, "not fully merged")) {
            lastOutput_ = r.errorOutput;
            problem_ = {GitProblem::Kind::BranchNotMerged, name, "It has commits that are on no other branch; deleting it loses them."};
        } else if (!failed(r, args))
            note("deleted the branch " + name);
        refresh();
    });
}

void GitViewModel::deleteRemoteBranch(const std::string& name)
{
    const size_t slash = name.find('/');
    if (slash == std::string::npos)
        return;
    if (demo()) {
        auto& v = demo_.branches;
        v.erase(std::remove_if(v.begin(), v.end(), [&](const Branch& b) { return b.kind == RefKind::Remote && b.name == name; }), v.end());
        note("deleted " + name + " on the server");
        return;
    }
    act({"push", name.substr(0, slash), "--delete", name.substr(slash + 1)}, "deleted " + name + " on the server");
}

void GitViewModel::deleteBranches(const std::vector<std::string>& names, bool onRemoteToo)
{
    for (const std::string& n : names) {
        std::string upstream;
        for (const Branch& b : repo().branches)
            if (b.kind == RefKind::Local && b.name == n && !b.gone)
                upstream = b.upstream;
        deleteBranch(n, false);
        if (onRemoteToo && !upstream.empty())
            deleteRemoteBranch(upstream);
    }
    note("deleted " + plural(names.size(), "branch", "branches") + (onRemoteToo ? " here and on the server" : ""));
}

void GitViewModel::merge(const std::string& name)
{
    if (demo()) {
        setDemoMerge(true);
        return;
    }
    run({"merge", "--no-edit", name}, [this, name](const interfaces::ProcessResult& r) {
        lastOutput_ = r.output + r.errorOutput;
        if (r.exitCode == 0)
            note("merged " + name + " into " + real_.status.branch);
        else if (contains(r.output, "CONFLICT"))
            note("merging " + name + ": some files changed on both sides and need you (see Conflicts)");
        else
            failed(r, {"merge"});
        reloadOpenDocuments();
        refresh();
    });
}

void GitViewModel::rebase(const std::string& onto)
{
    if (demo()) {
        note("rebased " + demo_.status.branch + " onto " + onto + ": your commits now come after " + onto + "'s");
        return;
    }
    run({"rebase", onto}, [this, onto](const interfaces::ProcessResult& r) {
        lastOutput_ = r.output + r.errorOutput;
        if (r.exitCode == 0)
            note("rebased onto " + onto);
        else if (contains(r.output + r.errorOutput, "CONFLICT"))
            note("rebasing onto " + onto + ": a commit changes lines that changed there too (see Conflicts)");
        else
            failed(r, {"rebase"});
        reloadOpenDocuments();
        refresh();
    });
}

namespace {
const char* operationCommand(GitOperation op)
{
    switch (op) {
    case GitOperation::Merge: return "merge";
    case GitOperation::Rebase: return "rebase";
    case GitOperation::CherryPick: return "cherry-pick";
    case GitOperation::Revert: return "revert";
    case GitOperation::None: break;
    }
    return "";
}
} // namespace

void GitViewModel::continueOperation()
{
    const GitOperation op = repo().operation;
    if (op == GitOperation::None)
        return;
    if (demo()) {
        setDemoMerge(false);
        note("the merge is finished and saved as a commit");
        return;
    }
    act({operationCommand(op), "--continue"}, std::string("finished the ") + operationCommand(op));
}

void GitViewModel::abortOperation()
{
    const GitOperation op = repo().operation;
    if (op == GitOperation::None)
        return;
    if (demo()) {
        demo_.operation = GitOperation::None;
        auto& v = demo_.status.changes;
        v.erase(std::remove_if(v.begin(), v.end(), [](const Change& c) { return c.path == "src/view/RibbonView.cpp"; }), v.end());
        note("the merge is cancelled: everything is as it was before");
        return;
    }
    act({operationCommand(op), "--abort"}, std::string("cancelled the ") + operationCommand(op) + ": everything is as it was before");
}

void GitViewModel::skipOperation()
{
    const GitOperation op = repo().operation;
    if (op != GitOperation::Rebase && op != GitOperation::CherryPick)
        return;
    if (!demo())
        act({operationCommand(op), "--skip"}, "skipped that commit");
}

// ---- Sync ---------------------------------------------------------------------------------------

void GitViewModel::fetch()
{
    if (demo()) {
        note("asked the server for news: nothing changed in your files");
        return;
    }
    act({"fetch", "--all", "--prune"}, "fetched from the server");
}

void GitViewModel::pull()
{
    if (demo()) {
        demo_.status.behind = 0;
        for (Branch& b : demo_.branches)
            if (b.current)
                b.behind = 0;
        note("pulled: you have everyone's latest commits on " + demo_.status.branch);
        return;
    }
    std::vector<std::string> args{"pull"};
    if (!pullRebase_)
        args.push_back("--no-rebase");
    act(args, "pulled the latest commits");
}

void GitViewModel::push(bool force)
{
    if (demo()) {
        demo_.status.ahead = 0;
        for (Branch& b : demo_.branches)
            if (b.current) {
                b.ahead = 0;
                if (b.upstream.empty())
                    b.upstream = demo_.status.upstream = "origin/" + b.name;
            }
        note("pushed " + demo_.status.branch + ": the server now has your commits");
        return;
    }
    std::vector<std::string> args{"push"};
    if (force)
        args.push_back("--force-with-lease");
    if (real_.status.upstream.empty() && !real_.status.detached)
        args.insert(args.end(), {"-u", defaultRemote_, real_.status.branch});
    act(args, real_.status.upstream.empty() ? "published " + real_.status.branch + " to " + defaultRemote_
                                            : "pushed " + real_.status.branch);
}

void GitViewModel::sync()
{
    if (demo()) {
        demo_.status.ahead = demo_.status.behind = 0;
        for (Branch& b : demo_.branches)
            if (b.current)
                b.ahead = b.behind = 0;
        note("pulled and pushed " + demo_.status.branch + ": it now matches the server");
        return;
    }
    std::vector<std::string> args{"pull"};
    if (!pullRebase_)
        args.push_back("--no-rebase");
    run(args, [this, args](const interfaces::ProcessResult& r) {
        if (failed(r, args)) {
            refresh();
            return;
        }
        push();
    });
}

void GitViewModel::runFailedInTerminal()
{
    std::string line = "git";
    for (const std::string& a : failedArgs_)
        line += a.find(' ') == std::string::npos ? " " + a : " \"" + a + "\"";
    problem_ = {};
    owner_.runInTerminal(line);
}

// ---- Remotes and clone --------------------------------------------------------------------------

void GitViewModel::loadRemotes()
{
    if (demo() || !real_.repository)
        return;
    run({"remote", "-v"}, [this](const interfaces::ProcessResult& r) {
        remotes_.clear();
        for (const std::string& line : model::git::lines(r.output)) {
            // "origin\thttps://... (fetch)"
            const size_t tab = line.find('\t');
            const size_t space = line.rfind(' ');
            if (tab == std::string::npos || space == std::string::npos || space < tab)
                continue;
            const std::string name = line.substr(0, tab), url = line.substr(tab + 1, space - tab - 1);
            auto it = std::find_if(remotes_.begin(), remotes_.end(), [&](const GitRemote& g) { return g.name == name; });
            if (it == remotes_.end()) {
                remotes_.push_back({name, {}, {}});
                it = remotes_.end() - 1;
            }
            (line.compare(space + 1, std::string::npos, "(push)") == 0 ? it->pushUrl : it->fetchUrl) = url;
        }
        if (!remotes_.empty() && std::none_of(remotes_.begin(), remotes_.end(), [](const GitRemote& g) { return g.name == "origin"; }))
            defaultRemote_ = remotes_.front().name;
    }, {}, true);
}

void GitViewModel::addRemote(const std::string& name, const std::string& url)
{
    if (name.empty() || url.empty())
        return;
    if (demo()) {
        demoRemotes_.push_back({name, url, url});
        note("added the remote " + name);
        return;
    }
    act({"remote", "add", name, url}, "added the remote " + name + ": Fetch to see its branches", [this]() { loadRemotes(); });
}

void GitViewModel::removeRemote(const std::string& name)
{
    if (demo()) {
        demoRemotes_.erase(std::remove_if(demoRemotes_.begin(), demoRemotes_.end(), [&](const GitRemote& g) { return g.name == name; }),
                           demoRemotes_.end());
        note("removed the remote " + name);
        return;
    }
    act({"remote", "remove", name}, "removed the remote " + name + " (nothing on the server changed)", [this]() { loadRemotes(); });
}

void GitViewModel::renameRemote(const std::string& from, const std::string& to)
{
    if (demo()) {
        for (GitRemote& g : demoRemotes_)
            if (g.name == from)
                g.name = to;
        note("renamed the remote " + from + " to " + to);
        return;
    }
    act({"remote", "rename", from, to}, "renamed the remote " + from + " to " + to, [this]() { loadRemotes(); });
}

void GitViewModel::setRemoteUrl(const std::string& name, const std::string& url)
{
    if (demo()) {
        for (GitRemote& g : demoRemotes_)
            if (g.name == name)
                g.fetchUrl = g.pushUrl = url;
        note(name + " now points to " + url);
        return;
    }
    act({"remote", "set-url", name, url}, name + " now points to " + url, [this]() { loadRemotes(); });
}

std::string GitViewModel::cloneName(const std::string& url)
{
    std::string s = url;
    while (!s.empty() && (s.back() == '/' || s.back() == '\\'))
        s.pop_back();
    const size_t cut = s.find_last_of("/:\\");
    if (cut != std::string::npos)
        s = s.substr(cut + 1);
    if (s.size() > 4 && s.compare(s.size() - 4, 4, ".git") == 0)
        s.resize(s.size() - 4);
    return s;
}

void GitViewModel::clone(const std::string& url, const std::string& parentFolder, const std::string& name)
{
    if (url.empty() || parentFolder.empty() || name.empty())
        return;
    if (demo() && learning_) {
        note("would clone " + url + " into " + parentFolder + "/" + name + " and open it (not while learning)");
        return;
    }
    if (!processes_) {
        note("cloning needs the git command line, which this platform cannot run");
        return;
    }
    const std::string target = parentFolder + "/" + name;
    note("cloning " + url + "...");
    Job job{{"clone", "--progress", url, name}, {}, {}, false, parentFolder};
    const std::vector<std::string> args = job.args;
    job.done = [this, args, target](const interfaces::ProcessResult& r) {
        if (failed(r, args))
            return;
        note("cloned into " + target);
        owner_.openFolder(target);
    };
    queue_.push_back(std::move(job));
    if (!running_)
        runNext();
}

char GitViewModel::statusOf(const std::string& workspacePath) const
{
    if (demo() || !real_.repository)
        return 0;
    const std::string path = repoPath(workspacePath);
    char status = 0;
    for (const Change& c : real_.status.changes)
        if (c.path == path) {
            if (c.group == Area::Conflicts)
                return '!';
            if (!status || c.group == Area::Changes)
                status = c.status;
        }
    return status;
}

bool GitViewModel::folderChanged(const std::string& workspaceFolder) const
{
    if (demo() || !real_.repository)
        return false;
    const std::string prefix = repoPath(workspaceFolder) + "/";
    return std::any_of(real_.status.changes.begin(), real_.status.changes.end(),
                       [&](const Change& c) { return c.path.rfind(prefix, 0) == 0; });
}

// ---- Stash --------------------------------------------------------------------------------------

void GitViewModel::stashPush(const std::string& message, const std::vector<std::string>& paths, bool untracked, bool keepIndex)
{
    const std::string name = message.empty() ? "WIP on " + repo().status.branch : message;
    if (demo()) {
        Stash s;
        s.message = name;
        s.when = "just now";
        s.branch = demo_.status.branch;
        auto& v = demo_.status.changes;
        for (auto it = v.begin(); it != v.end();) {
            const bool chosen = paths.empty() || std::find(paths.begin(), paths.end(), it->path) != paths.end();
            if (chosen && it->group != Area::Conflicts && (untracked || it->group != Area::Untracked) && !(keepIndex && it->group == Area::Staged)) {
                s.files.push_back({it->path, it->status, 12, 3});
                it = v.erase(it);
            } else
                ++it;
        }
        if (s.files.empty())
            return;
        demo_.stashes.insert(demo_.stashes.begin(), s);
        for (size_t i = 0; i < demo_.stashes.size(); ++i)
            demo_.stashes[i].ref = "stash@{" + std::to_string(i) + "}";
        note("put " + plural(s.files.size(), "file", "files") + " aside as \"" + name + "\". Pop it to bring them back.");
        return;
    }
    std::vector<std::string> args{"stash", "push", "-m", name};
    if (untracked)
        args.push_back("-u");
    if (keepIndex)
        args.push_back("--keep-index");
    if (!paths.empty()) {
        args.push_back("--");
        args.insert(args.end(), paths.begin(), paths.end());
    }
    act(args, "put your changes aside as \"" + name + "\"");
}

void GitViewModel::stashApply(int index, bool pop)
{
    const GitRepository& r = repo();
    if (index < 0 || index >= int(r.stashes.size()))
        return;
    if (demo()) {
        for (const FileChange& f : demo_.stashes[size_t(index)].files)
            if (!demoChange(f.path, Area::Changes) && !demoChange(f.path, Area::Untracked))
                demo_.status.changes.push_back({f.path, f.status == 'U' ? 'U' : f.status, f.status == 'U' ? Area::Untracked : Area::Changes, {}, {}});
        const std::string message = demo_.stashes[size_t(index)].message;
        if (pop) {
            demo_.stashes.erase(demo_.stashes.begin() + index);
            for (size_t i = 0; i < demo_.stashes.size(); ++i)
                demo_.stashes[i].ref = "stash@{" + std::to_string(i) + "}";
        }
        note(std::string(pop ? "popped" : "applied") + " \"" + message + "\": its changes are back under Changes");
        return;
    }
    act({"stash", pop ? "pop" : "apply", r.stashes[size_t(index)].ref},
        std::string(pop ? "popped" : "applied") + " \"" + r.stashes[size_t(index)].message + "\"");
}

void GitViewModel::stashDrop(int index)
{
    const GitRepository& r = repo();
    if (index < 0 || index >= int(r.stashes.size()))
        return;
    const std::string message = r.stashes[size_t(index)].message;
    if (demo()) {
        demo_.stashes.erase(demo_.stashes.begin() + index);
        note("dropped the stash \"" + message + "\"");
        return;
    }
    act({"stash", "drop", r.stashes[size_t(index)].ref}, "dropped the stash \"" + message + "\"");
}

void GitViewModel::stashBranch(int index, const std::string& branch)
{
    const GitRepository& r = repo();
    if (index < 0 || index >= int(r.stashes.size()) || !model::git::validBranchName(branch))
        return;
    if (demo()) {
        createBranch(branch, {}, true);
        stashApply(index, true);
        return;
    }
    act({"stash", "branch", branch, r.stashes[size_t(index)].ref}, "created " + branch + " with the stash's changes");
}

void GitViewModel::loadStashFiles(int index)
{
    if (demo() || index < 0 || index >= int(real_.stashes.size()) || !real_.stashes[size_t(index)].files.empty())
        return;
    const std::string ref = real_.stashes[size_t(index)].ref;
    run({"stash", "show", "--include-untracked", "--name-status", "-z", ref}, [this, ref](const interfaces::ProcessResult& names) {
        const bool withUntracked = names.exitCode == 0;
        std::vector<std::string> numArgs{"stash", "show", "--numstat", "-z", ref};
        std::vector<std::string> nameArgs{"stash", "show", "--name-status", "-z", ref};
        if (withUntracked)
            numArgs.insert(numArgs.begin() + 2, "--include-untracked");
        run(numArgs, [this, ref, withUntracked, names, nameArgs](const interfaces::ProcessResult& nums) {
            auto store = [this, ref](const std::string& nameText, const std::string& numText) {
                for (Stash& s : real_.stashes)
                    if (s.ref == ref)
                        s.files = model::git::parseFileChanges(nameText, numText);
            };
            if (withUntracked)
                store(names.output, nums.output);
            else // git before 2.32: no untracked files in `stash show`
                run(nameArgs, [store, nums](const interfaces::ProcessResult& n) { store(n.output, nums.output); }, {}, true);
        }, {}, true);
    }, {}, true);
}

// ---- History and compare ------------------------------------------------------------------------

void GitViewModel::loadLog(bool allBranches, const std::string& path, bool more)
{
    if (demo())
        return;
    if (!real_.repository || real_.status.initial) {
        real_.log.clear();
        real_.logLoaded = true;
        return;
    }
    logAll_ = allBranches;
    logPath_ = path;
    const int count = more ? int(real_.log.size()) + 300 : std::max<int>(300, int(real_.log.size()));
    std::vector<std::string> args{"log", std::string("--format=") + model::git::logFormat(), "--date-order", "-n", std::to_string(count)};
    args.push_back(allBranches ? "--all" : "HEAD");
    if (!path.empty())
        args.insert(args.end(), {"--follow", "--", path});
    run(args, [this, count](const interfaces::ProcessResult& r) {
        if (r.exitCode != 0)
            return;
        std::vector<Commit> commits = model::git::parseLog(r.output);
        // Keep files already loaded.
        for (Commit& c : commits)
            for (const Commit& old : real_.log)
                if (old.hash == c.hash)
                    c.files = old.files;
        real_.logComplete = int(commits.size()) < count;
        model::git::layoutGraph(commits);
        addTimes(commits);
        real_.log = std::move(commits);
        real_.logLoaded = true;
    }, {}, true);
}

void GitViewModel::loadCommitFiles(int index)
{
    if (demo() || index < 0 || index >= int(real_.log.size()) || !real_.log[size_t(index)].files.empty())
        return;
    const std::string hash = real_.log[size_t(index)].hash;
    const std::vector<std::string> base{"diff-tree", "-r", "--root", "--no-commit-id", "-m", "--first-parent", "-z"};
    std::vector<std::string> names = base, nums = base;
    names.insert(names.end(), {"--name-status", hash});
    nums.insert(nums.end(), {"--numstat", hash});
    run(names, [this, nums, hash](const interfaces::ProcessResult& n) {
        run(nums, [this, hash, n](const interfaces::ProcessResult& s) {
            for (Commit& c : real_.log)
                if (c.hash == hash)
                    c.files = model::git::parseFileChanges(n.output, s.output);
        }, {}, true);
    }, {}, true);
}

void GitViewModel::showCommitFile(const std::string& hash, const FileChange& file)
{
    auto show = [this, file, hash](const std::string& oldText, const std::string& newText) {
        diff_ = {};
        owner_.review().setDiff(file.path + " @ " + hash.substr(0, 7),
                                model::diffLines(model::splitLines(oldText), model::splitLines(newText)));
        owner_.showPanelTab(PanelTab::Diff);
    };
    if (demo()) {
        show("a\nb\nc\n", "a\nB\nc\nd\n");
        return;
    }
    const bool stash = hash.rfind("stash@", 0) == 0;
    const std::string before = stash ? hash + "^1" : hash + "^";
    run({"show", before + ":" + file.path}, [this, hash, file, show](const interfaces::ProcessResult& old) {
        run({"show", hash + ":" + file.path}, [old, file, show](const interfaces::ProcessResult& now) {
            show(old.exitCode == 0 && file.status != 'A' ? old.output : std::string(),
                 now.exitCode == 0 && file.status != 'D' ? now.output : std::string());
        }, {}, true);
    }, {}, true);
}

void GitViewModel::showFileBetween(const std::string& oldRef, const std::string& newRef, const FileChange& file)
{
    if (demo()) {
        showCommitFile(newRef, file);
        return;
    }
    run({"show", oldRef + ":" + file.path}, [this, oldRef, newRef, file](const interfaces::ProcessResult& old) {
        run({"show", newRef + ":" + file.path}, [this, old, oldRef, newRef, file](const interfaces::ProcessResult& now) {
            diff_ = {};
            owner_.review().setDiff(file.path + ": " + oldRef + " vs " + newRef,
                                    model::diffLines(model::splitLines(old.exitCode == 0 ? old.output : std::string()),
                                                     model::splitLines(now.exitCode == 0 ? now.output : std::string())));
            owner_.showPanelTab(PanelTab::Diff);
        }, {}, true);
    }, {}, true);
}

void GitViewModel::cherryPick(const std::vector<std::string>& hashes)
{
    if (hashes.empty())
        return;
    if (demo()) {
        demo_.status.ahead += int(hashes.size());
        note("copied " + plural(hashes.size(), "commit", "commits") + " onto " + demo_.status.branch +
             " (cherry-pick). They are new commits on your branch.");
        return;
    }
    std::vector<std::string> args{"cherry-pick"};
    args.insert(args.end(), hashes.begin(), hashes.end());
    run(args, [this, args, hashes](const interfaces::ProcessResult& r) {
        lastOutput_ = r.output + r.errorOutput;
        if (r.exitCode == 0)
            note("copied " + plural(hashes.size(), "commit", "commits") + " onto " + real_.status.branch);
        else if (contains(r.output + r.errorOutput, "CONFLICT") || contains(r.errorOutput, "could not apply"))
            note("cherry-pick stopped: a commit changes lines that changed here too (see Conflicts)");
        else
            failed(r, args);
        reloadOpenDocuments();
        refresh();
    });
}

void GitViewModel::revert(const std::string& hash)
{
    if (demo()) {
        note("undid " + hash + " with a new commit that reverses it; history keeps both");
        return;
    }
    act({"revert", "--no-edit", hash}, "undid " + hash.substr(0, 7) + " with a new commit that reverses it");
}

void GitViewModel::reset(const std::string& hash, ResetMode mode)
{
    const char* flag = mode == ResetMode::Soft ? "--soft" : mode == ResetMode::Mixed ? "--mixed" : "--hard";
    if (demo()) {
        note(std::string("moved ") + demo_.status.branch + " back to " + hash + (mode == ResetMode::Hard ? "; uncommitted changes were thrown away" : "; the later changes are kept as edits"));
        return;
    }
    act({"reset", flag, hash}, std::string("moved ") + real_.status.branch + " to " + hash.substr(0, 7));
}

void GitViewModel::tag(const std::string& name, const std::string& hash)
{
    if (name.empty())
        return;
    if (demo()) {
        Branch b;
        b.name = name;
        b.kind = RefKind::Tag;
        demo_.branches.push_back(b);
        note("tagged " + hash + " as " + name);
        return;
    }
    act({"tag", name, hash}, "tagged " + hash.substr(0, 7) + " as " + name);
}

void GitViewModel::checkoutDetached(const std::string& hash)
{
    if (demo()) {
        note("you are looking at " + hash + " (detached): switch back to a branch to keep working");
        return;
    }
    act({"switch", "--detach", hash}, "checked out " + hash.substr(0, 7) + " (detached: switch to a branch to keep working)");
}

void GitViewModel::compare(const std::string& base, const std::string& target)
{
    GitRepository& r = demo() ? demo_ : real_;
    if (r.compareBase == base && r.compareTarget == target && (demo() || !r.compareFiles.empty() || !r.compareCommits.empty()))
        return;
    r.compareBase = base;
    r.compareTarget = target;
    if (demo()) {
        demo_.compareCommits.clear();
        for (auto [hash, message, when] : {std::tuple{"7e7e7e7", "Clamp the tab strip scroll", "3 hours ago"},
                                           std::tuple{"6d6d6d6", "Keyboard navigation in the overflow list", "yesterday"},
                                           std::tuple{"5c5c5c5", "Keep the + button after the last tab", "2 days ago"}}) {
            Commit c;
            c.hash = c.shortHash = hash;
            c.message = message;
            c.when = when;
            c.author = "Sam Rivera";
            demo_.compareCommits.push_back(c);
        }
        demo_.compareFiles = {{"src/view/MainView.cpp", 'M', 64, 18}, {"src/view/DocumentListView.cpp", 'M', 22, 5},
                              {"tests/UiIdTest.cpp", 'M', 31, 0}, {"docs/tabs.md", 'A', 40, 0}};
        return;
    }
    real_.compareCommits.clear();
    real_.compareFiles.clear();
    if (base == target)
        return;
    const std::string range = base + "..." + target;
    run({"log", std::string("--format=") + model::git::logFormat(), "--cherry-pick", "--right-only", "--no-merges", "-n", "200", range},
        [this](const interfaces::ProcessResult& r) {
            if (r.exitCode != 0)
                return;
            std::vector<Commit> commits = model::git::parseLog(r.output);
            addTimes(commits);
            real_.compareCommits = std::move(commits);
        }, {}, true);
    run({"diff", "--name-status", "-z", range}, [this, range](const interfaces::ProcessResult& names) {
        run({"diff", "--numstat", "-z", range}, [this, names](const interfaces::ProcessResult& nums) {
            real_.compareFiles = model::git::parseFileChanges(names.output, nums.output);
        }, {}, true);
    }, {}, true);
}

void GitViewModel::takeFiles(const std::string& ref, const std::vector<std::string>& paths, bool stage)
{
    if (paths.empty())
        return;
    if (demo()) {
        for (const std::string& p : paths) {
            Change* c = demoChange(p, Area::Changes);
            if (!c && !demoChange(p, Area::Staged))
                demo_.status.changes.push_back({p, 'M', stage ? Area::Staged : Area::Changes, {}, {}});
            else if (c && stage)
                c->group = Area::Staged;
        }
        note("took " + plural(paths.size(), "file", "files") + " from " + ref + (stage ? " and staged them; commit when ready" : "; review them under Changes"));
        return;
    }
    std::vector<std::string> args{"restore", "--source=" + ref, "--worktree"};
    if (stage)
        args.push_back("--staged");
    args.push_back("--");
    args.insert(args.end(), paths.begin(), paths.end());
    act(args, "took " + plural(paths.size(), "file", "files") + " from " + ref + (stage ? " and staged them" : "; review them under Changes"));
}

void GitViewModel::restoreFile(const std::string& ref, const std::string& path)
{
    if (demo()) {
        takeFiles(ref, {path}, false);
        return;
    }
    const std::vector<std::string> args{"restore", "--source=" + ref, "--worktree", "--", path};
    run(args, [this, ref, path](const interfaces::ProcessResult& r) {
        if (r.exitCode != 0 && ref.rfind("stash@", 0) == 0) // an untracked file of a stash lives in its third parent
            act({"restore", "--source=" + ref + "^3", "--worktree", "--", path}, "restored " + fileName(path) + " from " + ref);
        else if (!failed(r, {"restore"})) {
            note("restored " + fileName(path) + " as it is in " + ref + "; review it under Changes");
            reloadOpenDocuments();
            refresh();
        }
    });
}

void GitViewModel::applyFileChange(const std::string& commit, const std::string& path)
{
    if (demo()) {
        takeFiles(commit, {path}, false);
        return;
    }
    const bool stash = commit.rfind("stash@", 0) == 0;
    const std::vector<std::string> diffArgs = stash ? std::vector<std::string>{"diff", commit + "^1", commit, "--", path}
                                                    : std::vector<std::string>{"show", "--format=", "--first-parent", commit, "--", path};
    run(diffArgs, [this, path, commit](const interfaces::ProcessResult& r) {
        if (r.exitCode != 0 || r.output.empty()) {
            failed(r, {"diff"});
            return;
        }
        act({"apply", "--3way", "-"}, "applied " + commit.substr(0, 7) + "'s change to " + fileName(path) + "; review it under Changes", {},
            r.output);
    }, {}, true);
}

void GitViewModel::openVersion(const std::string& ref, const std::string& path)
{
    if (demo()) {
        note("opens " + fileName(path) + " as it is in " + ref + " (read-only)");
        return;
    }
    run({"show", ref + ":" + path}, [this, ref, path](const interfaces::ProcessResult& r) {
        if (failed(r, {"show"}))
            return;
        owner_.openTextDocument(fileName(path) + " @ " + (ref.size() > 12 ? ref.substr(0, 7) : ref), path, r.output);
    }, {}, true);
}

void GitViewModel::compareWithWorkingTree(const std::string& ref, const std::string& path)
{
    if (demo()) {
        showChange(path, Area::Changes);
        return;
    }
    const std::string working = owner_.readWorkspaceFile(workspacePath(path));
    run({"show", ref + ":" + path}, [this, ref, path, working](const interfaces::ProcessResult& r) {
        diff_ = {};
        owner_.review().setDiff(path + ": " + ref.substr(0, std::min<size_t>(ref.size(), 20)) + " vs your copy",
                                model::diffLines(model::splitLines(r.exitCode == 0 ? r.output : std::string()), model::splitLines(working)));
        owner_.showPanelTab(PanelTab::Diff);
    }, {}, true);
}

// ---- Line level ---------------------------------------------------------------------------------

void GitViewModel::applyRows(const std::vector<bool>& chosen, RowsAction action)
{
    if (!diff_.active || std::none_of(chosen.begin(), chosen.end(), [](bool b) { return b; }))
        return;
    const char* what = action == RowsAction::Stage ? "staged" : action == RowsAction::Unstage ? "unstaged" : "discarded";
    const std::string path = diff_.path;
    const Area area = diff_.area;
    if (demo()) {
        note(std::string(what) + " the chosen lines of " + fileName(path));
        return;
    }
    // Stage: the diff is index -> working file; its chosen rows go into the index.
    // Unstage: the diff is HEAD -> index; the chosen rows are taken back out of the index.
    // Discard: the diff is index -> working file; the chosen rows are taken out of the file.
    // Taking rows back out = the reversed diff (new -> old) with only those rows, applied to new.
    std::string patch;
    if (action == RowsAction::Stage)
        patch = model::git::buildPatch(path, diff_.oldText, diff_.rows, chosen);
    else {
        std::vector<model::DiffRow> reversed = diff_.rows;
        for (model::DiffRow& r : reversed) {
            std::swap(r.leftLine, r.rightLine);
            std::swap(r.leftText, r.rightText);
            if (r.kind == model::DiffKind::Added)
                r.kind = model::DiffKind::Removed;
            else if (r.kind == model::DiffKind::Removed)
                r.kind = model::DiffKind::Added;
        }
        patch = model::git::buildPatch(path, diff_.newText, reversed, chosen);
    }
    std::vector<std::string> args{"apply", "--whitespace=nowarn"};
    if (action != RowsAction::Discard)
        args.push_back("--cached");
    args.push_back("-");
    act(args, std::string(what) + " the chosen lines of " + fileName(path), [this, path, area]() { showChange(path, area); }, patch);
}

const std::vector<LineChange>& GitViewModel::lineChanges(const std::string& workspacePath, long long version,
                                                       const std::function<std::string()>& textOf)
{
    static const std::vector<LineChange> none;
    if (demo() || !real_.repository || real_.status.initial)
        return none;
    auto it = std::find_if(lineCache_.begin(), lineCache_.end(), [&](const LineCache& c) { return c.path == workspacePath; });
    if (it == lineCache_.end()) {
        lineCache_.push_back({workspacePath, {}, -1, {}, false, {}, {}});
        if (lineCache_.size() > 16)
            lineCache_.erase(lineCache_.begin());
        it = lineCache_.end() - 1;
    }
    LineCache& cache = *it;
    if (cache.head != real_.status.head) {
        // HEAD moved (commit, switch): fetch the committed text again.
        cache.head = real_.status.head;
        cache.headLoaded = false;
        cache.version = -1;
        const std::string path = repoPath(workspacePath);
        run({"show", "HEAD:" + path}, [this, workspacePath](const interfaces::ProcessResult& r) {
            for (LineCache& c : lineCache_)
                if (c.path == workspacePath) {
                    c.headText = r.exitCode == 0 ? r.output : std::string();
                    c.headLoaded = r.exitCode == 0;
                    c.version = -1;
                }
        }, {}, true);
    }
    if (!cache.headLoaded || cache.version == version)
        return cache.changes;
    const std::string text = textOf();
    cache.version = version;
    if (text.size() > 2 * 1024 * 1024) { // large files: no markers
        cache.changes.clear();
        cache.rows.clear();
        return cache.changes;
    }
    const std::vector<std::string> now = model::splitLines(text);
    cache.rows = model::diffLines(model::splitLines(cache.headText), now);
    const std::vector<model::DiffRow>& rows = cache.rows;
    cache.changes.assign(now.size(), LineChange::None);
    for (const model::DiffRow& r : rows) {
        if (r.rightLine >= 0 && r.rightLine < int(now.size())) {
            if (r.kind == model::DiffKind::Added)
                cache.changes[size_t(r.rightLine)] = LineChange::Added;
            else if (r.kind == model::DiffKind::Changed)
                cache.changes[size_t(r.rightLine)] = LineChange::Modified;
        } else if (r.kind == model::DiffKind::Removed) {
            // Mark the line after the gap.
            int next = 0;
            for (const model::DiffRow& s : rows)
                if (&s > &r && s.rightLine >= 0) {
                    next = s.rightLine;
                    break;
                }
            if (next < int(now.size()) && cache.changes[size_t(next)] == LineChange::None)
                cache.changes[size_t(next)] = LineChange::DeletedAbove;
        }
    }
    return cache.changes;
}

bool GitViewModel::changeAt(const std::string& workspacePath, int line, int& first, int& last, std::vector<std::string>& headLines) const
{
    for (const LineCache& c : lineCache_) {
        if (c.path != workspacePath || c.rows.empty())
            continue;
        // The run of changed rows that touches `line` (or, for a deletion, sits just above it).
        const std::vector<model::DiffRow>& rows = c.rows;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (rows[i].kind == model::DiffKind::Same)
                continue;
            size_t j = i;
            while (j + 1 < rows.size() && rows[j + 1].kind != model::DiffKind::Same)
                ++j;
            int lo = -1, hi = -1;
            headLines.clear();
            for (size_t k = i; k <= j; ++k) {
                if (rows[k].rightLine >= 0) {
                    lo = lo < 0 ? rows[k].rightLine : std::min(lo, rows[k].rightLine);
                    hi = std::max(hi, rows[k].rightLine);
                }
                if (rows[k].leftLine >= 0)
                    headLines.push_back(rows[k].leftText);
            }
            if (lo < 0) { // only deletions: they sat above the next line
                int next = 0;
                for (size_t k = j + 1; k < rows.size(); ++k)
                    if (rows[k].rightLine >= 0) {
                        next = rows[k].rightLine;
                        break;
                    }
                lo = next;
                hi = next - 1;
            }
            if (line >= lo && line <= std::max(lo, hi)) {
                first = lo;
                last = hi;
                return true;
            }
            i = j;
        }
    }
    return false;
}

void GitViewModel::documentSaved(const std::string& workspacePath, const std::string& text)
{
    requestRefresh();
    if (demo())
        return;
    const std::string path = repoPath(workspacePath);
    const bool conflicted = std::any_of(real_.status.changes.begin(), real_.status.changes.end(),
                                        [&](const Change& c) { return c.path == path && c.group == Area::Conflicts; });
    if (conflicted && model::git::findConflicts(model::git::lines(text)).empty()) {
        stage({path});
        note(fileName(path) + " has no conflict markers left, so it is marked resolved (staged)");
    }
}

const std::vector<model::git::BlameLine>& GitViewModel::blame(const std::string& workspacePath) const
{
    static const std::vector<model::git::BlameLine> none;
    for (const auto& [path, lines] : blames_)
        if (path == workspacePath)
            return lines;
    return none;
}

bool GitViewModel::blameShown(const std::string& workspacePath) const
{
    return std::find(blameShown_.begin(), blameShown_.end(), workspacePath) != blameShown_.end();
}

void GitViewModel::toggleBlame(const std::string& workspacePath, const std::string& text)
{
    const auto it = std::find(blameShown_.begin(), blameShown_.end(), workspacePath);
    if (it != blameShown_.end()) {
        blameShown_.erase(it);
        return;
    }
    if (demo()) {
        note("blame shows who last changed each line and when (open a file of a real repository to see it)");
        return;
    }
    blameShown_.push_back(workspacePath);
    loadBlame(workspacePath, text);
}

void GitViewModel::loadBlame(const std::string& workspacePath, const std::string& text)
{
    if (demo() || !real_.repository)
        return;
    run({"blame", "--porcelain", "--contents", "-", "--", repoPath(workspacePath)}, [this, workspacePath](const interfaces::ProcessResult& r) {
        if (failed(r, {"blame"}))
            return;
        std::vector<model::git::BlameLine> lines = model::git::parseBlame(r.output);
        for (auto& entry : blames_)
            if (entry.first == workspacePath) {
                entry.second = std::move(lines);
                return;
            }
        blames_.emplace_back(workspacePath, std::move(lines));
    }, text, true);
}

} // namespace enp::viewmodel
