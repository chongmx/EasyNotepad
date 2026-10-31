// Git as data: what the git command line prints, parsed into plain structs (status, branches,
// commits, stashes, changed files, blame), the commit graph's lanes, merge-conflict blocks, and the
// patch that stages part of a file. Pure: no processes, no I/O.
#pragma once

#include "enp/model/Diff.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enp::model::git {

// Where a changed file is shown in the Git tab.
enum class Area { Staged, Changes, Untracked, Conflicts };

struct Change {
    std::string path;     // repository-relative, '/' separated
    char status = 'M';    // M A D R U (untracked) ! (conflict)
    Area group = Area::Changes;
    std::string origPath; // renames: where it came from
};

struct Status {
    bool initial = false;  // no commit yet
    bool detached = false;
    std::string branch;    // or the short hash when detached
    std::string head;      // commit id
    std::string upstream;  // empty: none
    int ahead = 0, behind = 0;
    std::vector<Change> changes;
};
// `git status --porcelain=v2 --branch -z --untracked-files=all`
Status parseStatus(std::string_view text);

enum class RefKind { Local, Remote, Tag };
struct Branch {
    std::string name;      // short name: main, origin/main, v1.0
    RefKind kind = RefKind::Local;
    std::string upstream;
    int ahead = 0, behind = 0;
    bool merged = false;   // into the current branch
    bool gone = false;     // upstream deleted on the remote
    bool current = false;
    int64_t lastUsed = 0;  // committer time of its tip (unix seconds)
};
// The format string for `git for-each-ref --format=...` that parseBranches() reads.
const char* branchFormat();
std::vector<Branch> parseBranches(std::string_view text);

struct FileChange {
    std::string path;
    char status = 'M';     // M A D R
    int added = 0, removed = 0;
};
// `--name-status -z` and `--numstat -z` output of the same diff, merged by path.
std::vector<FileChange> parseFileChanges(std::string_view nameStatus, std::string_view numstat);

struct Commit {
    std::string hash;      // full
    std::string shortHash;
    std::vector<std::string> parents;
    std::string message;   // subject line
    std::string author;
    int64_t time = 0;
    std::string when;      // "2 hours ago" (filled by the caller)
    std::string refs;      // "HEAD -> main, origin/main, tag: v1.0"
    int lane = 0;          // graph column
    std::vector<std::pair<int, int>> edges; // lane here -> lane on the next row
    std::vector<FileChange> files;          // loaded on demand
};
// The format string for `git log --format=...` that parseLog() reads.
const char* logFormat();
std::vector<Commit> parseLog(std::string_view text);
// Assigns lanes and the edges between consecutive rows, newest commit first.
void layoutGraph(std::vector<Commit>& commits, int maxLanes = 8);

struct Stash {
    std::string ref;       // stash@{0}
    std::string message;
    std::string branch;    // where it was made
    int64_t time = 0;
    std::string when;
    std::vector<FileChange> files;
};
// `git stash list --format=%gd%x1f%gs%x1f%ct`
std::vector<Stash> parseStashList(std::string_view text);

// `git blame --porcelain`: per line of the file, the commit that last changed it.
struct BlameLine {
    std::string hash;      // all zeros: not committed yet
    std::string author;
    int64_t time = 0;
    std::string summary;
};
std::vector<BlameLine> parseBlame(std::string_view text);

// "just now", "5 minutes ago", "yesterday", "3 weeks ago", ...
std::string relativeTime(int64_t time, int64_t now);
// The rules of `git check-ref-format --branch` that matter when typing a name.
bool validBranchName(std::string_view name);
// Lines of `text`, separated on '\n' (a '\r' before it is dropped).
std::vector<std::string> lines(std::string_view text, char separator = '\n');

// ---- Merge conflicts ------------------------------------------------------------------------------

struct ConflictBlock {
    int start = 0;       // the <<<<<<< line
    int base = -1;       // the ||||||| line (diff3 style), or -1
    int middle = 0;      // the ======= line
    int end = 0;         // the >>>>>>> line
    std::string current; // label after <<<<<<<
    std::string incoming;
};
std::vector<ConflictBlock> findConflicts(const std::vector<std::string>& lines);
enum class Resolution { Current, Incoming, Both };
// The block's lines replaced by the chosen side(s).
std::vector<std::string> resolveConflict(const std::vector<std::string>& blockLines, const ConflictBlock& block,
                                         Resolution resolution);

// ---- Partial staging ------------------------------------------------------------------------------

// A unified patch for `path` that turns `oldText` into a text with only the chosen changes of
// `rows` (a diffLines(old, new) result) applied. Rows not chosen stay as in `oldText`.
// Apply with `git apply --cached` to stage, or `--reverse` against the new text to discard.
std::string buildPatch(const std::string& path, const std::string& oldText, const std::vector<DiffRow>& rows,
                       const std::vector<bool>& chosen);
// The row range [first, last] of the change ("hunk") around row `row`.
std::pair<int, int> hunkAt(const std::vector<DiffRow>& rows, int row);

} // namespace enp::model::git
