# 07 · Undo by edits, not snapshots

**Problem.** Every undo step stores a copy of the whole document (and each new step copies it
again): memory and time grow with file size.

**Design.** `TextBuffer` journals its own changes while a journal is open: every insert / erase /
line operation records (position, removed text, inserted text). An undo step is the journal of one
edit plus the caret and selection before and after. Undo replays it backwards, redo forwards.
Coalescing (typing, deleting) appends to the open step. `setText` records one replace-all entry.

**Tests.** Unit: undo/redo round trips for every editing command (typing, newline with smart indent,
backspace, line operations, replace all, indent conversion); memory: 100 edits on a 1 MB document
keep the history small; existing undo tests still pass.
