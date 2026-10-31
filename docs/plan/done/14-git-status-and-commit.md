# 14 · Git: status, diff and commit

**Problem.** There is no source control in the editor. Seeing what changed, staging and committing
all need the terminal; the only git pieces are two sample custom actions (`git status`, `git log`).

This is the first of four git items (14–17). It builds the foundation the others use.

## Goal

A person who never used git from a GUI can see what changed, review it, and commit, without
leaving the editor. Everything is also reachable from the keyboard, the command palette and the
connector, so agents can use it too.

```
┌ Explorer ───────────────────────┐
│ Files │ Open │ Git ●3           │   ← third explorer tab, count of changes
├─────────────────────────────────┤
│ ⎇ main  ↑2 ↓0        [⟳] [⋯]   │   ← branch chip (item 15), refresh, more menu
│ ┌─────────────────────────────┐ │
│ │ Message (Ctrl+Enter)        │ │
│ └─────────────────────────────┘ │
│ [✓ Commit]  ☐ Amend             │
│ ▾ Staged (1)                    │
│   M  src/app/Main.cpp      [−]  │   ← hover: unstage
│ ▾ Changes (2)                   │
│   M  README.md       [↶][+]     │   ← hover: discard, stage
│   D  old/notes.txt   [↶][+]     │
│ ▾ Untracked (1)                 │
│   U  docs/plan/14.md [🗑][+]    │
└─────────────────────────────────┘
```

**Preview.** The interface for items 14–18 is already in the app with sample data
(`view/GitView`). Each item replaces the sample data with real git and keeps the look.

## Design

**Running git (shared by 14–18).** We use the `git` command-line tool, not a library. It is
already installed wherever developers work, and it uses their config, credentials and hooks.
- `GitViewModel` sends commands through `IProcessService` one at a time from a queue. Each command
  is an argument list.
- Quoting arguments for the shell is done in one function and unit-tested. Commit messages go
  through a temp file with `git commit -F <file>`, so they are never quoted.
- Every command runs with:
  - `GIT_TERMINAL_PROMPT=0`, so git never waits for input that has nowhere to go.
  - `GIT_OPTIONAL_LOCKS=0` for the background refresh, so it never fights the user's own git in the
    terminal.
  - `-c core.quotepath=off`, so non-ASCII paths come back readable.
- The busy state and the last error show in the Git tab. The full command output is one click away
  ("Show output").

**Model (pure, unit-tested).** `model/Git.h`:
- `parseStatus(porcelain v2 -z)` gives the branch, upstream, ahead/behind and a list of entries.
  Each entry has a path, its original path for renames, the index state, the work-tree state and a
  conflict flag.
- `parseLog`, `parseBranches` and `parseStash` come in later items.
- `quoteArg(arg)` is the shell quoting above.

**Repository detection.**
- `git rev-parse --show-toplevel` runs when a folder opens. The workspace may be a subfolder of
  the repository, so paths are converted between repository-relative and workspace-relative.
- If git is not found, the Git tab says so and links the setting for the git path.
- If the folder is not a repository, the tab offers **Initialize repository**.

**Refresh.** `git status --porcelain=v2 --branch -z` runs:
- after any git action;
- after a save;
- when the window regains focus;
- when a terminal command finishes;
- on the existing 2 s disk poll, but only while the Git tab or the branch chip is visible.

Refreshes are read-only and debounced, and only one runs at a time.

**Git tab (Explorer).**
- **Groups:** Staged, Changes, Untracked, and Conflicts (item 16). Each group stages or unstages
  everything in it from its header.
- **Rows:** the status letter is coloured with new theme tokens `gitAdded`, `gitModified`,
  `gitDeleted`, `gitUntracked` and `gitConflict`.
  - Clicking a row opens the Diff tab: HEAD against the work tree, or HEAD against the index for
    staged rows. It reuses `diffLines` and `renderSideBySideDiff`.
  - Double-click opens the file.
  - Hover buttons stage, unstage, or discard.
- **Commit:**
  - The commit box sits above the list; Ctrl+Enter commits.
  - Commit with nothing staged asks "Stage all changes and commit?" (Yes / Cancel).
  - Amend loads the last message into the box.
  - A failing hook shows its output in the error area.

**Selecting several files.**
- Ctrl-click adds or removes a row; Shift-click selects a range within one group. A plain click
  selects one row and shows its diff.
- With two or more rows selected, a bar above the lists shows "N selected" and icon buttons for
  Stage, Unstage, Stash (item 17's dialog, set to "only the selected files"), Discard and Clear.
  Esc clears the selection.
- A right-click on a selected row acts on the whole selection ("Stage 3 files", "Discard changes
  to 3 files...").
- A row's menu also has **Take this file from ▸ (branch or tag)**, which replaces the file with
  that version (item 18).

**Everywhere else.**
- The Files tree marks changed files with the same letters and colours, and folders that contain
  changes get a dot.
- Tabs of changed files show the letter too.
- A file's right-click menu (shared, from item 11) gains Stage, Unstage, Discard changes, Open diff,
  Copy relative path, and Reveal in Git tab.

**Safety.** Anything that throws work away asks first through the existing prompt, and the prompt
names what will be lost (for example "Discard changes to 2 files? This cannot be undone."). That
covers discard and deleting untracked files. Nothing runs without a user (or agent) action, except
read-only status.

**Commands and connector.**
- New commands: `GitRefresh`, `GitStage`, `GitUnstage`, `GitStageAll`, `GitDiscard`, `GitCommit`,
  `GitCommitAmend`, `GitInit`, `ShowGit`. They are available in the ribbon (new **Git** group on the
  Home tab, customizable like the others), the palette and the shortcuts.
- Connector tools:
  - `git_status`;
  - `git_diff` (path, staged);
  - `git_stage` / `git_unstage`;
  - `git_commit` (message).
- Discard is a command, so it goes through the same prompt for agents.
- The two sample custom actions are removed.

**Settings.** `git.path` (empty means find `git` on PATH) and `git.showDecorations`.

## Tests

**Unit:**
- `parseStatus` on recorded outputs: ordinary, renamed, staged + modified, untracked, conflicted,
  detached HEAD, no upstream, and non-ASCII paths.
- `quoteArg` with spaces, quotes, `$`, `%`, `!` and newlines, on both shells.
- Path mapping when the workspace is a subfolder.

**Integration (temp repository; skipped with a note if git is missing):**
1. Init, then create a file: it shows as Untracked.
2. Stage, then commit: the list is clean and `git log` has the message, including one with quotes
   and newlines.
3. Edit: it shows as Changes, and the diff is right.
4. Discard asks first, then restores the file.
5. Amend changes the message.
6. A pre-commit hook that fails leaves the changes staged and shows its output.
7. Select three rows with Ctrl and Shift, then Stage: exactly those three are staged.

**UI:**
- The sweep covers the Git tab in the empty, clean, changed and error states.
- Screenshots of a repository with changes.
