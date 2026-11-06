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
explorer tree that marks open and unsaved files and follows the active one (dot files and folders
such as `.github` show too; turn them off in Preferences > Explorer or the folder's right-click menu;
`.git` and build output stay out), find and replace, bookmarks, line operations,
multiple encodings and line endings, word wrap, zoom, and an **integrated terminal** with your real
shell (PowerShell, cmd, Git Bash, WSL, bash, zsh) in a VS Code-style bottom panel. Full-screen
terminal programs such as `top`, `htop` and `vim` work (xterm-compatible emulator, alternate
screen, colours, mouse-free keys), and the terminal can **pop out** into its own window.

**VS Code editing keys**: several carets at once (**Alt+click** adds one, **Ctrl+Alt+Up/Down** adds
one above or below, **Shift+Alt+drag** or a middle-button drag selects a column), **Ctrl+D** adds the
next occurrence of the selection, **Ctrl+Shift+L** selects every occurrence, **Shift+Alt+I** puts a
caret at the end of each selected line; typing, Backspace, Delete, Enter, Tab, paste and the line
commands act at every caret, and Escape goes back to one. **Alt+Up/Down** moves lines,
**Shift+Alt+Up/Down** copies them, **Ctrl+Shift+K** deletes them. **Ctrl+R** switches to a recent
folder or file, **Ctrl+E** jumps to the terminal (and back), **Ctrl+W** closes the file and
**Ctrl+Tab** goes to the next tab.

Markdown files open **rendered** (Ctrl+Shift+V or a double-click to edit the text): headings,
emphasis, code with syntax colours, lists and task lists, quotes, tables, links, **images**
(`![alt](path)`, `<img src>`, data URLs; PNG, JPEG, GIF, BMP, TGA, PSD, PNM), HTML character
references (`&copy;`, `&rarr;`, `&#x3B1;`), and **LaTeX math**: `$...$` and `\(...\)` inline,
`$$...$$` and `\[...\]` as display blocks, with fractions, roots, scripts, sums and integrals
with limits, Greek letters, the usual symbols and relations, `\left(...\right)`, `\text{}`,
accents, `\binom`, and several lines: `\\` and `&`, `aligned`, `cases`, the matrices and `array`;
a display formula too long for the page breaks before an operator. Images are decoded once, shared by the documents that show them, and freed with the
last one.

