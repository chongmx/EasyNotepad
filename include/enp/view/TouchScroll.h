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
