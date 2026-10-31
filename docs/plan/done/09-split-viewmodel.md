# 09 · Smaller main view-model

**Problem.** NotepadViewModel (2,229 lines) owned documents, editing, history, find, folding,
commands, settings, the agent pointer and the tour.

**Done.**
- `EditHistory`: the undo journal of item 07, per document (begin/end/undo/redo, nesting,
  coalescing). NotepadViewModel keeps `beginEdit`/`endEdit` for the refresh around it.
- `PointerViewModel`: the agent cursor, the guided tour and the "show me where" hints.
- `SyntaxColorSettings`: the colour overrides and their settings section.
- The command table and the line operations moved to their own source files
  (`NotepadCommands.cpp`, `LineOperations.cpp`); they stay members because they bind everything.
- NotepadViewModel.cpp: 2,229 → 1,379 lines; the public API is unchanged (forwarders), so no caller
  changed.

**Deferred.** A separate `DocumentsViewModel` (the open documents and their tab operations): about
500 lines that most of the class touches; moving them would rewire everything for little gain now.
Worth revisiting if documents get more state (splits, pinned tabs).

**Tests.** All existing tests pass unchanged, on Windows and Linux.
