---
name: cross-platform-app
description: Build, port and polish one GUI app that runs well on Windows, Linux, macOS, the web and Android (phones and tablets, portrait and landscape). Use when creating a cross-platform desktop/mobile app, porting a desktop UI to phones or small windows, or fixing cross-platform problems such as UI drawn under Android's navigation bar, notch or keyboard; content that cannot be scrolled by touch, or a touch drag that moves the whole screen; text overlapping or cut off on narrow screens; dialogs larger than the screen; layouts that break on rotation; supporting older Windows (7/8) or 32-bit builds; CI matrices and toolchains for several platforms; or child processes that hang the UI. Especially relevant for immediate-mode UIs (Dear ImGui) but the rules are general. Also use to audit a UI on every form factor with screenshots.
---

# Cross-platform apps: one codebase, every screen

Hard-won rules from shipping one C++/Dear ImGui app on Windows (x64, x86, ARM64), Linux, macOS,
the web and Android. They apply to any toolkit; the ImGui-specific code is in `references/`.

## 1. Architecture: one view, thin platform shells

- **One UI for every platform.** Platform code is only the entry point (window, GL/EGL surface,
  input feed, clipboard, file dialogs) and an OS layer behind small interfaces (files, processes,
  shell, sockets, pty). No `#ifdef _WIN32` in views or view-models.
- **View ↔ view-model ↔ services.** Views draw and route input; view-models hold state and commands
  (testable headless); services wrap the OS and external tools. Platform differences live in the
  service implementations.
- **Lay out by size and shape, never by platform.** A phone, a narrow desktop window and a split-screen
  tablet are the same problem. Decide the arrangement every frame from the window size in density
  independent units (dp = px / scale):
  - room to spare → the desktop layout;
  - narrower than ~600 dp → **portrait phone**: one screen at a time with a bottom navigation bar;
  - shorter than ~500 dp (or narrow and wider than tall) → **landscape phone**: the same screens with a
    navigation rail on the left, so the content keeps its height.
  Rotation is then just a resize; nothing special happens.
- **A test-only window size flag** (`--window-size=390x844`, in dp) and **a pretend safe area**
  (`--safe-area=l,t,r,b`) let every phone layout be developed and screenshotted on a desktop.

See `references/responsive-layout.md` for the phone layouts in detail (top bar, navigation,
screen switching, which panels collapse) and the thresholds.

## 2. Safe area (Android especially)

- Android 15 (targetSdk 35) draws apps **edge to edge**: the window runs under the status bar, the
  3-button navigation bar, notches and the keyboard. NativeActivity's `contentRect` does **not**
  shrink for the navigation bar there, so UI ends up under the system buttons.
- Read the **window insets** in Java (`systemBars | displayCutout | ime`; older APIs:
  `getSystemWindowInset*` + `DisplayCutout`) and keep only the part that overlaps the app's own
  view (`android.R.id.content`, located with `getLocationInWindow`). That one formula is right both
  edge to edge and on older Android that already keeps clear of the bars — never subtract twice.
- Recompute on inset changes and layout changes (rotation, keyboard). Poll it from native every few
  frames through JNI; fall back to `contentRect` until known.
- The whole UI then lives inside that rect; the system areas just show the clear colour.

Code: `references/android-safe-area.md`.

## 3. Touch

- An immediate-mode UI usually scrolls only by wheel or scrollbar. Add **drag-to-scroll**: a finger
  that moves past ~8 dp pans the area under it, follows the finger 1:1 (catch up the threshold), and
  glides after release with exponential decay; the next touch stops it. Keep drawing frames while it
  glides.
- **Only scroll what the user may scroll**: skip windows with scrolling disabled (the app frame, fixed
  bars) — they can still have content past their edge (a tab row wider than the phone), and dragging
  them shifts the whole app. Scroll sideways only where a horizontal scrollbar is allowed. Walk up
  from the innermost window under the finger; lock to the dominant axis.
- **A pan cancels the press**, so a button under the finger does not fire when the user scrolls from
  it; a tap (no movement) still clicks. Text being edited keeps its own drag (selection); so do
  sliders and splitters.
- **Custom views** (a code editor) must not start a selection on a finger: act on release, and only
  if the press did not become a pan (tap = place caret, drag = scroll). A mouse keeps its old
  behaviour. Use the input source (touch vs mouse) the backend reports (ImGui: `io.MouseSource`).
- Rows too wide for a phone (ribbon tabs, toolbars) become their **own scrolling strip** with a thin
  scrollbar; when a tab is chosen, scroll it into view.
- Long-term fix: put drag scrolling in the toolkit itself (a small, versioned patch on a pinned
  version) instead of an app-side layer — see `references/touch-scroll.md`.

## 4. Narrow screens: nothing may overlap

The most common cross-platform bug: desktop layouts with fixed positions and widths. Rules:

1. **No columns pinned at fixed offsets from the right edge.** Lay columns out by priority from the
   available width, dropping the least important first (author → hash → time), and give the main
   text whatever remains — cut with an ellipsis, never drawn under or over a neighbour.
