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
| 12 | [Files changed on disk](12-external-changes.md) | Edits made outside the editor go unnoticed | |
| 13 | [Macros and SHA-256](13-macros-and-hash.md) | Stubs | |
