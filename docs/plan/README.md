# Plan

Work items from the review of what is missing or worth streamlining, in the order they are done.
Each file says what the item is for, how it is built, and how it is tested. When an item is done,
it is described as a feature in [../FEATURES.md](../FEATURES.md), marked here, and its file moves to
[done/](done/).

| # | Item | Why | Status |
|---|------|-----|--------|
| 01 | [Unsaved changes are never lost](done/01-unsaved-changes.md) | Closing a tab or the app discards edits silently | Done |
| 02 | [Open and Save As](done/02-open-save-as.md) | "Open" is a stub; there is no Save As | Done |
| 03 | [Recent files and session restore](done/03-recent-and-session.md) | "Recent files" is a fixed sample list; nothing is restored on restart | Done |
| 04 | [Find in Files](done/04-find-in-files.md) | Stub | Done |
| 05 | [Real workspaces show real data](done/05-real-workspace-mode.md) | Sample agent chat and review findings appear in real folders | Done |
| 06 | [Highlighting cost independent of file size](done/06-highlight-cache.md) | Every frame re-highlights from line 1 to the screen | Done |
| 07 | [Undo by edits, not snapshots](done/07-undo-journal.md) | Every undo step copies the whole document | Done |
| 08 | [One settings store](done/08-settings-store.md) | Settings code lives ad hoc in the main view-model; most settings are not saved | Done (before 03, which builds on it) |
| 09 | [Smaller main view-model](done/09-split-viewmodel.md) | NotepadViewModel is ~1,900 lines doing everything | Done (documents split deferred) |
| 10 | [One language picker](done/10-language-picker.md) | Seven per-language commands; the ribbon offers 7 of 21 languages | Done |
| 11 | [One document list](done/11-document-list.md) | The same open-document rows are drawn three ways | Done |
| 12 | [Files changed on disk](done/12-external-changes.md) | Edits made outside the editor go unnoticed | Done |
| 13 | [Macros and SHA-256](done/13-macros-and-hash.md) | Stubs | Done |
| 14 | [Git: status, diff and commit](done/14-git-status-and-commit.md) | No source control in the editor; also builds the git foundation 15–18 use | Done |
| 15 | [Git: branches and sync](done/15-git-branches-and-sync.md) | Switching, creating, merging, pull and push need the terminal | Done |
| 16 | [Git: merge conflicts](done/16-git-merge-conflicts.md) | Conflicts mean hand-editing markers and remembering commands | Done |
| 17 | [Git: history, stash and line-level changes](done/17-git-history-stash-lines.md) | No log, blame, stash, gutter markers or partial staging | Done |
| 18 | [Git: compare and pick files](done/18-git-compare-and-pick.md) | Taking some commits or files from another branch, commit or stash needs several git commands | Done |
| 19 | [Learn Git: an interactive guide](done/19-learn-git-guide.md) | Beginners need git's ideas explained on the real controls, safely; reviewers need a walkthrough of 14–18 | Done |
| 20 | [Shared control: connected agents, acting agents and the task manager](20-shared-control.md) | Up to 16 agents connect and read; 2 may act; one task manager drives the pointer; the user first | Planned |
| 21 | [A local agent on the device (Gemma 4 E2B)](21-local-agent.md) | Offline code completion, judging agent requests, small tasks locally, escalating big ones; a safety net for its faults | In progress: Claude / Local / Auto in the Agent panel, fallback, local answers (done); request judging, watching agents, inline completion (to do) |
| 22 | [Drag-to-scroll inside Dear ImGui](22-imgui-drag-scroll.md) | Touch scrolling belongs in ImGui, not in an app-side workaround; and the ImGui version is not truly locked | Planned (not started) |
| 23 | [Local inference engines: start on the CPU, climb to the fastest](23-local-inference-engines.md) | The local agent's speed depends on reaching the GPU/NPU; each vendor's path differs. Bootstrap on CPU, probe, build or download, verify, switch, drop back. Phase 1: ggml (CPU + Vulkan) and fixed TensorRT | In progress: 23.0 spike done on Linux; Windows, Android and R2 INT4 next |

Done without a plan item (see [../FEATURES.md](../FEATURES.md)):
- The in-app agent: Claude Code streaming, set-up, Stop, and Markdown answers.
- Markdown files shown formatted.
- Windows 7 and 32-bit builds.
- The phone layouts.
- The slim ribbon.

**Reviewing items 14–19:** open Help (?) › Learn Git. Each lesson names its plan item, what was built
and anything left for later, with an "I have checked this part" box; it runs on demo data that Reset
puts back.

Where the git items differ from their plans:
- Commit messages go to `git commit -F -` on stdin, not through a temp file; git runs without a shell
  (exact arguments), so there is no quoting at all.
- Commit with nothing staged commits everything; the button says so ("Commit all") instead of asking.
- Branches sort by the time of their last commit, not the reflog.
- Open tabs show git status by the colour of their title, not a letter.
- Left for later: fetching on a timer, creating a repository on GitHub, choosing which remote branch a
  local branch tracks, and a git path setting (the `EASYNOTEPAD_GIT` variable covers it).
