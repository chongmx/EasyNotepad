// Drag-to-scroll for touch screens. ImGui scrolls only with a wheel or by the scrollbar; on a
// phone a finger should move the content under it (and let it glide on a little after letting go).
// Touch input only: a mouse keeps selecting text and dragging as before.
#pragma once

#include "imgui.h"

namespace enp::view::touch {

// Once per frame, after ImGui::NewFrame() and before the UI is built.
void update(float uiScale);

bool fingerDown();  // the current press is a finger (or the last one, until the next press)
bool panning();     // that finger is moving content now
bool moved();       // the current or last press became a pan (so its release is not a tap)
bool gliding();     // content still moving after the finger lifted: keep drawing frames

// Two fingers moving apart or together (a phone's pinch), as the platform reports them: the
// number of fingers down and the first two's positions (screen pixels, as ImGui's mouse). Fewer
// than two fingers ends the pinch. While it lasts no press, drag or tap acts.
void pinch(int fingers, float x0, float y0, float x1, float y1);
bool pinching();

// For a reader's view (the current window): takes this frame's zoom when it is centred in the
// window (the pinch between the fingers, or Ctrl+wheel over it on a desktop) and zooms `zoom`
// (kept within [lowest, highest]) about that point, keeping it where it is: the window's scroll
// is moved for the new size at once, so this frame's content is drawn from it, and set again next
// frame if the old content's range cut it short (`pending`). Call each frame inside the window, before the content; returns whether `zoom` changed.
bool zoomView(float& zoom, float lowest, float highest, ImVec2& pending);

// For a view whose content does not grow in proportion to its zoom (pages with gaps of a fixed
// size between them, centred when narrower than the view), which keeps the point under the
// fingers in place itself: this frame's zoom for the current window, as zoomView takes it, but
// without scrolling. `zoom` is changed within [lowest, highest] and `centre` is where (screen
// pixels); true when it changed. Last frame's `pending` scroll is set again first, as zoomView does.
bool takeZoom(float& zoom, float lowest, float highest, ImVec2& centre, ImVec2& pending);
// Scrolls the current window to `to` now (this frame's content is drawn from it), and again next
// frame if the old content's range cut it short (`pending`): after takeZoom, before the content.
void scrollTo(ImVec2 to, ImVec2& pending);

} // namespace enp::view::touch