**Emoji** render in colour in rendered Markdown and the agent chat, following Unicode's emoji rules
(UTS #51): every recommended emoji of Unicode 18.0, including skin tones, ZWJ sequences
(👩‍🔬, ❤️‍🔥), keycaps (#️⃣), country and subdivision flags, and `U+FE0F` / `U+FE0E` choosing the
emoji or the text form (⬇ stays a text arrow, ⬇️ is an emoji), however they are written: the
characters themselves or `&#x2B07;&#xFE0F;`. A sequence without a picture of its own shows its
parts side by side. The pictures are the 72 px Noto Emoji images (Apache 2.0; flags public
domain) in one uncompressed pack, `resources/emoji/noto-emoji-72.zip`, rebuilt by
`scripts/emoji/build_emoji_pack.py`; only its index and the emoji on screen are held in memory.

**Data files have views of their own**, switched with the icons at the top right (or Ctrl+Shift+V):
**CSV/TSV** opens as a **table** (delimiter detected, quoted fields and line breaks in quotes, the
first row as header or not, columns that resize and sort by clicking their header, numbers in
number order, only the rows on screen drawn, so large files stay quick). **JSON and YAML** open
as text and switch to a **tree** (keys on the left, the chosen value on the right with its path,
type and line, its children listed, "Show in text" to jump to it; arrow keys move) or an
**accordion** (nested sections that open and close, long lists 200 at a time). A file that cannot
be read shows where it fails and takes you to the line. The parsed table or tree exists only while
a view shows it: it is freed when the document closes or goes back to text.

Files are **checked before they are loaded**: the content (magic numbers, NUL bytes) decides, the
extension only hints. Images open in an image view (fit or actual size, transparency shown on a
checkerboard): PNG, JPEG, GIF, BMP, TGA, PSD, PNM, Radiance HDR, QOI and ICO/CUR. Formats it
recognises but cannot decode yet (WebP, TIFF, AVIF, HEIC, JPEG 2000) and binary files
(executables, archives, PDFs, media, fonts, databases, models) are not poured into a text buffer:
the editor says what the file is and offers **Open as text anyway**. Saving is refused for a
file that was not opened as text, so the empty view can never overwrite it. A hex view for binary
files is planned.

**Libraries and programs show what they contain**: Linux shared libraries, executables and objects
(ELF, 32/64-bit, either byte order), Windows DLLs, executables and objects (PE/COFF, including
/bigobj), and static libraries (`.a`, Windows `.lib`, import libraries included). A tree on the
left lists the **overview** (type, machine, entry point, SONAME or export name, interpreter, build
ID or PDB, and hardening: PIE/ASLR, non-executable stack/DEP, RELRO, Control Flow Guard, stack
protector, signature), the **sections**, the **exported functions** and **data**, the **imports
per library** (versioned ELF imports by the library they were linked against; PE imports and
delay-loaded imports per DLL), the internal symbols when the file is not stripped, the
**dependencies**, and an archive's **members**. The chosen list is a table that sorts and filters:
name, address or RVA, file offset, size, section, and per platform the binding and symbol version
(ELF) or the ordinal, hint and forwarder (PE). **Mangled C++ names** (GCC/Clang and MSVC), Rust and
D names are demangled with LLVM's demangler (`3rdparty/llvm-demangle`) and carry a tag saying so;
the name as stored is in the tooltip, the details below the table, a column, or everywhere with
"Demangle names" off. A name that is mangled but could not be demangled is marked too. The file
is read in pieces, and what was read is freed when the document closes or is opened as text.

The app draws only when something changes, and caps how often it redraws (Preferences >
Performance), so an idle editor costs next to no CPU, even without a GPU. Closing a folder or the
last document frees what they used and hands the memory back to the OS right away, so the release
shows in a task manager instead of staying with the process for reuse. Every heap allocation is
accounted to its subsystem (`docs/MEMORY.md`): the developer-mode status bar and the automation
`get_state` reply show what each still holds, and the unit tests fail if an open/close cycle keeps
a byte.

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

**Customize Ribbon** (the sliders button after the tabs) has three sections. **Tabs**: tick the tabs
to show, untick the ones you do not need (Read and Review, say), and change their order by dragging
or with the arrows; each workspace keeps its own arrangement, and **Restore default tabs** brings
back the original. **Pinned commands**: what My Tools shows, in your order. **Custom actions**: your
shell-command buttons. Right-click a tab on the ribbon to hide it, move it, or restore the
defaults. Everything is remembered.

## Connect an agent (MCP)

Start EasyNotepad, then register the connector with your MCP client as a stdio server:

```sh
# Claude Code
claude mcp add easynotepad -- "/path/to/EasyNotepad" --mcp
```

The connector is a command-line program: `EasyNotepad --mcp`. Its tools include `get_state`,
`list_elements`, `click`, `point`, `open_file`, `read_document`, `set_language`, `search_files`, `read_terminal`, `send_keys`, `read_outline`, `set_detail_level`,
`execute_command`, `type_text`, `run_in_terminal` and `screenshot`. `screenshot` returns a PNG of the app
window as drawn now, whatever covers it on screen and even while it is minimized (the app draws the frame
again off screen and reads it back; `path` also saves it). It talks to the running app over a local-only
endpoint protected by a per-session token. Start the app with `--no-automation` to turn it off.

## Open a folder

`EasyNotepad <folder> [files...]` opens a folder as the workspace (dropping a folder on the window
works too); files are read from and saved to disk. Without a folder, a small sample project is shown.

**File > Open Workspace** imports a VS Code workspace (`.code-workspace`) or a Visual Studio solution
(`.sln`, `.slnx`); they can be given on the command line too. See [docs/FEATURES.md](docs/FEATURES.md).

## Building

```sh
git clone --recursive <repo-url>
scripts/build.sh --test --run     # build, run the tests, launch
```

Binaries go to `bin/<os>/<arch>/`, and Android APKs to `bin/android/`. CMake presets cover Windows (MSVC x64/ARM64/x86, MinGW x64/x86),
Linux (native and aarch64 cross), macOS (universal) and the web; `android/` is a Gradle project.

The Windows build runs on Windows 7 and later, 64- or 32-bit. The built-in terminal needs Windows 10
1809 or later, and the app needs OpenGL 3.0 graphics drivers. For a 32-bit build with MinGW, install
the x86 edition of [w64devkit](https://github.com/skeeto/w64devkit/releases) at `C:\w64devkit-x86`
(or set `W64DEVKIT_X86` to its folder), then:

```sh
cmake --preset windows-mingw-x86 && cmake --build --preset windows-mingw-x86   # bin/windows/x86/
```

The on-device engine is llama.cpp, a submodule pinned to one commit (`3rdparty/llama.cpp`). The
build never uses it as checked out. CMake exports that commit into the build folder and applies
`patches/llama.cpp/*.patch` there (see [patches/README.md](patches/README.md)). The engine is in
64-bit builds only; `-DENP_LOCAL_ENGINE=OFF` leaves it out.

The agent CLI used for dissection is set with `EASYNOTEPAD_AGENT_CLI` (default `claude -p`); it
gets the prompt on stdin and answers with JSON.

Launch options: `--workspace=agent|code`, `--detail=Full|Balanced|Overview|Outline`,
`--panel=terminal`, `--tour`, `--native-titlebar`, `--no-viewports` (popups and the popped-out
terminal stay inside the main window), `--no-automation`.

Tracking down a leak: turn on developer mode (seven taps on the version in About) and the status
bar shows what the process holds (memory, handles, GDI and USER objects, threads, Dear ImGui
allocations and windows), refreshed every second; the automation `get_state` reply carries the
same numbers. Whichever count climbs while the app idles names the culprit.

## Architecture

MVVM in layers: **os** (files, sockets, pseudo-terminals) → **model** (text, outline, terminal
emulator, diff) → **interfaces / services** → **view-models** → **view** (ImGui). Services are
**command-line based**: they wrap CLI tools (shells, linters, the agent) behind `I*Service`
interfaces. Unit tests and a headless UI test (ImGui ID conflicts and usage errors) run in CI.
