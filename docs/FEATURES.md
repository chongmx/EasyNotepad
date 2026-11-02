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
  - **Other agents can use it too:** the connector's `ask_agent` sends it a prompt, as if typed
    into the panel, and `read_agent_chat` returns the conversation, whether the agent is working,
    and on what.
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

## Phones, tablets and small windows

The layout follows the shape of the window. It is the same view on every platform, so Android, a
narrow desktop window and the web behave alike.

- **Room to spare** (a desktop, or a tablet): the full layout, with the explorer, editor, agent
  column, bottom panel and status bar.
- **Upright phone** (narrower than 600 dp): one screen at a time. A bottom bar switches between
  **Files**, **Code**, **Agent** and **Tools** (problems, diff, terminal, search, history). A dot
  on Agent means the agent is working, and a dot on Tools means there are findings.
- **Phone turned sideways** (less than 500 dp tall): the same screens, with a rail on the left
  instead, so the editor keeps its full height.
- **Top bar:** the menu button shows or hides the ribbon (every command). The document's name opens
  the list of open documents. Save, undo and redo sit on the right, and *More* has the file
  commands, theme, preferences and help.
- **Switching screens:** the app does it for you. Opening a file shows the editor, *Ask* shows the
  agent, and terminal, search or review commands show Tools.
- **Rotation** switches between the two phone layouts as it happens.
- **Android:** the app keeps clear of the status and navigation bars (including the three-button
  bar), a notch, and the on-screen keyboard, which shrinks the screen instead of covering it.
  - The safe area comes from Android's window insets, measured against the app's own view. That
    works both when Android 15 draws the app under the bars and when older versions keep it clear.
- **Touch scrolling:** drag the content with a finger. This works in the editor, the ribbon, the
  files, the chat, the terminal and the panels.
  - The content follows the finger and glides on after release; the next touch stops it.
  - A drag that starts on a button scrolls instead of pressing it, while a tap still presses.
  - In the editor a tap places the caret and a drag scrolls (a mouse still selects by dragging).
  - It works on Windows touch screens too.
- The editor's side bar (layers, minimap) is hidden in the phone layouts, giving the text the
  full width.
- The bottom panel's tabs become a drop-down when they do not fit.
- **Nothing overlaps on a narrow screen:**
  - Text that could run into its neighbour is cut short with "..." or wraps.
  - Rows of buttons and fields wrap onto another line.
  - History lists drop their less important columns (author, then hash) to keep the message readable.
  - History and Compare put details below the list instead of beside it.
  - The Problems table drops its Tool column and wraps long messages.
  - Search moves its file filter to a line of its own.
  - Dialogs (stash, branches, Learn Git, colours, preferences, About, ...) are never larger than
    the screen and are kept on it.
- **Trying it on a desktop:** `--window-size=390x844` opens a phone-sized window (in dp).
  `--safe-area=left,top,right,bottom` pretends the system covers those edges (in dp).

## Slim ribbon

The small chevron in the ribbon's lower-right corner (or **Ctrl+F1**, or View ▸ Panels ▾ ▸ *Slim
Ribbon*) makes the ribbon slim, giving about 40 dp back to the work area:

- The same buttons, as icons only in one row. Their names are in the tooltips.
- The extra commands of each group sit behind a small caret.

The chevron turns over to bring the full ribbon back. The choice is remembered.

## Local agent (offline, on the device)

Gemma 4 E2B answers in the Agent panel on the device itself: no network, no account. It is
downloaded once from the agent set-up (the gear), or from the Engine Lab.

