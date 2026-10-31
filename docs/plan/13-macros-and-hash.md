# 13 · Macros and SHA-256

**Problem.** Macro playback and the SHA-256 tool are stubs; recording only sets a flag.

**Design.**
- Macros: while recording, editing actions (typed text, Enter, Backspace/Delete, caret moves,
  editing commands) are appended as steps; Play replays them at the caret as one undo step;
  Play ×N; the last macro is kept in settings.
- SHA-256: of the selection, or of the document when nothing is selected; shown in a dialog with Copy.
  A small self-contained implementation in `model/Hash`.

**Tests.** Unit: SHA-256 test vectors; a recorded macro (type, newline, line operation) replays the
same changes elsewhere and undoes in one step.
