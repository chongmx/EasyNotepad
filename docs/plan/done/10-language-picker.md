# 10 · One language picker

**Problem.** Seven `Language*` commands exist for seven of the 21 languages; the ribbon offers only
those; the status-bar menu has its own list.

**Design.** One `ChooseLanguage` command opens a searchable language picker (all languages of the
table, the current one checked, type to filter, Enter picks). The ribbon's Language group, the
status bar and the command search all use it. The seven old commands are removed; the connector
gets `set_language` (by table key) instead.

**Tests.** Unit: `set_language` for every table key. UI sweep: picker open, filtered.
