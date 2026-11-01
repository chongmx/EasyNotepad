# Responsive layout: desktop, phone portrait, phone landscape

## Choosing the arrangement

Every frame, from the area left after the safe area, in dp:

```cpp
enum class Layout { Desktop, Portrait, Landscape };

Layout chooseLayout(float widthDp, float heightDp)
{
    if (widthDp < 600.0f) // a phone, or a narrow window
        return widthDp >= heightDp ? Layout::Landscape : Layout::Portrait;
    if (heightDp < 500.0f) // a phone turned sideways
        return Layout::Landscape;
    return Layout::Desktop;
}
```

Expected: 390×844 → Portrait, 844×390 → Landscape, 412×915 → Portrait, 800×1280 (tablet) →
Desktop, 1440×900 → Desktop. Put these cases in a test.

Size in dp = pixels / UI scale (Android: density / 160; desktop: monitor content scale).

## Phone portrait

- **Top bar** (one row): a menu button that shows/hides the full toolbar/ribbon; the active
  document's name (tap → list of open documents); save (and undo/redo only when the name keeps
  ~140 dp); a "more" menu with file commands, theme, preferences, help. Window buttons only on a
  desktop running the phone layout.
- **One screen at a time**, picked by a **bottom navigation bar** (~58 dp): e.g. Files, Code, Agent,
  Tools. Icon over label, the active one with a pill behind the icon; small dots for "something
  waiting" (agent working, findings).
- **No status bar.** No side panels: the panel that was a column becomes a screen.
- The toolbar/ribbon, when shown, scrolls sideways (its tab row too).

## Phone landscape

The same screens with a **navigation rail** (~64 dp wide, 64 dp cells) on the left, so the
content keeps the full height. Top bar as in portrait.

## Switching screens automatically

The user should not hunt for where things went. Subscribe to the view-model:
- a document becomes active (opened from files, search, the agent) → the editor screen;
- "ask the agent" → the agent screen;
- the bottom panel shown or its tab changed (terminal, search, problems) → the tools screen;
- the explorer shown → the files screen.
Desktop ignores the current screen, so these subscriptions are harmless there.

## Panels that change on a phone

- A bottom panel with many tabs: when the tabs do not fit, show the current tab with a ▾ that lists
  them all (keep automation ids for every tab).
- Panel maximize/close buttons: hidden (the panel is a whole screen).
- Editor side bars (minimap, outline): hidden; the text gets the full width.
- Lists with details: details below the list (see SKILL.md §4).

## Testing on a desktop

- `--window-size=WxH` (dp) creates a window of that size.
- `--safe-area=l,t,r,b` (dp) pretends the system covers those edges.
- Rotation: resize the running window programmatically (Win32 `SetWindowPos`), not with the
  user's mouse — the layout must switch on the next frame.
