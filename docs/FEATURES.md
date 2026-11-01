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

- **Agent chat with Claude Code.** By default your question goes to Claude Code, run in the
  workspace folder under your own `claude` login. The app finds `claude` on PATH, in its
  installer's folders (`~/.local/bin`, `~/.claude/local`), or inside the Claude Code extension for
  VS Code. The chat is one conversation: each prompt continues it, and **New chat** starts another.
  The answer streams into the panel as it is written. Each step the agent takes shows as a line
  ("Read src/main.cpp", "Edit hello.py", "click panel-tab:Terminal"), and the line under the chat
  says what it is doing now. The file and line you are on travel with the prompt.
  - **It edits files directly** in the default *Edits files* mode, and the editor reloads them.
    *Plan only* lets it read and propose without changing anything. The model menu picks Opus,
    Sonnet or Haiku, or leaves Claude Code's own setting (*Default model*). Shell commands are not
    allowed.
  - **It can operate the app** when you ask it to ("show me where…", "open the terminal"). It
    gets the app's MCP connector, so it moves its glowing pointer, points at controls and clicks
    them, without touching your mouse.
  - **Answers are formatted.** Headings, bold and italics, lists and task lists, quotes, tables,
    links and code are shown as they are meant to look. Code blocks are coloured for their
    language and have a copy button. Right-click a message to copy it as written.
  - The status bar shows the turns and cost of each answer.
  - The model, the mode, the agent command and app control are remembered between sessions.
  The agent runs in the background and the editor stays responsive
  while it works. **Stop** ends the agent program and anything it started
  within a fraction of a second, whether it is answering the chat, dissecting the project or
  writing a commit message. Closing the app ends it too.
- **Agent setup** (the gear in the Agent panel). It opens by itself when Claude Code is missing
  or a run fails because it is not logged in.
  - **Status:** whether Claude Code was found, and where.
  - **Install:** the official one-line installer for your system, with *Install in terminal*
    (it runs in the built-in terminal) and *Copy*. Other ways are listed too (winget, Homebrew,
    npm).
  - **Log in:** *Log in in terminal* starts `claude` so you can sign in with your Claude account
    (Pro, Max, Team, Enterprise or Console).
  - **Check again** looks for it once more after installing, and *Setup guide* opens Anthropic's
    setup page.
  - **Agent command:** leave it empty for Claude Code, or give another command that gets the
    prompt on stdin and answers on stdout. That command answers in one piece, without streaming
    or app control, and the recent conversation is sent along with each prompt. The
    `EASYNOTEPAD_AGENT_CLI` environment variable still works when the field is empty.
  - **App control:** a switch for whether the agent may point and click in the window.
  - The app stores no keys: Claude Code keeps its own login.
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

## Markdown files shown formatted

A Markdown file you open is shown formatted, as a readable column. It has headings, emphasis,
lists and task lists, quotes, tables, links and coloured code blocks.

- **Edit** (top right of the file), **Ctrl+Shift+V** or *Rendered* in the View tab switches to
  the text to edit, and back.
- **Double-click** a paragraph to edit the text right there.
- Switching back to the formatted view keeps your place.
- Links open in the browser, or open the file they point to in the workspace.
- New untitled Markdown documents start as text.

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

## Files changed by other programs

When another program (git, a formatter, an agent) changes an open file, EasyNotepad notices within
a couple of seconds:

