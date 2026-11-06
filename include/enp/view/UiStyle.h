// Hover hints (tooltips) and the hover glow, in two looks. Normal: a rounded, semi-transparent box
// without a border. Enhanced UI (Preferences, or View > Theme): no box, the text translucent with a
// glowing outline, and a glow around the button or tab under the pointer, as in a game's interface.
//
// Every tooltip of the app goes through here, so each gets its own padding whatever the window it
// is opened from sets: the ribbon's groups set none, which left hints touching their box's edges.
#pragma once

#include "imgui.h"

namespace enp::view {
struct Palette;
}

namespace enp::view::ui {

// Once a frame, before the UI: the look and its colours.
void configure(bool enhanced, const Palette& palette, float scale);
bool enhanced();

// As ImGui's SetTooltip, SetItemTooltip, BeginTooltip, BeginItemTooltip and EndTooltip.
void tooltip(const char* fmt, ...) IM_FMTARGS(1);
void itemTooltip(const char* fmt, ...) IM_FMTARGS(1);
bool beginTooltip();
bool beginItemTooltip();
void endTooltip();

// The enhanced look's glow around control `id` (its ImGui item id) at [min, max]: it fades in while
// `hovered` and out after. Called once a frame for the control; nothing in the normal look.
void hoverGlow(ImDrawList* dl, ImGuiID id, bool hovered, ImVec2 min, ImVec2 max, float rounding);
// How lit control `id` is, 0..1 (as its last hoverGlow left it; 0 in the normal look).
float light(ImGuiID id);
// `tint` more saturated and brighter by `amount` (0..1): an icon with light shining in it.
ImU32 lit(ImU32 tint, float amount);
// A soft round light behind an icon, brightest at `center`, gone at `radius`.
void iconLight(ImDrawList* dl, ImVec2 center, float radius, ImU32 tint, float amount);
// Whether a hint or a glow is fading: the app keeps drawing frames until it ends.
bool animating();

} // namespace enp::view::ui
