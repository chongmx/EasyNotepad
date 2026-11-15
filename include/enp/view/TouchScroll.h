// Drag-to-scroll for touch screens. ImGui scrolls only with a wheel or by the scrollbar; on a
// phone a finger should move the content under it (and let it glide on a little after letting go).
// Touch input only: a mouse keeps selecting text and dragging as before.
#pragma once

#include "imgui.h"

namespace enp::view::touch {

// Once per frame, after ImGui::NewFrame() and before the UI is built.
void update(float uiScale);

// An item that moves with a drag itself claims the drag along the axes it moves on: a slider or a
// splitter between columns across (ClaimX), a splitter between rows up and down (ClaimY), a canvas
// both. A finger that lands on it and drags that way moves the item, never the window under it,
// even when that window scrolls the same way; a drag the other way still scrolls (a page scrolls
// up and down over its sliders, as on Android). Call right after the item, every frame.
enum Claim { ClaimX = 1, ClaimY = 2, ClaimBoth = 3 };
void claimDrag(int axes);

bool fingerDown();  // the current press is a finger (or the last one, until the next press)
bool panning();     // that finger is moving content now
bool moved();       // the current or last press became a pan (so its release is not a tap)
bool gliding();     // content still moving after the finger lifted: keep drawing frames

// One finger on a touch screen, from the platform (Android) in place of ImGui's backend, in screen
// pixels as ImGui's mouse. ImGui sees where the finger is but not its press until the gesture is
// known: a finger that lifts without having moved is a tap (a press and a release where it lifted);
// one that drags content scrolls it and presses nothing; one that moves where nothing scrolls gets
// its press where it landed and drags from there (a slider, a splitter, selecting text). So no row
// opens and no tab switches under a finger that only meant to scroll. fingerCancelled(): the gesture
// is over without a click (a second finger began a pinch, the system took the touch).
void fingerLanded(float x, float y);
void fingerMoved(float x, float y);
void fingerLifted(float x, float y);
void fingerCancelled();

// Two fingers moving apart or together (a phone's pinch), as the platform reports them: the
// number of fingers down and the first two's positions (screen pixels, as ImGui's mouse). Fewer
// than two fingers ends the pinch. While it lasts no press, drag or tap acts.
void pinch(int fingers, float x0, float y0, float x1, float y1);
bool pinching();

// Zooming a reader's view about the point under the fingers. No view's content grows exactly in
// proportion to its zoom (page gaps, headers and margins keep their size; reflowed text wraps
// anew), so scaling the scroll would drift and jitter. Each view instead, inside its window and
// before laying out its content:
//   1. takeZoom(): this frame's zoom, when centred in the window (the pinch between the fingers,
//      or Ctrl+wheel over it on a desktop);
//   2. when a pinch starts, finds the point under the fingers in its own terms (a page and a
//      point on it, a cell position before zooming, a paragraph and how far into it) and keeps
//      it while the fingers stay down (found again each frame from a scroll of whole pixels, it
//      would creep);
//   3. lays out at the new zoom and scrollTo() puts that point back under the fingers.
// takeZoom: `zoom` is changed within [lowest, highest] and `centre` is where (screen pixels);
// true when it changed. Last frame's `pending` scroll is set again first.
bool takeZoom(float& zoom, float lowest, float highest, ImVec2& centre, ImVec2& pending);
// Scrolls the current window to `to` now (this frame's content is drawn from it), and again next
// frame if the old content's range cut it short (`pending`): after takeZoom, before the content.
void scrollTo(ImVec2 to, ImVec2& pending);

} // namespace enp::view::touch
