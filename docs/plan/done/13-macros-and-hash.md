# 13 · Macros and SHA-256

**Problem.** Macro playback and the SHA-256 tool were stubs; recording only set a flag.

**Done.**
- Macros: while recording, typed text (coalesced), Enter, Backspace, Delete, Tab, caret moves
  (relative to where the caret was, so a macro replays anywhere) and editing commands (duplicate,
  delete and move lines, sort, case, comment, indent, outdent, trim, select all) are recorded. Only
  the outermost action is recorded, so a command's internal edits are not recorded twice. Play
  replays them at the caret as one undo step; the view-model can play N times (`playMacro(n)`), the
  ribbon plays once. Stop with nothing recorded keeps the previous macro. The last macro is kept in
  the settings.
- SHA-256: of the selection, or of the document when nothing is selected; shown in a dialog with
  Copy. Self-contained implementation in `model/Hash`, checked against the FIPS 180-4 vectors.

**Not done.** A "Play N times" control in the ribbon (the view-model supports it).

**Tests.** Unit: SHA-256 test vectors (including one million 'a'); a recorded macro (typing, a line
command, a caret move) replays elsewhere twice as one undo step and is remembered across sessions.
UI sweep: the hash dialog.
