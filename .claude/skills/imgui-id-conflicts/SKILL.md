---
name: imgui-id-conflicts
description: Find, fix and prevent Dear ImGui ID conflicts and other ImGui usage errors. Use whenever a Dear ImGui app shows the "Programmer error: N visible items with conflicting ID!" tooltip or a "MESSAGE FROM DEAR IMGUI" error such as "SetCursorPos()/SetCursorScreenPos() to extend window/parent boundaries", when two widgets highlight together, when a button, tab or popup reacts to the wrong click or does not open, or when writing or reviewing ImGui code with loops, tab rows, icon-only buttons, PushID/PopID, InvisibleButton, popups or several UI regions drawn into one window. Also use for adding a headless regression test that catches ID conflicts automatically.
---

# Dear ImGui ID conflicts

## How ImGui builds IDs

Every interactive item's ID is `hash(label, ID stack)`. The ID stack starts at the **window**
(each `Begin`/`BeginChild` is a separate root) and grows with `PushID`, `TreePush`, tab bars and
tables. Two items in the same window with the same label and the same stack get the same ID.
ImGui then hovers and activates both, and reports the error when the mouse is over one of them
(`io.ConfigDebugHighlightIdConflicts`, on by default in 1.91+).

Label syntax:
- `"Save"`: the text is shown and hashed.
- `"Save##toolbar"`: shows "Save" and hashes the whole string. Use it to tell apart items with the same text.
- `"Problems 18###problems"`: shows the text but hashes only `###problems`. Use it when the text changes (counts, file names) but the identity must not.
- `""` hashes like the parent. Never use an empty label: write `"##what"`.

## Typical causes

1. **Sibling regions drawn into the same window.** For example, a title bar and a ribbon both call
   `PushID(i); InvisibleButton("##tab")`. Ribbon tab 1 and document tab 1 then collide. This is the
   most common case in custom chrome: top bars, ribbons, status bars and toolbars that are not child windows.
2. **Loops without `PushID`**, or `PushID(index)` used by two different loops in the same scope.
3. **Icon-only buttons** with the same glyph: two `Button(ICON_FA_XMARK)` in one window.
4. **The same command shown twice**, e.g. a ribbon button that appears in two groups, scoped only by the command id.
5. **Dynamic labels without `###`**: the ID changes with the text, so tab selection or popup state is lost.
6. **`OpenPopup` and `BeginPopup` under different ID stacks**: the popup never opens. They must be
   called with the same stack, e.g. both inside the same `PushID` scope.
7. **Two `BeginChild` calls with the same name in one parent**: the children share state.

## Fix rules

- **Scope every UI region drawn into a shared window** with a string ID at its entry point, and pop it
  at its single exit:
  ```cpp
  void TitleBar::render()  { ImGui::PushID("titleBar");  /* ... */ ImGui::PopID(); }
  void Ribbon::render()    { ImGui::PushID("ribbon");    /* ... */ ImGui::PopID(); }
  void StatusBar::render() { ImGui::PushID("statusBar"); /* ... */ ImGui::PopID(); }
  ```
  Nested sub-regions with their own loops get their own scope too (`PushID("documentTabs")`).
- **Every loop body gets `PushID(key)`**. Prefer a stable key (document id, path, enum value) over the
  index when items can be reordered, closed or filtered. Nest scopes when keys come from different
  spaces, e.g. a group index and then the command id inside a ribbon group.
- **Icon-only and repeated buttons get a `##purpose` suffix**: `ICON_FA_XMARK "##closeFind"`.
- **Use `###stableId` whenever the visible label changes.**
- **Keep `PushID`/`PopID` balanced with no early returns between them.** Watch for `return` after a
  failed `BeginPopup` or `BeginTable` inside a pushed scope.
- **Child windows are already separate ID roots.** No extra scope is needed for items inside a
  `BeginChild` unless that child itself contains several such regions.

## Audit procedure

1. Read the error tooltip: hold Ctrl and use the **Item Picker** to break inside the conflicting item's
   call stack, or note the mouse position.
2. List the windows: for each `Begin`/`BeginChild`, list the components that draw directly into it.
   Every component sharing a window needs its own `PushID` scope.
3. Grep for risky patterns and check each one against the rules above:
   ```sh
   grep -nE 'PushID\(|InvisibleButton\("|Button\(ICON_|Selectable\("##|BeginTabItem\(|OpenPopup\(' src/**/*.cpp
   ```
   Look for the same literal label (`"##tab"`, `"##close"`, `"##row"`) used in more than one function
   that renders into the same window.
4. Fix, then verify with the test below. **Temporarily revert the fix and confirm the test fails at the
   reported spot.** A test that never failed proves nothing.

## Regression test (headless, no GPU)

Conflicts are only detected for **hovered** items: ImGui counts items whose ID equals the previous
frame's hovered ID, and `NewFrame()` sets `GImGui->DebugDrawIdConflictsId`. So the test must move the
mouse over the whole UI, one frame per position, in every state that changes the layout (tabs,
modes, open panels). A 10 px grid over a 1440×900 window costs about 13k frames, which takes seconds
without rendering.

`references/headless_id_test.cpp` is a complete template. Replace `renderUi()` with your root view and
list the UI states to visit. Essentials:
- `ImGui::CreateContext()`, set `io.DisplaySize` and `io.DeltaTime`, load the real fonts.
- With ImGui 1.92+ dynamic fonts, set `io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures` and
  acknowledge texture requests after each `Render()` (set a dummy `TexID` and `ImTextureStatus_OK`;
  mark `WantDestroy` as `Destroyed`). Without this, NewFrame asserts or text has no glyphs.
- Feed input through `io.AddMousePosEvent` and `io.AddMouseButtonEvent`. To click, use three frames:
  move, press, release.
- After each `NewFrame()`, check `GImGui->DebugDrawIdConflictsId != 0` (needs `imgui_internal.h`) and
  record the previous mouse position and UI state.
- Register it with CTest. It needs only the view library and ImGui core, no window or GL context.

**Also fail on every other ImGui usage error.** Set `io.ConfigErrorRecoveryEnableAssert = false` and
keep `ConfigErrorRecoveryEnableTooltip`/`EnableDebugLog` on. After `Render()`, check
`GImGui->ErrorCountCurrentFrame > 0` and take the message from the last `[imgui-error]` line of
`GImGui->DebugLogBuf` (clear the buffer every frame). This catches mismatched Push/Pop and Begin/End,
and the error below.

**Sweep every layout state, not just the default one.** Errors often appear only when content is
missing: no documents open, an empty list, a hidden panel, another theme or zoom, an open find bar.

### Related error: "SetCursorPos()/SetCursorScreenPos() to extend window/parent boundaries"

This appears when a custom-drawn region (title bar, status bar, ribbon) ends by moving the cursor past
the items it submitted, e.g. `SetCursorScreenPos(ImVec2(origin.x, origin.y + h))`, and nothing is submitted
afterwards. It often only shows in some states, because sometimes the region's own items happen to reach
that edge. Fix: end every hand-laid-out region with an item that claims its area:
```cpp
ImGui::SetCursorScreenPos(origin);
ImGui::Dummy(ImVec2(width, height)); // grows the parent and advances the layout
```

Optional extra check for apps with a custom title bar: fail when `GImGui->HoveredId != 0` while the
mouse is inside a region the app reports as draggable caption. On Windows (`WM_NCHITTEST` returning
`HTCAPTION`), such an item could never be clicked.
