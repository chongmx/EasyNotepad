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

// The enhanced look's glow around a hovered control [min, max]; nothing in the normal look.
void hoverGlow(ImDrawList* dl, ImVec2 min, ImVec2 max, float rounding);

} // namespace enp::view::ui