- **Who answers** (the picker at the left of the Agent panel's header; remembered):
  - **Auto** (default): the local model for quick questions about the code (explain, what, why,
    how, summaries, commit messages, short questions); Claude Code for changes, several files,
    plans, tests and long or unclear requests. A follow-up in a conversation Claude is having
    stays with Claude.
  - **Claude**: always Claude Code.
  - **Local**: always the local model.
- **Fallback** (*Fall back to the local model*, on by default): when Claude Code is not installed,
  not logged in, offline or fails, the local model answers instead, with a line saying why. On a
  phone, where Claude Code does not run, the local model is the agent. In the sample workspace a
  ready local model answers instead of the made-up replies.
- **Answers say who gave them** (*Agent · Local* or *Agent · Claude*). Under a local answer, **Ask
  Claude instead** sends the same question to Claude Code.
- **What the local model sees:**
  - the selection, or the 40 lines around the caret, with the question (it cannot open files
    itself);
  - the recent conversation.
  It cannot edit files or use the app, and says so when asked.
- **Speed:**
  - it streams as it writes, using the drafter when it is downloaded;
  - the model loads on the first question (a few seconds) and is unloaded after 5 idle minutes,
    giving its memory back;
  - S24 Ultra (CPU): about 30 tokens/s writing, about 100 tokens/s reading the question.

## Engine lab (developer view)

A hidden page that shows how fast the on-device model (plan 23) runs on this device. It does not
take up any space in the normal UI.

- **Turning it on:** tap the version in About **seven times**, as with Android's developer options.
  After that, Engine Lab appears in Help (and in About, with a *Turn off* button). It is also the
  `EngineLab` command for agents.
- **This device:** the chip, CPU and its features, memory, the GPUs the engine can reach, and the
  engine version.
- **Model files:**
  - Google's Gemma 4 E2B (QAT, 4-bit, 3.35 GB) and its MTP drafter (77 MB), pinned to a repository
    commit and checked by SHA-256 after the download.
  - It checks for free space first.
  - Downloads resume. On Android they run in the system's download manager (also in the
    background), and they wait for Wi-Fi unless *Downloads may use mobile data* is ticked.
  - A file already on the device can be used instead.
- **Runs:** the CPU thread count, the GPU (when the build has one), and how many tokens to
  generate. *Run*, *Run with drafter*, or *Compare both* (one after the other).
- **Results:**
  - Generating and prompt speed, the time to the first token, the load time and peak memory.
  - With the drafter: the share of drafts accepted, and the speed-up against the run without it.
  - The start of what the model wrote, to check it is sensible.
  - A bar chart of the history, and *Copy report* for sharing.
  - The history is kept with the settings.
- **Where it runs:**
  - 64-bit Windows, Linux and macOS, and Android.
  - The CPU code needs AVX2 on PCs and ARMv8.2 dot product on phones; older processors get a
    message, not a crash.
  - 32-bit and web builds have no engine.

## Windows 7 and 32-bit Windows

- The Windows build starts on **Windows 7 and later**. The per-monitor DPI functions of newer
  Windows are looked up when the app runs, and the system-wide values are used where they are
  missing. Nothing newer than Windows 7 is linked, so Windows can always start the program.
- **32-bit (x86) builds:**
  - With Visual Studio: the `windows-x86` preset, which CI also builds and tests.
  - With MinGW-w64: the `windows-mingw-x86` preset, for example with w64devkit's x86 edition at
    `C:\w64devkit-x86`.
  - Either way, the output goes to `bin/windows/x86/`.
- **On older Windows:**
  - The built-in terminal needs Windows 10 1809 or later. Before that it is simply unavailable.
  - Claude Code itself needs Windows 10 1809 or later.
  - Graphics drivers must offer OpenGL 3.0.

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

## Git: status at a glance

The **Status** tab of the bottom panel, next to History, shows everything `git status` tells you.
It is also in Git ▸ Inspect, in the Git tab's ⋯ menu, and is the `ShowGitStatus` command.

- **The branch** and the remote branch it tracks (or *Detached at …*).
- **In step with the remote?** One line, coloured by what it asks of you:
  - *Up to date* (green);
  - *n commits not pushed* (orange);
  - *n to pull* (blue);
  - *Diverged: n to push, m to pull* (red);
  - *Not on the remote yet* (never pushed);
  - *Its remote branch was deleted*;
  - *Not on a branch*.

  Under it: when the remote was last checked (the last fetch), since the counts are only that
  fresh.
