# Touch drag-to-scroll for Dear ImGui (app-side)

ImGui scrolls only by wheel or scrollbar. This module, called once per frame after
`ImGui::NewFrame()` and before building the UI, makes a finger drag the content under it.

Rules it implements (see SKILL.md §3): threshold ~8 dp; innermost *user-scrollable* window
(skips `NoScrollWithMouse`/`NoScrollbar`; horizontal only with a horizontal scrollbar); axis lock;
1:1 movement; `ClearActiveID()` on pan start so the pressed button does not click; text being
edited keeps its drag; momentum with exp(-4·t) decay; a new press stops the glide. Keep the app
drawing while `gliding()`.

Custom views (an editor) check `touch::fingerDown()` and act on release only if `!touch::moved()`.

Root-cause alternative: implement the same inside ImGui (`NewFrame` → `UpdateDragScroll()`), with
per-window/per-item opt-outs (`ImGuiChildFlags_NoDragScroll`, `ImGuiItemFlags_NoDragScroll`) and
widgets that claim drags (InputText, sliders, splitters), kept as patch files applied to a pinned
ImGui commit in a build-dir copy.

## TouchScroll.h

```cpp
// Drag-to-scroll for touch screens. ImGui scrolls only with a wheel or by the scrollbar; on a
// phone a finger should move the content under it (and let it glide on a little after letting go).
// Touch input only: a mouse keeps selecting text and dragging as before.
#pragma once

namespace enp::view::touch {

// Once per frame, after ImGui::NewFrame() and before the UI is built.
void update(float uiScale);

bool fingerDown();  // the current press is a finger (or the last one, until the next press)
bool panning();     // that finger is moving content now
bool moved();       // the current or last press became a pan (so its release is not a tap)
bool gliding();     // content still moving after the finger lifted: keep drawing frames

} // namespace enp::view::touch
```

## TouchScroll.cpp

```cpp
#include "enp/view/TouchScroll.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cmath>

namespace enp::view::touch {

namespace {

struct State {
    bool finger = false;   // the press is a touch
    bool pressed = false;  // a finger is down and may still become a pan
    bool panning = false;
    bool moved = false;
    int axis = -1;         // 0 = x, 1 = y
    ImGuiID window = 0;    // the window being scrolled
    ImVec2 start, last;
    float velocity = 0.0f; // pixels per second along `axis`, for the glide
};

State s;

// Whether the user may scroll `w` along `axis`: the same windows a mouse wheel or a scrollbar
// scrolls. A window with scrolling turned off (the app's frame, a body sized to fit) may still
// have content past its edge, such as a tab row too wide for a phone; dragging must not shift it.
bool userScrollable(const ImGuiWindow* w, int axis)
{
    if (w->Flags & (ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoScrollbar))
        return false;
    if (axis == 0 && !(w->Flags & (ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar)))
        return false;
    return (axis == 0 ? w->ScrollMax.x : w->ScrollMax.y) > 0.5f;
}

// The innermost window under `window` (itself or a parent) that can scroll along `axis`.
ImGuiWindow* scrollableAlong(ImGuiWindow* window, int axis)
{
    for (ImGuiWindow* w = window; w; w = (w->Flags & ImGuiWindowFlags_ChildWindow) ? w->ParentWindow : nullptr)
        if (userScrollable(w, axis))
            return w;
    return nullptr;
}

// Moves the window's content by `delta` pixels; returns false at the end of its range.
bool scrollBy(ImGuiWindow* w, int axis, float delta)
{
    const float before = axis == 0 ? w->Scroll.x : w->Scroll.y;
    const float maxValue = axis == 0 ? w->ScrollMax.x : w->ScrollMax.y;
    const float after = ImClamp(before - delta, 0.0f, maxValue);
    if (axis == 0)
        ImGui::SetScrollX(w, after);
    else
        ImGui::SetScrollY(w, after);
    return after != before;
}

} // namespace

void update(float uiScale)
{
    ImGuiContext& g = *GImGui;
    const ImGuiIO& io = g.IO;
    const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 60.0f;

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s = State{};
        s.finger = io.MouseSource == ImGuiMouseSource_TouchScreen;
        s.pressed = s.finger;
        s.start = s.last = io.MousePos;
        return;
    }

    if (s.pressed && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const ImVec2 pos = io.MousePos;
        if (!s.panning) {
            const ImVec2 d(pos.x - s.start.x, pos.y - s.start.y);
            if (std::sqrt(d.x * d.x + d.y * d.y) < 8.0f * uiScale)
                return;
            // Text being edited keeps its own drag (selecting).
            if (g.ActiveId != 0 && g.ActiveId == g.InputTextState.ID) {
                s.pressed = false;
                return;
            }
            const int along = std::fabs(d.y) >= std::fabs(d.x) ? 1 : 0;
            ImGuiWindow* target = scrollableAlong(g.HoveredWindow, along);
            int axis = along;
            if (!target) { // nothing scrolls that way: the other way, when the drag leans to it too
                const float minor = along == 1 ? std::fabs(d.x) : std::fabs(d.y);
                if (minor > 4.0f * uiScale) {
                    target = scrollableAlong(g.HoveredWindow, 1 - along);
                    axis = 1 - along;
                }
            }
            if (!target) {
                s.pressed = false; // not a pan: whatever is under the finger keeps the press
                return;
            }
            s.panning = s.moved = true;
            s.axis = axis;
            s.window = target->ID;
            s.last = s.start; // the content catches up with the finger from where it went down
            ImGui::ClearActiveID(); // the press no longer clicks anything
        }
        if (ImGuiWindow* w = ImGui::FindWindowByID(s.window)) {
            const float delta = s.axis == 0 ? pos.x - s.last.x : pos.y - s.last.y;
            scrollBy(w, s.axis, delta);
            s.velocity = s.velocity * 0.4f + (delta / dt) * 0.6f;
        }
        s.last = pos;
        return;
    }

    if (s.pressed && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) { // lifted
        s.pressed = false;
        s.panning = false;
        if (std::fabs(s.velocity) < 60.0f * uiScale)
            s.velocity = 0.0f;
    }

    // The glide: slows to a stop within about a second, or stops at the end of the content.
    if (s.velocity != 0.0f) {
        ImGuiWindow* w = ImGui::FindWindowByID(s.window);
        if (!w || !scrollBy(w, s.axis, s.velocity * dt)) {
            s.velocity = 0.0f;
            return;
        }
        s.velocity *= std::exp(-4.0f * dt);
        if (std::fabs(s.velocity) < 20.0f * uiScale)
            s.velocity = 0.0f;
    }
}

bool fingerDown() { return s.finger; }
bool panning() { return s.panning; }
bool moved() { return s.moved; }
bool gliding() { return s.velocity != 0.0f; }

} // namespace enp::view::touch
```

## Testing with a simulated finger

```cpp
io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen); // before AddMousePosEvent, every frame
io.AddMousePosEvent(x, y);
io.AddMouseButtonEvent(ImGuiMouseButton_Left, down);
```

Assert: a 300 px drag scrolls the target ~300 px; the glide never moves backwards; a tap on a nav
button switches screens; a drag starting on a button scrolls without pressing it; a sideways drag on
a too-wide row leaves the root window's Scroll at 0; a mouse drag still selects and does not scroll.
Find windows by their own name (after the last "/") — "##editor" also matches "##editorPanel".
