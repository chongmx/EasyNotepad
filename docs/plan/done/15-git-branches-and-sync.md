# 15 · Git: branches and sync

**Problem.** After item 14 you can commit, but switching, creating or merging branches and
pulling or pushing still need the terminal.

## Goal

The current branch is always visible, and switching takes two clicks. Every branch action is in
one place. Sync is one button that does the safe thing.

```
Status bar:  … │ ⎇ feature/git ↑2 ↓1 │ UTF-8 │ LF │ C++
                     │ click
                     ▼
┌ Branches ─────────────────────────────────┐
│ 🔍 filter…                                │
│ [+ New branch…]  [⇅ Sync]  [↓ Fetch]      │
│ ▾ Local                                   │
│   ● feature/git        ↑2 ↓1  origin/…    │   ← current
│     main               ✓      origin/main │
│     spike/old          (no upstream)      │
│ ▾ Remote (origin)                         │
│     origin/release-1.2                    │
│ ▾ Tags                                    │
│     v0.3.0                                │
└───────────────────────────────────────────┘
   row click = switch · right-click = Rename, Delete, Merge into current,
   Rebase current onto, Push, Set upstream, Compare with current, Copy name
```

## Design

**Branch chip.** The status bar and the top of the Git tab show the branch, ahead/behind and a
dirty dot. A detached HEAD shows the short hash. In the middle of a merge or rebase, the chip shows
"MERGING" or "REBASING" (item 16).

**Branch picker.**
- A popup with a filter, like the language picker from item 10, and keyboard navigation.
- The data comes from:
  - `git for-each-ref` over refs/heads, refs/remotes and refs/tags, with upstream and track info;
  - parsed by `parseBranches`, which is unit-tested.
- **Switch.**
  - Uses `git switch`. Selecting a remote branch with no local copy creates a tracking branch.
  - If local changes block the switch, the prompt offers **Stash and switch** (item 17), **Bring
    changes along** (works when git allows it), or Cancel. It never forces.
  - After the switch, open documents reload through the existing disk-change path (item 12):
    unmodified documents reload, and edited ones show the conflict bar.
- **New branch.** An inline name box, checked with `git check-ref-format --branch` as you type. It
  starts from the current branch, or from the commit/branch it was opened on. "Switch to it" is
  ticked by default.
- **Rename** works on local branches.
- **Delete** uses `-d`. If git refuses because the branch is not merged, a second prompt names the
  commits that would be lost and offers **Delete anyway** (`-D`). Deleting a remote branch is a
  separate, clearly-labelled action that asks first.
- **Merge into current** and **Rebase current onto** show conflicts in item 16's flow.
- **Compare with current** opens the Compare view (item 18) with that branch: commits to
  cherry-pick and files to take.

**Keeping the list tidy.**
- Local branches are sorted by most recent use (from the reflog), with the current branch first.
  A "Sort: recent / name" toggle switches to alphabetical.
- Badges after the name: **merged** (`git branch --merged`: its commits are all in the current
  branch) and **remote gone** (its upstream was deleted; `[gone]` in `for-each-ref`).
- **Clean up...** (in the picker and the Git tab's "⋯" menu) opens a dialog listing only branches
  that are merged or whose remote is gone, each with a checkbox (on by default):
  - It never lists the current branch, the default branch, or a branch with unmerged commits.
    Those need the per-branch Delete and its warning.
  - "Also delete them on origin" is off by default.
  - The button says how many will go ("Delete 2 branches").

**Sync.**
- **Fetch** runs `git fetch --prune`. There is an optional auto-fetch every N minutes; it is off by
  default and only fetches, never merges.
- **Pull** follows the user's `pull.rebase` config. If it is unset, a setting chooses merge or
  rebase, defaulting to merge.
- **Push** pushes the current branch. With no upstream it uses `-u origin <branch>` (the remote is
  asked for when there are several).
- **Sync** = pull, then push. It is shown as "⇅ 2↓ 1↑" and is disabled when there is nothing to do.
- **Force push** is only on the right-click menu. It uses `--force-with-lease` and asks first.
- **Authentication:**
  - Git runs with `GIT_TERMINAL_PROMPT=0`, so a missing credential fails fast instead of hanging.
  - The error then offers **Run in terminal**, which runs the same command in the terminal panel,
    where password, passphrase and 2FA prompts work. Credential helpers keep working as usual.
- Long operations show progress (`--progress`, read from stderr) and can be cancelled.

**Remotes.** The `⋯` menu has a small Remotes dialog: list, add, rename, remove and change the URL.
It also has **Clone…**, which asks for a URL and a folder, clones, and opens the folder.

**Commands and connector.**
- Commands: `GitSwitchBranch`, `GitNewBranch`, `GitFetch`, `GitPull`, `GitPush`, `GitSync`,
  `ShowBranches`.
- Connector:
  - `git_branches`;
  - `git_switch` (name, create);
  - `git_fetch`, `git_pull`, `git_push`.
- Force push and remote delete are not connector tools.

## Tests

**Unit:**
- `parseBranches` (tracking info, gone upstream, detached HEAD, merged flag, last-used order).
- The clean-up candidate list never includes the current, default or unmerged branches.
- Branch name validation.
- The choice of pull strategy.

**Integration (temp repository with a bare "origin" in the same folder):**
- Create, switch, rename and delete branches, including the unmerged-delete prompt.
- Switching with blocking local changes.
- Pushing with no upstream sets it.
- Ahead/behind after commits on both sides; Sync brings them level.
- Fetch prunes a branch deleted on the remote.
- Auth failure with a fake unreachable remote shows the terminal offer instead of hanging.
- Clean up deletes the merged and gone branches it lists, and nothing else.

**UI:** the sweep covers the picker, the new-branch box, the Remotes dialog and the clone dialog.
