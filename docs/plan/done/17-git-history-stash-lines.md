# 17 · Git: history, stash and line-level changes

**Problem.** After items 14–16 the everyday loop works. Three things are still missing:
- looking back (log, blame, undoing a commit);
- parking work (stash);
- the finer control people expect from a full git client: changed-line markers, and staging only
  some lines.

## Design

**History tab.** It is in the bottom panel, next to Problems, Diff, Terminal and Search. The
`PanelTab` value is appended, since those values are saved as numbers.

```
┌ History ── branch: main ▾ ── 🔍 message, author, hash… ── ☐ this file only ┐
│ ●─  a1b2c3d  Add git status parser          Mei     2h   (HEAD, main)      │
│ │●  9f8e7d6  Merge feature/x                 Sam     1d                     │
│ ●┘  4455aa1  Fix ribbon overflow             Mei     2d   (origin/main)     │
├─────────────────────────────────────────────────────────────────────────────┤
│ a1b2c3d · Mei <mei@…> · 2026-10-31 14:02                                    │
│ Add git status parser                                                       │
│   M include/enp/model/Git.h   +42 −0                                        │
│   M src/model/Git.cpp        +180 −3      ← click: diff of this commit      │
└─────────────────────────────────────────────────────────────────────────────┘
```

- **Loading.** `git log --format=<fields with separators> -z --parents`, loaded in pages of 200 as
  you scroll and parsed by `parseLog`.
- **Graph.** The lanes are computed from the parent links by `model::layoutGraph`, which is pure and
  unit-tested. The column stays narrow: the lanes are capped, and anything past the cap is drawn as
  "…".
- **Filters:** branch or "all", and text (message, author or hash). "This file only" uses
  `--follow -- <path>`, and the editor's right-click menu opens it as **File history**.
- **Selecting a commit** shows its details and changed files (`git show --numstat`), with
  Checkout, Cherry-pick and Revert buttons. A file click opens the diff of that commit for that
  file, and a right-click gives item 18's per-file actions.
- The History tab has two modes, **Log** (this) and **Compare** (item 18).
- **Right-click on a commit:**
  - Checkout (detached; the prompt explains what that means);
  - Create branch here;
  - Cherry-pick;
  - Revert;
  - Reset current branch to here: soft, mixed or hard. Hard asks first and names the uncommitted
    changes it would lose.
  - Create tag;
  - Copy hash;
  - Compare with working tree.
- **Item 15's "Compare with current"** shows `A..B` and `B..A` here.

**Blame.**
- **Toggle Blame** (editor right-click menu, and a command) shows a narrow left column on each line:
  the short hash, author and age. Lines from the same commit are grouped.
- Hovering shows the full message, and clicking selects that commit in History.
- The data comes from `git blame --porcelain`, parsed and cached per document version.
- The column hides while the buffer has unsaved edits past the blamed version, or it shows "not
  committed" for those lines.

**Stash.**
- **Stash...** (ribbon, the Git tab's "⋯" menu, the icon on the Stashes header, the selection bar)
  opens a dialog:
  - a name (optional; the default is "WIP on <branch>");
  - **All changes (N files)** or **Only the selected files (M)**, the latter chosen when it was
    opened from a selection (`git stash push -m <name> -- <paths>`);
  - ☑ Include untracked files (`-u`), ☐ Keep staged changes in place (`--keep-index`);
  - the list of files that will be stashed, so nothing is a surprise.
- **Stashes** group at the bottom of the Git tab (`git stash list`, parsed by `parseStash`):
  - Each stash shows its name, when and on which branch it was made, and opens to list its files
    (`git stash show --name-status`).
  - Each file has item 18's per-file actions, so one file can be taken out of a stash
    (`git restore --source=stash@{n} -- <path>`) without applying the rest.
  - Right-click a stash: Apply (keep it), Pop (apply and drop), Apply on a new branch
    (`git stash branch`), View all changes, Rename, Drop (asks first).
- **Stash and switch** from item 15 uses the same code.
- A pop that conflicts uses item 16's flow, and the stash is kept until it is resolved.

**Changed-line markers.**
- The editor gutter shows added, modified and deleted lines against HEAD, like the existing change
  bar but coloured by git state.
- The HEAD version comes from `git show HEAD:<path>` once per file and HEAD change. The live diff
  uses `diffLines` against the buffer, debounced, and only for files under a size limit.
- Clicking a marker opens a small inline peek with the old text, plus **Revert this change** and
  **Stage this change**.

**Partial staging.**
- In the Diff tab, each hunk gets **Stage hunk / Unstage hunk / Discard hunk**, and selecting lines
  gives **Stage selected lines**.
- `model::buildPatch(diff, selection)` produces a minimal unified patch that is applied with
  `git apply --cached` (or `--reverse`). The patch builder is the risky part, so it gets the most
  unit tests.

**Commands and connector.**
- Commands: `ShowHistory`, `FileHistory`, `ToggleBlame`, `GitStash`, `GitStashPop`, `GitRevert`,
  `GitCherryPick`, `GitStageHunk`.
- Connector: `git_log` (path, limit), `git_show` (commit), `git_blame` (path, lines),
  `git_stash` (push/list/apply/pop).
- Reset hard and Drop are commands, never connector tools.

## Tests

**Unit:**
- `parseLog` (merges, root commit, unusual author names);
- `layoutGraph` (linear, branch-and-merge, octopus, lane cap);
- blame parsing;
- `parseStash`;
- `buildPatch` round-trips: applying the patch to the base gives the expected text for single
  hunks, partial lines, end of file with and without a final newline, and CRLF.

**Integration (temp repository):**
- History paging;
- a file history across a rename;
- revert and cherry-pick commits;
- each reset mode;
- stash with untracked files, then pop;
- stash only two selected files: the others stay in the working tree;
- take one file out of a stash: only that file changes, and the stash is kept;
- stage one of two hunks, after which `git diff --cached` shows only that hunk;
- blame after an edit.

**UI:**
- The sweep with a 1,000-commit repository: History scrolls without stalls, measured like item
  06's highlight cost.
- Screenshots of history, blame and gutter markers.
