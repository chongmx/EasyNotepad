# 03 · Recent files and session restore

**Problem.** "Recent files" is a fixed sample list and a restart forgets the folder and the open files.

**Design** (stored by the settings store, item 08):
- Recent files: the last 15 files opened or saved (absolute paths), newest first, missing files
  dropped; recent folders likewise (10). File menu: Recent files, Recent folders, Clear.
- Session: on exit the workspace folder, open files (in tab order), the active one and each caret
  line are saved; on start without arguments they are restored. Command-line arguments win.

**Tests.** Unit: opening files fills the list (deduplicated, capped, newest first); session saved and
restored by a second view-model on the same settings file; arguments override the session.