2. **Every text that can be long is either wrapped or ellipsized.** One-line messages
   ("Nothing to compare yet…") wrap. Labels in rows, badges and table cells ellipsize.
3. **Button and control rows wrap**: before each item, keep it on the line only if it fits
   (`sameLineIfFits`), else start a new line. Fixed-width inputs shrink to the room left
   (`width = clamp(avail - controls, min, wanted)`); a secondary field (a file filter) moves to its
   own line when narrow.
4. **Side-by-side panes stack below ~640 dp** (list above, details below; commits above, files below).
5. **Bars that hold wrapping rows size to their content** (auto-resize height), not a fixed row count.
6. **Tables**: on narrow widths disable secondary columns at setup (ImGui:
   `ImGuiTableColumnFlags_Disabled`; `TableSetColumnEnabled` needs `Hideable`), wrap the main
   column. **Measure the available width before `BeginTable`** — inside a table it measures the cell.
7. **Dialogs never exceed the screen.** A size set every frame (`SetNextWindowSize(440, 0)`) silently
   overrides any later shrink: wrap every fixed size in `windowSize(min(wanted, screen - margin))`,
   give auto-sizing dialogs a max-size constraint, and as a safety net clamp every floating window's
   position and size into the safe area each frame on phone layouts. Inside dialogs, fixed widths
   (a 190 px list, a 200 px column) become a fraction of the room.
8. **Hide what has no meaning on a phone** (maximize/close of a panel that is a whole screen, a side
   bar beside the editor) instead of squeezing it.

Helpers (`fit::text`, `buttonWidth`, `sameLineIfFits`, `itemWidth`, `windowSize`,
`constrainNextWindow`) and the window clamp: `references/fit-helpers.md`.

## 5. Audit every screen, every form factor

Do not trust one screenshot. Script it (`references/ui-audit.md`):

- Drive the real app through its automation/MCP connector at phone size, in a **fresh profile**
  (empty settings ⇒ sample/demo data, no session restore), one fresh process per screen (dialogs
  cannot always be closed by automation; restarting is reliable), hide the agent/automation pointer,
  capture the window content (not the screen — other windows may cover it), retry the flaky ones.
- Cover every screen, panel tab and dialog; stitch contact sheets to review quickly; produce
  before/after pairs for the user.
- Keep automated **UI tests** at phone sizes too: sweep every screen for ID conflicts / usage errors,
  and behaviour tests with a simulated finger (drag scrolls ≈ the drag distance; a tap switches
  screens; a drag from a button does not press it; a sideways drag on a too-wide row never moves the
  app frame; a mouse drag still selects).

## 6. Builds and platforms

- **CMake presets** for every target (MSVC x64/ARM64/x86, MinGW x64/x86, Linux native + cross, macOS
  universal, Emscripten, Android via Gradle using the same CMakeLists). Outputs to
  `bin/<os>/<arch>/`. CI builds and tests the matrix; keep actions current (Node 24 based majors).
- **Old Windows**: define `_WIN32_WINNT=0x0601` for all Windows code so nothing newer than Windows 7
  is declared or linked by accident; look newer functions up at run time with a fallback
  (`GetProcAddress` for per-monitor DPI, the pseudo console); verify with
  `objdump -p app.exe` that no import is newer than the target. Costs bytes, not megabytes.
- **32-bit MinGW**: a 64-bit-only toolchain cannot `-m32`; use a separate i686 toolchain (e.g.
  w64devkit's x86 edition) via a toolchain file. If its `bin` is not first on PATH, pass
  `-B<toolchain>/bin/` so GCC finds its own assembler and linker.
- **Pin third-party versions** (the submodule commit is the lock; drop `branch =` from
  `.gitmodules`); keep local patches as patch files applied to a build-dir copy, never edits inside
  the submodule.
- **Line endings**: with `core.autocrlf`, files may flip to CRLF after a checkout or merge; scripted
  edits must handle both.

Details and pitfalls: `references/builds-and-platforms.md`.

## 7. Processes never block the UI

- Never run a child process on the UI thread (even a "quick" `git diff`). Run it on a worker and post
  the result back; for request/response connectors, let the handler return a "run this" description
  that the connection thread executes.
- **Cancellation must kill the whole tree**: Windows job object with
  `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` (create suspended, assign, resume); POSIX `setpgid(0,0)` in
  the child and `kill(-pid, SIGKILL)`. Wait in short slices that check a cancel flag. Closing the
  app cancels everything still running.
- Executable lookups that may hit network folders (searching PATH) run in the background too.

## Checklist before calling a UI "cross-platform"

- [ ] Layout chosen by dp size/shape; phone portrait, phone landscape and desktop all tested (rotation = resize).
- [ ] Safe area from real insets; nothing under bars, notch or keyboard.
- [ ] Every scrollable area drags by finger; nothing else moves; taps still click.
- [ ] Audit screenshots of every screen and dialog at ~390×844 and ~844×390 dp: no overlap, no clipping, every control reachable.
- [ ] Dialogs fit the screen; fixed widths wrapped.
- [ ] UI tests at phone sizes in CI; builds for every target; oldest supported OS checked by import list.
- [ ] No child process on the UI thread; cancellation kills the tree.
