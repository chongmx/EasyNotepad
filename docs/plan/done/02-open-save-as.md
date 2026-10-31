# 02 · Open and Save As

**Problem.** File > Open is a stub and there is no Save As; untitled documents cannot be saved.

**Design.** An in-app file dialog (same look on every platform, works on the web build, drivable by
agents), not the OS dialog:
- `FileDialogViewModel`: mode (Open / Save As / Open Folder), current folder, entries from
  `IFileService::list`, a filter, the typed name/path, history (Back / Up), the workspace root and the
  home folder as places.
- Open: Enter or double-click opens; typing a path or a name filters; Ctrl+O.
- Save As: folder + file name, overwrite confirmation, the document takes its new path and language;
  Ctrl+Shift+Alt+S. Saving an untitled document opens Save As.
- Files outside the workspace open with absolute paths (the document keeps the absolute path).
- Open Folder sets the workspace (`openFolder`).

**Tests.** Unit (temp folder): listing sorts folders first; navigate down/up; Open opens a file;
Save As writes the new file, renames the tab, changes the language, asks before overwriting; untitled
Save goes to Save As. UI sweep: dialog in both modes.
