# 22 · Drag-to-scroll inside Dear ImGui (a maintained patch on a locked version)

**Problem.** ImGui scrolls only with a mouse wheel or by grabbing the scrollbar. On a touch screen
nothing scrolls by dragging the content. The app works around this in `view/TouchScroll.cpp`:
- every frame it looks at the window under the finger, scrolls it by hand, and cancels the press;
- the editor has its own rule ("a finger's drag scrolls, a tap places the caret").

That works, but it sits outside ImGui:
- it cannot know which widgets use a drag themselves (sliders, splitters, text selection);
- it reaches into ImGui internals from the outside;
- every new view must remember the rules.

The root of it is that ImGui has no notion of drag scrolling. The fix belongs in ImGui.

**Also:** the ImGui version is not truly locked. `.gitmodules` names a branch (`docking`), so
`git submodule update --remote` would silently move it. It also says `ignore = dirty`, which would
hide edits made inside the submodule.

## Where it stands (checked 15 Nov 2026)

Drag-scrolling works today, in every content window, on Android and on Windows touch screens. It is
built in the app (`view/TouchScroll.cpp`, about 350 lines), not in ImGui. Since this plan was
written it grew a gesture layer: on Android the finger's press is held back from ImGui until the
gesture is known, so a drag never clicks.