- **Counts:** conflicts, staged, not staged and untracked; or *Working tree clean*.
- **Only the actions that apply:** Fetch always; then Pull, Push, Sync or Publish, depending on the
  state.
- **The rest:** a merge, rebase or cherry-pick in progress (with its step), the last commit (hash,
  message, author, when), and the number of stashes.
- **The files,** grouped as git groups them (Conflicts, Staged, Not staged, Untracked), each group
  collapsible:
  - click a file for its diff, double-click to open it;
  - stage, unstage or mark resolved on hover;
  - more on right-click.
- **Layout:** summary and files side by side on a wide panel, stacked in one scrolling column on a
  phone (Tools ▸ Status).

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

## IntelliSense: completion, Go to Definition, Ctrl+click, Back and Forward

**Suggestions while typing.** In code (C/C++, C#, Java, JavaScript, TypeScript, Python, Rust, Go,
Lua, shell, PowerShell, CMake), after two letters of a name a list of suggestions opens at the
caret: the names the workspace declares (types, functions, namespaces, macros, constants, members,
variables at file and type level), the words of the document, and the language's keywords. After
`.` or `->` it lists members; after `Name::` what `Name` declares. Matching is fuzzy: `gw` finds
`getWidth`, exact-case prefixes come first, then word starts. Each row shows what the name is and
the file declaring it; the chosen one shows its declaration below the list. **Up/Down**, **Page
Up/Down** choose, **Enter** or **Tab** (or a click) insert it, **Esc** closes the list. It never opens
by itself inside a comment or a string. **Ctrl+Space** (Edit › IntelliSense › Complete) opens it
anywhere, also with nothing typed and in any language; **Auto Complete** turns the automatic list
off (saved). Inserting a suggestion is one ordinary edit: Undo takes it back.

**Go to Definition** (F12, also in Read › Navigate): jumps to where the name under the caret is
declared. The current document is searched first in its current, unsaved text; then the workspace,
definitions before declarations. On a declaration already (a prototype in a header), it goes to the
other place (the body in the source file). While the workspace is still being read, it waits and
jumps when it is ready.

**Ctrl+click** (Cmd+click on macOS): holding Ctrl over the code underlines the name under the mouse
and shows a hand when the workspace declares it; clicking goes to its definition, as F12 does from
there (also for names only this document declares, saved or not, which are not underlined). A
Ctrl+click on anything that is not a name places the caret as usual.

**Back and Forward** (Alt+Left and Alt+Right, also in Edit › IntelliSense and Read › Navigate): every
jump of Go to Definition or Ctrl+click remembers where it started; Back returns there, Forward goes
to the jump again. A document closed since is opened again from its file; an untitled one that
closed is skipped. A new jump drops the way forward. The last 50 places are kept each way, and they
are forgotten when the folder closes.

**On hover:** resting the mouse on a name for half a second shows its declaration and where it is.

**The index.** The workspace's code is read in the background the first time IntelliSense is used
in a folder (a list of suggestions, F12, or Ctrl held over the code; the project first, vendored folders after; build output, `node_modules` and files over
2 MB are skipped; at most 5,000 files). Saving a file updates its entries; renaming or deleting files
reads the workspace again on next use; **Rebuild Index** does so at once. Declarations are found by
reading the text (no compiler): the names are exact, the types of expressions are not known, so
members after `.` are those of any type.

**For AI completion.** A model plugs in as a provider (plan 21): it is asked with the code around
the caret and the context snippets IntelliSense chose for it, and its suggestions (which may span
several lines) are shown first in the same list. IntelliSense decides what context a model gets
(`contextFor()`: declarations matching the typed word, those the nearby code uses, those of the file,
ranked within a byte budget).

**Memory.** The index is the scope `intellisense` (docs/MEMORY.md); closing the folder or opening
another releases it to the byte, cancels a build still running and drops its late result. The
suggestion list is one window for every document.

