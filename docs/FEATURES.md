# Features

What EasyNotepad does, feature by feature, as built from the [plan](plan/README.md). Each entry says
what the user (or an agent) can do and how it behaves at the edges.

## Unsaved changes are never lost

Closing a document that has unsaved changes asks first: **Save**, **Don't Save** or **Cancel**. This
applies to every way of closing a document: the tab's ×, a middle click, Ctrl+W, the File menu, the
close button in the explorer's Open list and the tab context menu. Documents without changes close
at once. Bulk closes (Close Others, Close to the Right, Close All) keep unsaved documents open and say
so in the status bar.

Quitting with unsaved documents (the window's close button, Alt+F4, the taskbar, File > Exit) lists
them and offers **Save All and Quit**, **Quit Without Saving** or **Cancel**. If any document cannot
be saved, the app stays open on that document instead of quitting.

For an untitled document the prompt's button reads **Save As...**: it asks where to save, and the
document closes once it is saved. When quitting, untitled documents must be saved first (Save All
and Quit is disabled while there are any).

Agents can answer the prompts too: they are on-screen elements (`dialog:save-changes:save`,
`:discard`, `:cancel`, `dialog:quit:*`), and the prompt closes whichever way it was answered.

## Open, Save As and Open Folder

**File > Open** (Ctrl+O), **Save As** (Ctrl+Alt+S) and **Open Folder** (Ctrl+Alt+O) use the
editor's own file dialog, which looks and works the same on every platform:

- **Places** on the left (the workspace, your home folder, the drives on Windows or `/` elsewhere),
  the folder's contents on the right (folders first), **Back** and **Up**, and a folder box you can
  type a path into.
- In **Open**, typing a name narrows the list; typing a path (or a name) and pressing Enter opens it;
  double-clicking a file opens it, a folder goes into it. Files outside the workspace open too.
- In **Save As**, the document takes the new name, path and language (saving `notes` as `notes.py`
  turns on Python colouring and folding). Replacing an existing file asks first (**Replace**).
- **Save** on an untitled document opens Save As. In the "save changes?" prompt, **Save As...**
  saves an untitled document and then closes it.
- **Open Folder** makes the chosen folder the workspace.
- The dialog starts in the active document's folder (or the workspace, or home).

Files inside the workspace are always tracked by their workspace-relative path, however they were
opened, so the explorer, tabs and search treat them as the same file. Agents can drive the dialog:
its rows, places, name box and buttons are on-screen elements (`file-dialog:entry:<name>`,
`file-dialog:confirm`, ...).

## Settings are remembered

Everything you set up is kept between sessions in one `settings.json` in the app's data folder
(`%APPDATA%\EasyNotepad` on Windows, `$XDG_CONFIG_HOME/easynotepad` or `~/.config/easynotepad` on
Linux, `~/Library/Application Support/EasyNotepad` on macOS):

- **View:** theme, workspace (Agent / Code), which panels are shown, the bottom panel's tab, the side
  bar (Layers / Minimap, shown or hidden), detail level, folding mode, zoom.
- **Editor:** word wrap, whitespace, line numbers, indent guides, smart indent, default indentation.
- **Layout:** the widths of the explorer, agent and plan panels, the side bar, and the bottom panel's
  height.
- **Colours:** syntax colours per language and theme.
- **Ribbon:** pinned commands and custom actions in My Tools.
- **Performance:** redraw rate and caret blink.

Changes are written a moment after you make them (a burst of changes is one write) and once more
when the app closes. Command-line options (`--workspace=agent`, `--detail=...`) apply on top of
what was saved, and are remembered like any other change. A settings file that cannot be read is kept beside the new one
as `settings.json.bad` and the defaults are used; sections written by a newer version are kept as
they are.

## Recent files and picking up where you left off

**File > Recent files** lists the last 15 files you opened or saved (newest first; files inside the
workspace are shown by their workspace path), **Recent folders** the last 10 workspaces, and
**Clear recent** empties both. Choosing an entry that no longer exists removes it from the list.

Starting EasyNotepad without a folder or file on the command line carries on with the last session:
the same workspace, the same open files in the same tab order, the same active tab, and each file's
caret line. Files deleted in the meantime are skipped. Untitled documents are not part of the
session (they are asked about when quitting). Starting with a folder or files opens those instead;
with no session at all, the sample workspace is shown.

## Find in Files

**Ctrl+Shift+F** (Find > In Files) opens the **Search** tab of the bottom panel, with the selected
text as the query. It searches every file of the workspace:

- Options: **Aa** match case, **ab** whole word, **.*** regular expression (ECMAScript syntax; an
  invalid expression is reported, not searched). A **Files** filter limits the search (`*.cpp;*.h`,
  `test_?.py`).
- Open documents are searched in their current text, unsaved edits included. Build output and
  hidden folders, binary files and files over 4 MB are skipped. The search runs in the background;
  starting a new one discards the old.
- Results are grouped by file (click a file to fold it) and show each matching line with the match
  highlighted. Clicking a line opens the file there with the match selected. The tab shows the count.
- **Replace** (the arrow before the query): **Replace All** replaces in every result. Open documents
  are edited (one undo step each, not saved for you); other files are written. Regular-expression
  replacements can use groups (`$1`); plain replacements are literal.

Agents search with the connector's `search_files` tool, which returns the matches (path, line,
column, text) and shows them in the Search tab.

