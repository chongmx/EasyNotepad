# EasyNotepad

A native code editor built around **reading** code, with an **AI agent as a first-class
collaborator**. Written in C++17 with Dear ImGui, OpenGL and GLFW; one code base for Windows, Linux,
macOS, Android and the web, on x86_64 and aarch64.

> **Status: early preview.** Files, the terminal and the agent connector are real. AI summaries,
> the agent's replies and the review tools still use placeholders until their command-line
> services are connected.

## Read code at the level of detail you want

- **Detail levels**: Full, Balanced, Overview and Outline. Code folds into **one-line AI summaries**
  of what each function or block does, so you read intent first and code second.
- **Fold anything** from the gutter, the ribbon or the keyboard. A folded region is a summary pill;
  click it to expand.
- **Layers** beside the editor: the code as nested parts you zoom into, each with its AI summary.
  Scope it to the **file** (namespaces, types, functions, blocks, sections) or the whole **project**
  (folders, files, then their parts); pick **Compact**, **Summary** or **Detailed** cards; zoom in with
  a card's chevron, out with the breadcrumb; click a card to jump to that code.
- **Logical folding**: besides the usual syntax folding, functions fold into their **steps**
  (the paragraphs of the function, titled by their comments), so a long function reads as a short
  list of what it does.
- Prefer the classic view? Switch the side bar to a **minimap** of the whole file, or hide it.

## See the program, not the files

- **Logical map**: the project dissected into what it *does*: system, modules, components,
  functions and their steps, with the "uses" and "calls" relations between them. Switch the editor
  from **Code** to **Map** and the code becomes diagrams: layered boxes with summaries and arrows.
  Click to zoom in, down to a function's flow of steps; the bottom level opens the code. Going back
  to the map shows where your caret is.
- **Agent dissection**: one click asks your agent CLI (`claude -p` by default) to refine the map:
  better summaries and responsibilities, plus a **Features** lens with the program's user-facing
  features, each a flow through the components. The result is cached in the project
  (`.easynotepad/logical-map.json`).
- The same map is available as the **Logic** scope of the Layers side bar.

## Work with an agent

- **Two workspaces.** *Agent*: describe the goal, and the agent plans, edits and runs the reviews, while you
  accept or reject each change in a side-by-side diff. *Code*: classic editing, with the agent in a side panel.
- **Agent cursor.** The agent has its own glowing cursor. It can point at the control you need,
  explain what it does, give you a **tour**, or click things for you, all without touching your
  mouse or keyboard.
- **MCP connector.** Any MCP-capable agent can read and operate the running editor: state, on-screen
  elements, documents and outlines; click, point, run commands, type, use the terminal.

## Review code

Lint, static analysis, security, style and complexity checks plus an AI review, collected in one
Problems list with gutter markers and squiggles; side-by-side diffs against the saved file or the
agent's proposal; inline review comments; a review checklist.

## Everyday editing

A Notepad++-style editor: tabs that stay readable however many files are open (they scroll, show
the folder when names clash, reorder by dragging, and list everything in an overflow menu), an
explorer tree that marks open and unsaved files and follows the active one, find and replace, bookmarks, line operations,
multiple encodings and line endings, word wrap, zoom, and an **integrated terminal** with your real
shell (PowerShell, cmd, Git Bash, WSL, bash, zsh) in a VS Code-style bottom panel. Full-screen
terminal programs such as `top`, `htop` and `vim` work (xterm-compatible emulator, alternate
screen, colours, mouse-free keys), and the terminal can **pop out** into its own window.

The app draws only when something changes, and caps how often it redraws (Preferences >
Performance), so an idle editor costs next to no CPU, even without a GPU.

## Languages and colours