**What the app-side version already does (the plan's behaviour goals):**

| Goal | State | How |
|---|---|---|
| A finger drag scrolls any window or child that can scroll, 1:1 | Done | `update()`: the innermost window under the finger that scrolls along the drag's main axis (or the other axis, when only that one scrolls); the content catches up from where the finger went down |
| It glides after release | Done | velocity decays (about 4 per second), stops at the ends |
| A tap is a click; a drag never is | Done | Android: `fingerLanded/Moved/Lifted` hold the press back; a finger that lifts within the slop (8 px) is a press and release where it lifted; a pan presses nothing. Other touch screens: the press is cleared (`ClearActiveID`) when a pan starts |
| Windows that do not scroll are not shifted | Done | `userScrollable()`: only windows a wheel or scrollbar could scroll |
| Text being edited keeps its drag (selecting) | Done | the active `InputText` under the finger gets the press |
| Sliders and splitters keep their drag | **Partly** | by direction, not by asking the widget: a drag that nothing under the finger can scroll that way gets the press. A horizontal slider in a window that scrolls only vertically works. A slider or splitter in a window that scrolls the same way as the drag is panned instead |
| Pinch to zoom; two fingers never click | Done (beyond the plan) | `pinch()`, `zoomView()`, `takeZoom()` for the editor, Markdown and the document viewer |
| The editor's tap rule | Done | `EditorView` asks `touch::moved()` |
| Tested | Partly | UI tests: the editor scrolls and glides, a navigation tap, a list dragged without opening a file, the Preferences tab row, swipes, a pinch about the fingers on a 120-page PDF. **Not tested:** a finger drag on a slider, on a splitter, or selecting in an `InputText` |

**What is not done (the plan's mechanism):**
- **Nothing is in ImGui.** No patch, no `io.ConfigDragScroll*`, no per-window or per-item opt-out
  (`NoDragScroll`), no `IsDragScrolling()`. The submodule is unmodified upstream ImGui
  (`v1.93.0-docking-42`, `d682dc41f`).
- **The lock.** `.gitmodules` still names `branch = docking` and `ignore = dirty`; there is no
  `patches/imgui/BASE` and no configure check for ImGui. (CI checks out the recorded commit, so CI
  builds are pinned in practice.)
- **The patched-copy build exists, for llama.cpp only.** `cmake/PatchedSource.cmake`
  (`enp_patched_source`: checks `BASE`, exports the commit, applies `patches/<name>/*.patch`) was
  built for plan 23 and names this plan; ImGui does not use it yet. Step 2 below is therefore mostly
  done: ImGui only needs to be routed through it.
- No pen or mouse "hand tool", no chaining from a child at its end to its parent, no
  `scripts/imgui-update.sh`.

**What this changes in the plan.** The user-facing problem is solved. What remains is (a) the
lock, which is cheap and worth doing now, (b) the one behaviour gap (a widget that drags the same
way its window scrolls), and (c) whether to move the mechanism into ImGui at all. Two ways on:

1. **Keep it in the app** (smaller): do the lock (step 1); add an item-level opt-out in the app
   (an id list `touch::claimDrag(id)` that splitters and sliders call, checked before a pan
   starts, using `g.HoveredIdPreviousFrame`); add the missing tests. No ImGui patch to rebase on
   upgrades.
2. **Move it into ImGui** as planned below (larger): steps 1 to 5, with `TouchScroll`'s gesture
   rules (press held back until known, slop, pinch) carried into the patch, and the existing touch
   tests as the check that nothing changed.

Recommended: 1 now, and 2 only if upstream ImGui shows interest in taking the patch.

## Done, and why this plan is kept here (kiv: keep in view)

Way 1 was done on 15 Nov 2026:
- **The lock.** `patches/imgui/BASE` names the commit (`d682dc41f`, v1.93.0-docking-42);
  `cmake/PinnedSource.cmake` (`enp_pinned_source`) fails configuring when the submodule is at
  another commit (`-DENP_ALLOW_UNPINNED=ON` for experiments). `.gitmodules` no longer names a branch
  or says `ignore = dirty` for ImGui. No patched copy is made: ImGui needs no patch.
- **Per-item claims.** `touch::claimDrag(ClaimX | ClaimY)` after an item that moves with a drag: a
  finger that lands on it and drags along a claimed axis moves the item, even in a window that
  scrolls that way; the other axis still scrolls. Called by the sliders (Preferences, Engine Lab),
  the splitters (columns, panels, side bar, the data and library trees), the blame column's edge,
  the Markdown ruler, the minimap and the map canvas.
- **Tests.** A UI test drags a finger on a slider, a splitter and text being edited, in a window
  that scrolls both ways.

Way 2 (moving the mechanism into ImGui) is not planned. This file stays for that case: if upstream
ImGui takes drag scrolling, or the app-side rules stop being enough.

## Goal

- ImGui itself scrolls a window when a finger (or, optionally, a pen or mouse) drags its content.
  - It follows the finger exactly and glides after release.
  - It works for every window and child that can scroll, unless the window or an item opts out.
- **Widgets that use a drag keep it:** sliders, drags, text selection, splitters, resize grips,
  docking and window moving.
- **A tap is still a click; a drag never is.**
- The change is a small set of patch files kept in this repository, applied to an exact, recorded
  ImGui commit at build time. The submodule itself is never edited.
- Upgrading ImGui is a deliberate, scripted step: move the commit, re-apply, fix, test.

## The patch

**Public API (`imgui.h`), all additive:**
- `io.ConfigDragScrollSources`: a mask of `ImGuiMouseSource` values that drag-scroll. Default: the
  touch screen only. Pen and mouse can be added, e.g. for a "hand tool".
- `io.ConfigDragScrollMomentum` (default true): the glide after release.
- `io.ConfigDragScrollFriction` (default about 4 per second).
- **Per window:**
  - `ImGuiChildFlags_NoDragScroll`, for children;
  - `ImGui::SetNextWindowDragScroll(bool)`, for any window, before `Begin`.
  Windows drag-scroll by default when they can scroll; these turn it off.
- **Per item:** `ImGuiItemFlags_NoDragScroll` (push / pop) and `ImGuiButtonFlags_NoDragScroll`.
  An item that owns the drag (a custom splitter, a canvas) keeps the press instead of turning it
  into a scroll.
- `ImGui::IsDragScrolling()` (a drag scroll is moving content now) and
  `ImGui::WasDragScrolled()` (the last press became a scroll, so its release is not a tap). These
  are for views with their own pointer handling, such as our editor.

**Core (`imgui.cpp`):**
- A new `UpdateDragScroll()` in `NewFrame()`, right after `UpdateMouseWheel()`, with its state in
  `ImGuiContext::DragScroll`:
  - the target window, the axis, the start, and the velocity for the glide.
- **Start:** a press from an enabled source, then movement past `io.MouseDragThreshold`.
  - The axis is the dominant one, or the other if only that one can scroll.
  - The target is the innermost window under the press that can scroll along that axis and has
    not opted out, walking up through parents.
- **Skipped when:**
  - the active item claims drags (the flags above, or the built-in widgets below);
  - the press is on a scrollbar, a resize grip or a title bar;
  - a window is being moved or docked.
- **On start:** the active item is released without a click (`ClearActiveID()`). Its release
  therefore no longer counts as a click, through the same path ImGui uses when an item loses
  activation.
- **While dragging:** the scroll moves 1:1 with the pointer (the threshold distance is caught up),
  clamped to the window's range.
- **After release:** the glide decays with friction, stops at the ends, and stops at the next
  press. While it runs, a frame request flag lets the app's idle loop keep drawing.
- **Nested windows:** scrolling chains to the parent when the inner window is at its end on that
  axis (optional, behind a flag).

**Widgets (`imgui_widgets.cpp`).** These claim the drag, so a drag on them never scrolls:
- `InputText` (selecting text);
- `SliderBehavior`, `DragBehavior`;
- `SplitterBehavior`;
- window resize grips and moving, and docking. These are in `imgui.cpp`, and are mostly scrollbar
  and title-bar exclusions.

**Size.** Roughly 150–250 changed lines. Nearly all of it is the new function and the state struct;
the rest is a few one-line hooks. It is kept small and readable so it can be offered upstream
(ImGui has long-standing interest in touch scrolling). If upstream adds its own, we drop ours.

## Where it lives and how it is applied

The request was a patch folder under `resources/`. Two cautions about that:
- `resources/` is copied next to every binary and packed into the Android APK, so the patches would
  ship to users.
- The plan therefore uses `resources/patches/imgui/` and excludes `resources/patches/` from both
  copies (the CMake resource copy and Gradle's `assets.srcDirs`).
- The alternative is a top-level `patches/imgui/`, which needs no exclusions. Recommended, if that
  is acceptable.

```
resources/patches/imgui/
  BASE                        # the exact ImGui commit and its version string
  0001-drag-scroll-api.patch  # imgui.h, imgui_internal.h: flags, config, state, declarations
  0002-drag-scroll-core.patch # imgui.cpp: UpdateDragScroll, NewFrame hook, exclusions
  0003-drag-scroll-widgets.patch  # imgui_widgets.cpp: InputText / Slider / Drag / Splitter claim the drag
  README.md                   # what each patch does, how to rebase them
```

**Build.**
- CMake (desktop, web, and Android through Gradle, which uses the same CMakeLists):
  1. checks that `3rdparty/imgui` is at the commit in `BASE`. If not, configure stops and says how
     to fix it; `ENP_IMGUI_ALLOW_UNPINNED=ON` overrides this for experiments;
  2. copies the ImGui sources the build uses into `build/<preset>/imgui-patched/`;
  3. applies the patches in order with `git apply` (it works outside a repository). If a patch does
     not apply, configure stops and names it;
  4. builds `imgui` from that copy.
- The submodule stays pristine, so `ignore = dirty` no longer hides anything that matters.
- The copy is redone only when `BASE`, a patch or the submodule changes. CMake tracks them as
  configure dependencies.

**Locking.**
- `.gitmodules`: remove `branch = docking`. The superproject's recorded commit is the lock, and
  `BASE` repeats it in plain sight.
- CI: checkout uses `submodules: recursive` and gets exactly that commit. The configure check above
  catches any mismatch.

**Upgrading ImGui** (`scripts/imgui-update.sh <commit>`):
1. Move the submodule to the new commit and write `BASE`.
2. Try the patches in a scratch copy. If they fail, apply them as 3-way merges
   (`git apply --3way` in a temporary worktree), resolve, and regenerate the patch files from the
   result.
3. Build and run all tests, including the touch test below; commit the bump and the patches
   together.

## What changes in the app afterwards

- `view/TouchScroll.cpp` is removed, and so are its calls in `MainView` and `Application`.
  `io.ConfigDragScrollSources` stays at its touch-screen default.
- The editor keeps its tap rule, but asks `ImGui::WasDragScrolled()` instead of our module.
- The app's own splitters (explorer, agent column, bottom panel) get `ImGuiButtonFlags_NoDragScroll`
  so a finger still resizes them.
- **Optional:** a "hand tool" preference that adds the mouse to `ConfigDragScrollSources`.

## Tests

- **The existing touch UI test unchanged** (it tests behaviour, not the mechanism):
  - a finger drag scrolls the editor 1:1 and glides;
  - a tap on a navigation button switches screens;
  - a drag from a ribbon button scrolls the ribbon without pressing it;
  - a mouse drag still selects text.
- **New:**
  - a finger drag on a slider moves the slider, not the window;
  - a finger drag on a splitter resizes;
  - a finger drag in an InputText selects;
  - a child at its end chains to its parent (when enabled);
  - a window with `NoDragScroll` does not move.
- **Build tests:**
  - configure fails clearly on an unpinned ImGui and on a patch that does not apply;
  - a clean build from a fresh clone applies all patches;
  - the Android Gradle build sees the patched copy.

## Order of work (when approved)

1. **Locking:** `BASE`, `.gitmodules`, and the configure check, with no patch yet. A safe first
   step.
2. **The patched-copy build:** CMake copies and applies an empty patch set, on all presets
   including Android.
3. **The patches:** API, core, widgets, plus the new tests.
4. **The switch:** remove `TouchScroll`, opt the splitters out, and run the touch test unchanged.
5. **The update script and README.**