## Real folders get a real agent and real review tools

The sample conversation, plan, proposed changes, findings, comments and checklist belong to the
sample workspace only. Opening a folder clears them all and switches to live mode:

- **Agent chat.** Your question goes to the agent CLI (`claude -p` by default, or the command in
  `EASYNOTEPAD_AGENT_CLI`, which gets the prompt on stdin and answers on stdout), run in the
  workspace folder. The recent conversation and the file and line you are on travel with it, so
  follow-up questions work. **Stop** discards an answer that is still coming. With no agent CLI
  installed, the panel says so and how to connect one (or to drive the editor through the MCP
  connector).
- **Review tools.** Each tool runs the command you configure in the environment:
  `EASYNOTEPAD_LINT`, `EASYNOTEPAD_STATIC_ANALYSIS`, `EASYNOTEPAD_SECURITY`, `EASYNOTEPAD_STYLE`,
  `EASYNOTEPAD_COMPLEXITY`. In the command, `{file}` is the file you are editing and `{workspace}`
  the folder (for example `EASYNOTEPAD_LINT=clang-tidy {file}` or `ruff check {file}`). Output in
  the usual `path:line[:column]: [error|warning|note:] message [rule]` form (gcc, clang, clang-tidy,
  ruff, flake8, eslint's unix format, shellcheck's gcc format, ...) becomes findings in the Problems
  list, the gutter and the explorer badges. A tool that is not configured says which variable to set
  instead of inventing findings.
- **Agent Review** asks the agent CLI to review the current file and answer in that same format.
- Fold and layer summaries say they come from the code's own comments and structure.

## Large files stay fast

Syntax colouring no longer re-reads the file from the top on every frame. Each document remembers
the colouring state at the start of every line (inside a block comment, a multi-line string, a
Markdown code fence, ...). Drawing the screen colours only the visible lines, wherever they are in
the file. An edit forgets only what lies below the changed line, so typing near the end of a long
file does not re-colour the whole file. The editor, the minimap and folded regions all use the same
remembered state, and changing a document's language starts it afresh.

## Undo that scales

Undo and redo remember what each edit changed (the text removed and inserted, and where), not a
copy of the whole document. A run of typing or deleting is one step; an edit made of several
changes (smart Enter, a line operation, Replace All in a file) is one step; and a command that
would change nothing (upper-casing text that already is) leaves no step at all. Undo puts the caret
and selection back where they were before the edit, redo where they were after. A hundred edits
on a 1 MB file keep only a few kilobytes of history, and deciding whether a document has unsaved
changes no longer copies it either.

## Under the hood: a leaner core

Not a user-visible feature, but it keeps the others easy to change: the editor's main view-model
was split into focused parts (undo history, the agent cursor and tour, syntax colour settings),
with its command table and line operations in their own files. The main file went from 2,229 to
1,379 lines with no change in behaviour.

## One language picker

**Language** (Document tab of the ribbon, the language in the status bar, or the command search)
opens one picker with every language EasyNotepad knows. Type to filter by name or extension
(`py`, `.yml`, `cmake`); Enter takes the first match. The current language is checked, each entry
shows a typical extension, and **Customize colours...** leads to that language's colours. Agents set
a document's language with the connector's `set_language` tool (by key or name: `cpp`, `Python`,
`csv`); an unknown name gets the list of valid keys back.

## One list of open documents

The explorer's **Open** tab and the tabs' **⌄ N** overflow list now show the same rows and behave the
same: icon, name and folder, the active document highlighted, an unsaved dot that turns into a close
button on hover, middle-click to close (asking first when unsaved), drag to reorder (explorer), a
filter, and Enter to pick the first match (overflow list). Right-clicking a tab or a row opens the
same menu: Close, Close Others, Close to the Right, Close Saved, Close All, Copy Path (the full path)
and Reveal in Explorer.
