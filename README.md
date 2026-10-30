# EasyNotepad

A natively compiled text editor written in C++17 with [Dear ImGui](https://github.com/ocornut/imgui), OpenGL
and GLFW, inspired by Notepad++ (see `ref/`, not part of the build). One code base targets
**Windows, Linux, macOS, Android and the Web (WebAssembly)** on **x86_64 and aarch64**.

The menu system is a flat ribbon banner: a **Workspace** block on the left, tabs with a command search box,
and groups of large icon buttons with captions (a caption with a caret opens more commands).

> **Status: UI preview.** The view and view-models run on in-memory sample data (`DummyData.cpp`).
> No services are connected yet: files are not read or written, the agent replies are canned, and the
> review tools return fixed findings.

## Title bar

The window draws its own title bar (logo, File menu, quick actions, document tabs, workspace switch,
window buttons), so the chrome matches the theme.

| Platform | How |
|----------|-----|
| Windows | native frame kept, caption removed (`WM_NCCALCSIZE`) and the empty title-bar areas reported as caption (`WM_NCHITTEST`): Snap layouts, double-click to maximize, Win+arrows, edge resize and the shadow stay native |
| Linux X11 | undecorated window; move, double-click maximize and edge resize implemented in `GlfwWindowFrame` |
| macOS, Wayland, GLFW < 3.4 | native title bar kept |

Start with `--native-titlebar` to keep the OS title bar everywhere.

## Workspaces

| Workspace | Layout | For |
|-----------|--------|-----|
| **Agent** | plan & proposed changes · conversation · preview (side-by-side diff or editor) · review | describing a goal, letting the agent edit, reviewing and accepting/rejecting each change |
| **Code**  | explorer · editor · agent side panel · review | manual editing in the classic Notepad++ style, asking the agent about the code at hand |

Switch with the Workspace block, the Agent/Code buttons in the title bar, the status-bar badge, or
`Ctrl+Shift+M`. Start in a given workspace with `EasyNotepad --workspace=agent` (or `=code`).

## Code review tools

- **Analyzers**: Lint, Static Analysis, Security, Style, Complexity, plus an **AI review** by the agent.
  Findings appear in the Problems list (filter by tool, errors first, mark resolved), as gutter markers
  and squiggles in the editor, and as counts in the explorer.
- **Diff**: side-by-side comparison against the saved file (*Vs Saved*) or the agent's proposal (*Changes*).
- **Comments**: inline review comments on a line (`Ctrl+Alt+M`), shown in the gutter and the Comments tab.
- **Checklist**: a per-review checklist with progress.

## Editor

Tabs, line numbers, current-line highlight, syntax colouring (C/C++, Python, JavaScript, JSON, CMake,
Markdown), selection with mouse and keyboard, smart Home, word movement, auto-indent, undo/redo,
find/replace with match highlighting, bookmarks, line operations (duplicate, delete, move, sort, case,
comment, indent), word wrap, whitespace and indent guides, zoom, line-ending / encoding / language
selection from the ribbon or the status bar.

## Repository layout

```
include/enp/          public headers, one folder per layer
  os/                 OS abstraction layer (fs, dialog, shell, dispatch, log, platform)
  model/              TextBuffer, Document, Diff, Review, Agent data
  viewmodel/          NotepadViewModel, AgentViewModel, ReviewViewModel, commands, dummy data
  view/               MainView, RibbonView, EditorView, AgentPanel, ReviewPanel, DiffView, theme
src/                  layer implementations; app/ holds Application and the platform entry points
resources/fonts/      Roboto, Cousine, Font Awesome 6 (shared by all platforms)
android/ web/         Gradle project (NativeActivity) and the WebAssembly host page
tests/                unit tests for the model and view-models
3rdparty/             imgui and IconFontCppHeaders (submodules), glad
ref/                  reference sources (Notepad++), git-ignored
```

File naming: one class per file, PascalCase (`TextBuffer.cpp`, `NotepadViewModel.h`); service contracts
will use an `I` prefix (`IDocumentService.h`).

## Architecture (MVVM)

| Layer         | Depends on        | Responsibility                                          |
|---------------|-------------------|---------------------------------------------------------|
| **os**        | operating system  | files, dialogs, shell, threads, logging                 |
| **model**     | –                 | text buffer, documents, diff, review and agent data     |
| **viewmodel** | model             | presentation state (`Property`), `Command`s, events     |
| **view**      | viewmodel, ImGui  | immediate-mode rendering and input                      |

Next step: an `interfaces/` layer (`IDocumentService`, `ISettingsService`, `IAgentService`,
`IReviewService`, …) and `services/` implementations on top of the OS layer, injected into the
view-models in place of `DummyData`.

## Getting the code

```sh
git clone --recursive <repo-url>
# or, in an existing clone:
git submodule update --init --recursive
```

GLFW is used from the system if installed, otherwise CMake downloads GLFW 3.4.

## Building

```sh
scripts/build.sh                 # native build, Release (Linux, macOS, Git Bash/MSYS2 on Windows)
scripts/build.sh --test --run    # build, run unit tests, launch
scripts/build.sh web             # WebAssembly (activated emsdk required)
scripts/build.sh android         # APK (Gradle + Android SDK required)
```

Plain CMake works on every desktop platform:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

Presets: `windows-x64`, `windows-arm64` (MSVC, newest installed Visual Studio, so they keep working when
CI images move to a new Visual Studio release), `windows-mingw`, `linux`, `linux-arm64-cross`,
`macos-universal`, `web`. Platform prerequisites are the same as MDViewer's (Linux: GL/X11/Wayland dev
packages; macOS: `brew install cmake ninja`).

## Shortcuts

| Keys | Action | Keys | Action |
|------|--------|------|--------|
| `Ctrl+N` / `Ctrl+S` / `Ctrl+W` | new / save / close | `Ctrl+F`, `Ctrl+H`, `F3` | find, replace, next |
| `Ctrl+Z` / `Ctrl+Y` | undo / redo | `Ctrl+G` | go to line |
| `Ctrl+D` | duplicate line | `Ctrl+Shift+L` | delete line |
| `Ctrl+Shift+Up/Down` | move lines | `Ctrl+Q` | toggle comment |
| `Ctrl+F2`, `F2` | bookmark, next bookmark | `Ctrl+=`, `Ctrl+-`, `Ctrl+0` | zoom |
| `F7`, `Ctrl+Shift+F7` | lint, run all reviews | `F8` / `Shift+F8` | next / previous finding |
| `Ctrl+I` | ask the agent | `Ctrl+Shift+M` | switch workspace |
| `Ctrl+PageDown/Up` | next / previous tab | `Ctrl+Shift+E/R/I` | explorer / review / agent panel |