Syntax colouring and folding for **C/C++, C#, Java, JavaScript, TypeScript, Python, Rust, Go, Lua,
Shell, PowerShell, SQL, HTML, XML, CSS, JSON, YAML, TOML/INI, CSV/TSV, CMake and Markdown**,
compiled in and driven by one language table (adding a language is adding an entry). Folding
follows each language: braces, indentation, `if`/`endif`-style keywords, tags, Markdown sections and
code fences, `[sections]`, and `#region` markers everywhere. CSV columns each get their own colour.

**Smart indentation** follows the language too: Enter indents after `{`, `(`, `[`, a Python or YAML
`:`, an open HTML tag or `if(` / `function` / `then` / `do`; typing `}`, `</`, `endif(`, `end`, `fi`
or `else:` moves the line back to its opener; Enter between braces puts the closer on its own line;
doc comments continue with ` * `; Markdown lists continue (and end on an empty item). Each file keeps
its own style, detected from its content (2 or 4 spaces, tabs), shown and changed in the status bar,
with a saved default for new files (for example 2 spaces everywhere).

Every colour is yours to change: **Colours** (View tab, or the language menu in the status bar)
recolours keywords, strings, comments, keys, tags, columns and the rest, for all languages at once
or for one language, separately for the dark and light themes, with a live preview. Your colours
are saved in `settings.json` in the app's data folder.

## A ribbon you can make your own

Commands are organised in a flat ribbon (Home, Read, Edit, Review, View, Document, Tools; in the Agent
workspace: Agent, Read, Review, Files, View) with a command search box. **My Tools** holds the
commands you pin and **custom actions**: buttons that run your own shell commands in the terminal.

## Connect an agent (MCP)

Start EasyNotepad, then register the connector with your MCP client as a stdio server:

```sh
# Claude Code
claude mcp add easynotepad -- "/path/to/EasyNotepad" --mcp
```

The connector is a command-line program: `EasyNotepad --mcp`. Its tools include `get_state`,
`list_elements`, `click`, `point`, `open_file`, `read_document`, `set_language`, `search_files`, `read_terminal`, `send_keys`, `read_outline`, `set_detail_level`,
`execute_command`, `type_text` and `run_in_terminal`. It talks to the running app over a local-only
endpoint protected by a per-session token. Start the app with `--no-automation` to turn it off.

## Open a folder

`EasyNotepad <folder> [files...]` opens a folder as the workspace (dropping a folder on the window
works too); files are read from and saved to disk. Without a folder, a small sample project is shown.

## Building

```sh
git clone --recursive <repo-url>
scripts/build.sh --test --run     # build, run the tests, launch
```

Binaries go to `bin/<os>/<arch>/`. CMake presets cover Windows (MSVC x64/ARM64/x86, MinGW x64/x86),
Linux (native and aarch64 cross), macOS (universal) and the web; `android/` is a Gradle project.

The Windows build runs on Windows 7 and later, 64- or 32-bit. The built-in terminal needs Windows 10
1809 or later, and the app needs OpenGL 3.0 graphics drivers. For a 32-bit build with MinGW, install
the x86 edition of [w64devkit](https://github.com/skeeto/w64devkit/releases) at `C:\w64devkit-x86`
(or set `W64DEVKIT_X86` to its folder), then:

```sh
cmake --preset windows-mingw-x86 && cmake --build --preset windows-mingw-x86   # bin/windows/x86/
```

The agent CLI used for dissection is set with `EASYNOTEPAD_AGENT_CLI` (default `claude -p`); it
gets the prompt on stdin and answers with JSON.

Launch options: `--workspace=agent|code`, `--detail=Full|Balanced|Overview|Outline`,
`--panel=terminal`, `--tour`, `--native-titlebar`, `--no-automation`.

## Architecture

MVVM in layers: **os** (files, sockets, pseudo-terminals) → **model** (text, outline, terminal
emulator, diff) → **interfaces / services** → **view-models** → **view** (ImGui). Services are
**command-line based**: they wrap CLI tools (shells, linters, the agent) behind `I*Service`
interfaces. Unit tests and a headless UI test (ImGui ID conflicts and usage errors) run in CI.
