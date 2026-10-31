# 19 · Learn Git: an interactive guide

**Problem.** Git's ideas (staging, commits, branches, remotes) are the hard part for new users, and
the Git interface (items 14–18) assumes them. Text help in a separate page explains words, not the
buttons in front of you.

## Goal

A beginner learns git inside the app, on the real controls, without risking a real project:
- each idea is explained in plain words with a small picture;
- **Show me** points at the control;
- **Try it** asks them to do it and ticks itself when done;
- anything they point at is explained as they go.

The same guide is the walkthrough for reviewing items 14–18. Each lesson carries **plan notes**:
- which item builds it;
- what the preview already shows;
- what is still to build;
- a "checked" box.

## Where it lives

- **Help menu**: the title bar's "?" button is now a menu: Take the tour, **Learn Git**, About.
- The **Git ribbon tab**: a Learn group with Learn Git.
- The **Git tab**:
  - its demo banner has a graduation-cap button;
  - its "⋯" menu has Learn Git...;
  - the "not a repository" screen offers "New to git? Learn the basics".
- The **status bar** demo badge: clicking it opens the guide.
- The guide is a floating window over the right of the workspace. It is non-modal, so every
  control it explains stays usable. It never grows taller than the app window: the lesson scrolls
  and Back / Next stay in view.

## Demo data, said plainly

- The guide always runs on demo data: a made-up repository with changes, branches, commits, stashes
  and a comparison.
  - Until item 14 connects git, all Git views use it.
  - Afterwards the guide switches to its own demo repository (an in-memory model, or a throw-away
    folder under the temp directory), never the user's.
- The demo state is shown in three places, worded for someone who has never used git:
  - A banner at the top of the Git tab: orange "Demo data", purple "Learning · demo data" while
    the guide runs. Its tooltip says nothing touches the real project.
  - A badge in the status bar: "DEMO DATA" / "LEARNING GIT · DEMO DATA".
  - A line at the top of the guide: "You are practising on demo data. Nothing you do here touches
    your real files."
- **Reset demo data** (banner and guide) puts every file, branch, commit and stash back, and
  restarts the "Try it" ticks.

## Lessons

| # | Lesson | Picture | Try it (ticks itself) | Plan item |
|---|---|---|---|---|
| 1 | What is git? | files → staging → commits → remote | Open the Git tab | 14 |
| 2 | Changes (M, A, D, U) | files lit | Click a changed file to see its diff | 14 |
| 3 | Staging | files + staging lit | Stage Main.cpp | 14 |
| 4 | Commit | staging + commits lit | Write a message and commit | 14 |
| 5 | History | commits lit | Open History | 17 |
| 6 | Branches | two lines of commits, "you are here" | Create or switch a branch | 15 |
| 7 | Merging and conflicts | the feature line joining main | Merge preview: resolve, then Continue or Abort | 16 |
| 8 | Remotes: push and pull | commits + remote lit | Press Sync | 15 (remotes and publishing: a later item) |
| 9 | Stash | the stash drawer under the files | Stash some changes | 17 |
| 10 | Taking work from elsewhere | one commit copied across | Cherry-pick or take a file | 18 |
| 11 | Undoing safely | files, staging, commits lit | (reading only) | 14, 17 |
| 12 | Tidying up branches | a merged, faded branch | Clean up merged branches | 15 |

Each lesson has:
- the explanation;
- "In this app" (where the control is);
- **Show me**: opens what is needed (the Git tab, History, the branch list) and points the agent
  cursor at the control;
- the task;
- the plan notes.

A row of dots shows progress: green when a task is done, a ring on the current lesson. The
contents list marks each lesson done or reviewed.

## Help while you work

- **What's this?**
  - While the guide is open, pointing at any Git control outlines it and explains it in the guide,
    in one or two plain sentences.
  - The text is kept per control id: rows, groups, buttons, the branch chip, ribbon commands,
    History, Compare.
  - It never competes with the control's own tooltip.
- **What just happened.** Every demo action reports itself in the status bar ("Demo: …") and the
  guide repeats it in plain words. For example: "Main.cpp is staged: it will go into your next
  commit", or "saved your staged changes as a snapshot (commit e095028 …). It is now the newest
  entry in History, and the up-arrow counts it as one more commit to push."

## Design notes

- The lessons, the per-control explanations and the plan notes are tables in `view/GitView.cpp`.
  When item 14 lands they move to a small `viewmodel/GitGuide` so the connector can drive them.
- "Try it" checks read the demo state (the number of commits, stashes and branches since the guide
  opened or was reset; whether a diff was shown; the merge state).
- Show me reuses the agent cursor (`pointAt`) and the UI element registry, so it points at exactly
  what an agent would click.
- Connector (with item 14): `guide_open` (lesson), `guide_state` (lesson, ticks, reviewed). An
  agent can then teach: "let me show you how to stage a file".

## Tests

**Unit:**
- Every lesson's Show me target and every "What's this?" prefix names a control id that exists
  while the guide is open.
- Each Try-it check is false after Reset and true after its action.

**UI sweep:**
- The guide on every lesson, with Show me pressed.
- Reset in the middle of a merge.
- The guide on a short window: Back / Next visible.
- No ID conflicts.

**Manual:** a person who has never used git completes lessons 1–4 without outside help.
