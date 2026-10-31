# 08 · One settings store

**Problem.** Syntax colours, default indent and performance are loaded and saved by hand-written code
inside NotepadViewModel; theme, layout, view toggles, pinned commands, custom actions, recent files and
the session are not saved at all.

**Design.** `viewmodel::Settings`: a JSON document with named sections, loaded once and written
(debounced) through `IFileService`. Owners register what they keep: `bind("view.darkTheme",
property)` for observable properties (loaded into them, saved when they change) and section
load/save callbacks for structured data (colours, recent files, session, ribbon customization).
NotepadViewModel's settings code moves there. Unknown sections are preserved.

Saved: theme, workspace mode, panels shown, side bar, detail level, folding mode, word wrap,
whitespace, line numbers, indent guides, zoom, smart indent, performance, default indent, syntax
colours, pinned commands, custom actions, recent files/folders, session, panel sizes.

**Tests.** Unit: properties round-trip through a file; unknown sections survive a save; a corrupt
file is ignored (and kept as `settings.json.bad`); writes are debounced.
