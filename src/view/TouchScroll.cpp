#include "enp/view/TouchScroll.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cfloat>
#include <cmath>
#include <initializer_list>

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

// The pinch: the fingers' distance last reported, and the zoom gathered since the last frame
// (`gathered`) and this frame's for the view under the fingers (`factor`, at `centre`).
struct Pinch {
    bool active = false;
    float distance = 0.0f, gathered = 1.0f, factor = 1.0f;
    ImVec2 centre;
    bool wheel = false; // this frame's factor is Ctrl+wheel's (taken by the window under the mouse)
};

Pinch p;

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

// Scrolls `w` to `to` for this frame's content too, not from the next frame as SetScroll does: the
// window's content has not been submitted yet, so its start and cursor move with the scroll. Without
// it a zoomed frame is drawn at the new size from the old scroll, a jump of (scroll × change) that
// grows with the zoom: thousands of pixels deep in a magnified page.
void scrollNow(ImGuiWindow* w, ImVec2 to)
{
    const ImVec2 d(to.x - w->Scroll.x, to.y - w->Scroll.y);
    if (d.x == 0.0f && d.y == 0.0f)
        return;
    w->Scroll = to;
    w->ScrollTarget = ImVec2(FLT_MAX, FLT_MAX); // nothing left to apply next frame
    for (ImVec2* v : {&w->WorkRect.Min, &w->WorkRect.Max, &w->ContentRegionRect.Min, &w->ContentRegionRect.Max, &w->DC.CursorStartPos,
                      &w->DC.CursorPos, &w->DC.CursorPosPrevLine, &w->DC.CursorMaxPos, &w->DC.IdealMaxPos})
        *v = ImVec2(v->x - d.x, v->y - d.y);
    w->ParentWorkRect = w->WorkRect;
    w->DC.Indent.x -= d.x;
}

} // namespace

void pinch(int fingers, float x0, float y0, float x1, float y1)
{
    if (fingers < 2) {
        p.active = false;
        return;
    }
    const float d = std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0));
    p.centre = ImVec2((x0 + x1) * 0.5f, (y0 + y1) * 0.5f);
    if (p.active && p.distance > 1.0f && d > 1.0f)
        p.gathered *= d / p.distance;
    p.active = true;
    p.distance = d;
}

bool pinching() { return p.active; }

bool takeZoom(float& zoom, float lowest, float highest, ImVec2& centre, ImVec2& pending)
{
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (pending.x >= 0.0f) { // last frame's zoom: its scroll, now that the content has its size
        if (window->Scroll.x != pending.x || window->Scroll.y != pending.y) { // cut short by last frame's range
            ImGui::SetScrollX(window, pending.x);
            ImGui::SetScrollY(window, pending.y);
        }
        pending = ImVec2(-1.0f, -1.0f);
    }
    if (p.factor == 1.0f)
        return false;
    const ImVec2 c = p.wheel ? ImGui::GetIO().MousePos : p.centre;
    const ImRect r = window->InnerRect;
    if (!r.Contains(c) || (p.wheel && !ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)))
        return false;
    const float before = zoom;
    zoom = ImClamp(zoom * p.factor, lowest, highest);
    p.factor = 1.0f; // taken
    centre = c;
    return zoom != before;
}

void scrollTo(ImVec2 to, ImVec2& pending)
{
    pending = ImVec2(ImMax(0.0f, IM_ROUND(to.x)), ImMax(0.0f, IM_ROUND(to.y))); // whole pixels, the nearest (no drift one way)
    scrollNow(ImGui::GetCurrentWindow(), pending);
}

bool zoomView(float& zoom, float lowest, float highest, ImVec2& pending)
{
    const float before = zoom;
    ImVec2 c;
    if (!takeZoom(zoom, lowest, highest, c, pending))
        return false;
    // The point under the fingers stays: its place in the content grows with the zoom.
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    const float k = zoom / before;
    const ImVec2 at(c.x - window->Pos.x, c.y - window->Pos.y);
    scrollTo(ImVec2((window->Scroll.x + at.x) * k - at.x, (window->Scroll.y + at.y) * k - at.y), pending);
    return true;
}

void update(float uiScale)
{
    ImGuiContext& g = *GImGui;
    const ImGuiIO& io = g.IO;
    const float dt = io.DeltaTime > 0.0f ? io.DeltaTime : 1.0f / 60.0f;

    // This frame's zoom: what the fingers gathered, or Ctrl+wheel's (desktops); one the view
    // under it did not take last frame is dropped.
    p.factor = p.gathered;
    p.gathered = 1.0f;
    p.wheel = false;
    if (p.factor == 1.0f && io.KeyCtrl && io.MouseWheel != 0.0f && !p.active) {
        p.factor = std::pow(1.15f, io.MouseWheel);
        p.wheel = true;
    }
    if (p.active) { // two fingers: no pan, no glide, and the release is not a tap
        s.pressed = s.panning = false;
        s.velocity = 0.0f;
        s.moved = true;
        return;
    }

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
