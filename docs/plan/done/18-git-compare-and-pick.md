# 18 · Git: compare and pick files

**Problem.** Taking only part of another branch's work is common:
- one fixed file from a colleague's branch;
- two of five commits;
- one file out of a stash.

Git can do all of it (`restore --source`, `cherry-pick`, `diff | apply`), but only from the
command line, one cryptic command at a time.

## Goal

You can see what another branch, tag, commit or stash has that yours doesn't, tick what you want,
and press one button. Every place that lists a file from somewhere else offers the same few
actions.

```
┌ History ── [Log] [Compare] ── Into [feature/git-ui ▾] ⇄ take from [fix/tab-overflow ▾] ───────────┐
│ 3 COMMITS on fix/tab-overflow, not on feature/git-ui │ 4 FILES THAT DIFFER   ☑ Stage [Take 2 files] │
│                                    [Cherry-pick 3]   │ ☑ M MainView.cpp         src/view  +64 −18   │
│ ☑ 7e7e7e7 Clamp the tab strip scroll   3 hours ago   │ ☐ M DocumentListView.cpp src/view  +22 −5    │
│ ☑ 6d6d6d6 Keyboard navigation …        yesterday     │ ☑ M UiIdTest.cpp         tests     +31 −0    │
│ ☑ 5c5c5c5 Keep the + button …          2 days ago    │ ☐ A tabs.md              docs      +40 −0    │
└──────────────────────────────────────────────────────┴───────────────────────────────────────────────┘
```

## Design

**Compare view.** This is the History tab's second mode (item 17 adds the tab).
- **Choosing what to compare.** Two pickers list the current branch, other local branches,
  remotes and tags, with a swap button. "Into" is where things land, so it has to be the current
  branch before anything can be taken; the buttons say so otherwise.
- **Commits** on "take from" that are not on "Into" (`git log --cherry-pick --right-only
  into...from`): already-picked equivalents are hidden.
  - Each has a checkbox, on by default.
  - **Cherry-pick N** copies the checked commits onto the current branch, oldest first
    (`git cherry-pick <hashes>`).
  - A conflict stops in item 16's flow, with "commit 2 of 3" and Skip.
- **Files** that differ (`git diff --name-status --numstat into...from`).
  - Each has a checkbox, off by default.
  - **Take N files** writes those files' versions from "take from" into the working tree
    (`git restore --source=<from> --worktree -- <paths>`). With ☑ Stage it also stages them
    (`--staged`).
  - A file you have edited asks first and names it.
  - A click shows the file's diff between the two refs in the Diff tab.
- **Ways in:**
  - the branch list's "Compare with current" (item 15);
  - the Git tab's "⋯" menu, "Compare branches...";
  - a commit's "Compare with working tree".

**Per-file actions.** One menu, used for every file listed under a commit, a stash or a
comparison:

| Action | What it does | Git |
|---|---|---|
| Open this version | Opens the file as it is there, read-only, in a tab | `git show <ref>:<path>` |
| Compare with my copy | That version against the working tree, in the Diff tab | `git diff <ref> -- <path>` |
| Restore this file from here... | Replaces your copy with that version (asks if you have edits) | `git restore --source=<ref> -- <path>` |
| Apply only this file's change | Applies that commit's change to the file on top of your copy | `git diff <c>^ <c> -- <path>` piped to `git apply --3way` |
| File history | History filtered to this file | `git log --follow -- <path>` |
| Copy path | | |

- "Apply only this file's change" is offered for commits and stashes, where there is a single
  change to apply. It is not offered for a comparison, which spans many commits.
- A file restored or applied shows up under Changes (or Staged), so it is reviewed and committed
  like any other edit. Nothing is committed for you.

**Take one file from a branch.** A changed file's menu in the Git tab has **Take this file from ▸**
with the same list of branches and tags. It is the quick path when you already know where the
good version is.

**Connector.**
- `git_compare` (into, from): returns the commits and files.
- `git_cherry_pick` (hashes).
- `git_take_files` (from, paths, stage).
- `git_restore_file` (ref, path).
- Each one leaves the result in the working tree for the user to review; none of them commits.

## Tests

**Unit:**
- Parsing `log --cherry-pick --right-only` and `diff --name-status --numstat` output (renames,
  binary files show "-" counts).
- The per-file action list for each source kind (commit, stash, comparison).

**Integration (temp repository, two branches):**
- Compare lists exactly the commits and files that differ; after cherry-picking one commit it
  drops out of the list.
- Cherry-pick two of three checked commits, in order. A conflicting one stops with the banner
  showing "2 of 2", then Skip.
- Take two files with Stage on: `git diff --cached --name-only` is exactly those two, and the rest
  of the working tree is untouched.
- Take a file you have edited: it asks first; Cancel leaves it unchanged.
- Restore one file from a stash: only that file changes, and the stash is kept.
- Apply only one file's change from a three-file commit: only that file changes.

**UI:** the sweep covers the Compare view (both pickers, swap, checkboxes, both buttons) and the
per-file menu in all three places.