- If you have no unsaved changes in it, it simply reloads, keeping your place.
- If you do, a bar above the editor says so and offers **Reload** (take the file's version),
  **Keep mine** (your next save replaces the file) or **Compare** (the Diff tab, the file on disk
  against your text). Until you choose, saving refuses to overwrite the other program's change.
- If the file is deleted, the bar says so; **Keep editing** and saving recreate it. If it comes back
  by itself, the bar goes away.

## Macros and SHA-256

**Record** (Tools tab) starts a macro: what you type, Enter, Backspace, Delete, Tab, caret moves and
editing commands (duplicate, delete or move lines, sort, change case, comment, indent, trim, select
all) are recorded until **Stop**. Caret moves are recorded relative to where the caret was, so
**Play** repeats the macro wherever the caret is now (record "edit this line, go to the next one",
then press Play on each following line). A whole playback is one undo step. Stopping without having
recorded anything keeps the previous macro, and the last macro is remembered between sessions.

**SHA-256** (Tools > Utilities) shows the hash of the selection, or of the whole document when
nothing is selected, with a **Copy** button.

## Git: changes, staging and commits

The **Git** tab of the Explorer (next to Files and Open) shows the folder's repository: **Staged**,
**Changes**, **Untracked** and, during a merge, **Conflicts**, each file with a coloured letter
(M modified, A added, D deleted, R renamed, U untracked, C conflict). The same letters colour the
Files tree (folders holding changes get a dot) and the titles of open tabs.

- Click a file to see what changed in the **Diff** tab (the open document's unsaved text included);
  double-click opens it. Ctrl-click and Shift-click select several; a bar then stages, unstages,
  stashes or discards them all.
- Hover a row for **+** (stage), **−** (unstage), **↶** (discard, asks first); each group's header
  does the same for every file in it. Right-click for more, including **Take this file from** any
  branch or tag.
- In the Diff tab, click lines (Shift-click a range, double-click a whole change) and **Stage
  chosen**, **Unstage chosen** or **Discard chosen**: commit part of a file.
- Write the message and press **Commit** (Ctrl+Enter); with nothing staged it commits everything.
  **Amend** changes the last commit; the menu beside it commits and pushes or syncs. The magic-wand
  button asks the agent to write the message from the staged changes (without an agent it lists the
  files).
- A folder that is not a repository offers **Initialize repository** and **Clone a repository**.
- **Submodules** (repositories inside this one) show with a cube icon and what changed in them: new
  commits, modified files or untracked files. Click one for the commits it moved between; right-click
  > **Update submodule** checks out the commit this repository records.
- **⋯ > Show git status** shows git's own summary text.

Everything runs the `git` you have installed (set `EASYNOTEPAD_GIT` to use another), with your
settings, credentials and hooks. The view refreshes after saves, after each action and every few
seconds while it is on screen.

## Git: branches and the server

The status bar shows the branch, how many commits are waiting to be pushed (↑) and pulled (↓), and a
dot when there are uncommitted changes. Click it (or the branch name in the Git tab) for the branch
list:

- Type to filter; click a branch to switch (a remote branch gets a local branch that tracks it; a tag
  is opened "detached"). If your changes are in the way, you are offered **Stash and switch**.
- **New branch** (from the current one, or from a commit in History), **Rename**, **Delete** (asks;
  a branch with unmerged commits asks again), **Merge into current**, **Rebase current onto this**,
  **Compare with current**, and deleting a branch on the server.
- Labels show branches that are **merged** and whose server copy is **gone**; **Clean up...** deletes
  just those, after you tick them. Branches sort by recent use or by name.

**Sync** pulls then pushes; **Publish** appears instead for a branch the server does not have yet.
Pull, Push, Fetch and **Force push (with lease)** are in the Git ribbon tab and the Git tab's ⋯ menu.
When the server needs a password, git does not hang: you are offered **Run in terminal**, where it
can ask. When someone pushed first, you are offered **Pull, then push**. **Remotes...** lists, adds,
renames and removes remotes and changes their addresses; **Clone a repository...** copies one into a
new folder and opens it.

## Git: merge conflicts

When a merge, rebase, cherry-pick or revert stops on conflicts, a banner in the Git tab says so,
counts the conflicts left and offers **Continue** (once all are resolved), **Abort** (asks first)
and, for a rebase or cherry-pick, **Skip** and "commit 3 of 7". In the editor each conflict block is
tinted (yours and the incoming side) with **Accept current**, **Accept incoming**, **Accept both**
and **Compare** above it; a bar counts them, and **F8** / **Shift+F8** jump between them. Saving a
file with no conflict markers left marks it resolved.

## Git: history, blame, stash and changed lines

- The **History** tab of the bottom panel lists commits with a branch graph, refs, author and age;
  filter by message, author or hash, choose this branch or all, or one file's history. A commit shows
  its files with line counts and offers **Checkout**, **Cherry-pick**, **Revert**, **Create branch
  here**, **Reset** (soft, mixed, or hard after asking), **Create tag**, and comparing it with your
  files.
- **Blame** (Git ribbon tab; click again to hide it) adds a column to the editor: who last changed
  each line and when; hover for the commit's message, click to find it in History. Drag its right
  edge to resize it (remembered); narrower, it drops the author, then the age, down to the hash.
  Double-click the edge for the full width. **Ctrl+Alt+B** hides the Layers / Minimap side bar to
  make room.
- A thin bar beside the line numbers marks lines added or changed since the last commit (a red mark
  where lines were deleted). Click it to see what was there, **Revert this change** or open the diff.
- **Stash...** puts changes aside, with a name, all changes or only the selected files, with or
  without untracked files, optionally keeping staged changes. The Stashes list shows each stash's
  files; right-click a stash to apply, pop, apply on a new branch or drop it.

## Git: compare and take work from elsewhere

**Compare** (in History, or "Compare with current" on a branch) shows what another branch, tag or
commit has that yours lacks: its commits, to tick and **Cherry-pick**, and the files that differ, to
tick and **Take** into your folder (optionally staged; asks first if you have edits there). Wherever a
file is listed under a commit, a stash or a comparison, right-click it to **Open this version**,
**Compare with my copy**, **Restore this file from here**, or **Apply only this file's change**.
Nothing is committed for you: what you take shows up under Changes or Staged.

Agents get the same through the connector: `git_status`, `git_diff`, `git_stage`, `git_unstage`,
`git_commit`, `git_branches`, `git_switch`, `git_sync`, `git_log`, `git_conflicts`, `git_resolve`,
`git_stash`, `git_compare`, `git_take_files` and `git_cherry_pick`.

## Learn Git

**Help (?) › Learn Git** (also in the Git ribbon tab and the Git tab) opens a guide in twelve short
lessons, from "What is git?" to tidying up branches, for people who have never used git. It always
runs on **demo data**, a made-up repository, so nothing real is touched; a banner, a status bar badge
and the guide itself say so, and **Reset demo data** starts it again. Each lesson explains one idea
with a small picture, **Show me** points at the real control, and a **Try it** task ticks itself when
done. While the guide is open, pointing at any git control outlines it and explains it ("What's
this?"), and every action is described in plain words ("What just happened"). Each lesson also lists
which part of the implementation it covers, with a box to tick, for reviewing.

## Explorer right-click menu

Right-click a file, a folder, or the empty space under the tree (the opened folder itself):

- **Open**, and **Open changes** for a file git sees as changed.
- **New File...** (opened at once) and **New Folder...**, in that folder.
- **Find in Folder...**: the Search tab, limited to that folder (shown above the results, with a
  button to search all files again). **Open Terminal Here**.
- **Copy Path**, **Copy Relative Path**, **Reveal in File Explorer** (Finder on macOS, the file
  manager on Linux).
- **Git**: stage, unstage or discard (asks) what changed in it, its file history, and blame.
- **Rename...** (open documents follow the new name) and **Delete...** (asks first; on Windows it goes
  to the Recycle Bin, elsewhere it is deleted; open documents in it close).

Creating, renaming and deleting need an opened folder; in the sample workspace the menu says so.

## Closing a folder, and dialogs

**File › Close Folder** (also on the right-click menu of the empty Explorer space) closes the folder
and its documents, asking first to save or discard unsaved changes. The Explorer then shows "No folder
open" with **Open a folder** and the recent folders. An open file's right-click menu in the Explorer
has **Close**. Every dialog's **×** closes it (agents can use it too, through the connector).
