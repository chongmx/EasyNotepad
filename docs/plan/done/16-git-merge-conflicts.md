# 16 · Git: merge conflicts

**Problem.** A merge, rebase, pull, cherry-pick or stash pop can stop with conflicts. Resolving
them in a plain editor means hand-editing `<<<<<<<` markers and remembering which git command
finishes the operation.

## Goal

The editor says clearly that an operation is in progress, takes you to each conflict, resolves a
block with one click, and offers exactly two ways out: Continue and Abort.

```
┌ Git ───────────────────────────────────────────────┐
│ ⚠ Merging feature/git into main — 2 conflicts left │
│ [Continue merge]  [Abort merge]                    │
│ ▾ Conflicts (2)                                    │
│   !  src/app/Main.cpp                              │
│   !  README.md                                     │
```

```
Editor:
  ┌ Accept current │ Accept incoming │ Accept both │ Compare ┐
  <<<<<<< HEAD                          (current: main)
      int width = 800;
  =======
      int width = 1024;
  >>>>>>> feature/git                   (incoming)
```

## Design

**State.**
- Status (item 14) already reports unmerged entries. The operation in progress is read from the
  `.git` folder: `MERGE_HEAD`, `rebase-merge` / `rebase-apply`, `CHERRY_PICK_HEAD`, `REVERT_HEAD`.
- This gives `GitOperation {None, Merge, Rebase, CherryPick, Revert}`, with the names of the
  current and incoming sides.

**Banner.**
- Shown at the top of the Git tab and in the branch chip while an operation is in progress.
- Continue (`git merge --continue`, `rebase --continue`, …) stays disabled until no conflicts are
  left.
- Abort asks first, then runs `--abort`.
- During a rebase, the banner also shows "commit 3 of 7" and Skip.

**In the editor.**
- `model/Conflicts.h: findConflicts(buffer)` finds marker blocks: current, an optional base (diff3
  style), and incoming. It is pure and unit-tested.
- The editor tints the two sides with the theme's diff colours and draws a small action row above
  each block:
  - Accept current, Accept incoming, Accept both;
  - Compare, which opens the Diff tab with current on one side and incoming on the other.
- Each action is one undo step.
- The conflict bar (from item 12's disk bar) shows "Conflict 1 of 3 · Next / Previous". F8 and
  Shift+F8 jump between conflicts.
- When a file has no markers left, it is staged when saved (**Mark resolved**). This is a setting,
  on by default. It can also be done by hand from the row.
- Deleted-vs-modified conflicts get **Keep file** or **Delete file** instead of marker actions.
- Binary conflicts get **Use current** or **Use incoming** (`git checkout --ours/--theirs`).

**Connector.** `git_conflicts` (files and blocks) and `git_resolve` (path, block, side) let an
agent resolve conflicts with the same one-step actions. Continue and Abort are commands.

## Tests

**Unit:**
- `findConflicts` with merge and diff3 styles, nested-looking text that isn't a conflict, CRLF, and
  a marker at the end of the file.
- Each resolution's result text and its single undo step.

**Integration (temp repository):**
- A real merge conflict: the banner shows, Continue is disabled, resolving and saving stages the
  file, then Continue finishes the merge.
- Abort restores the files.
- A rebase with two conflicting commits shows the step count and Skip.
- A delete/modify conflict.

**UI:** the sweep with a conflicted file open; screenshot of the action row and banner.
