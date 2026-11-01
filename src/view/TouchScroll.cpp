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

// The innermost window under `window` (itself or a parent) that can scroll along `axis`.
ImGuiWindow* scrollableAlong(ImGuiWindow* window, int axis)
{
    for (ImGuiWindow* w = window; w; w = (w->Flags & ImGuiWindowFlags_ChildWindow) ? w->ParentWindow : nullptr)
        if ((axis == 0 ? w->ScrollMax.x : w->ScrollMax.y) > 0.5f)
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
