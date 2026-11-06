// Hover hints (tooltips) and the hover glow, in two looks. Normal: a rounded, semi-transparent box
// without a border. Enhanced UI (Preferences, or View > Theme): no box, the text translucent with a
// glow in its shape, and a glow around the control under the pointer, as in a game's interface;
// the glows can breathe, and every parameter is the user's (Effects, Preferences > Enhanced UI).
//
// Every tooltip of the app goes through here, so each gets its own padding whatever the window it
// is opened from sets: the ribbon's groups set none, which left hints touching their box's edges.
#pragma once

#include "imgui.h"

namespace enp::view {
struct Palette;
}

namespace enp::view::ui {

// The enhanced look's parameters (Preferences > Enhanced UI).
struct Effects {
    float strength = 1.0f;       // every glow's intensity, 0..2
    float reach = 12.0f;         // how far a hint's glow reaches, in pixels at 100%
    float iconLight = 1.0f;      // the light in a lit icon, 0..2
    bool breathe = true;         // glows swell and ebb slowly
    float breathDepth = 0.25f;   // by this share of their intensity, 0..0.6
    float breathSeconds = 3.0f;  // one breath
    float fadeIn = 0.12f;        // seconds a control takes to light up
    float fadeOut = 0.28f;       // and to go dark
    ImU32 color = 0;             // the glows' colour; 0: the theme's accent
};

// Once a frame, before the UI: the look, its parameters and its colours.
void configure(bool enhanced, const Effects& effects, const Palette& palette, float scale);
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
// As hoverGlow, without the box: moves the light of control `id` one frame towards lit (hovered) or
// dark and returns it, 0..1, for a control that lights its own parts (iconLight, textGlow).
float hoverLight(ImGuiID id, bool hovered);
// How lit control `id` is, 0..1 (as its last hoverGlow left it; 0 in the normal look).
float light(ImGuiID id);
// A soft glow in the text's shape behind `text` at `pos` (the current font), at `amount` (0..1).
void textGlow(ImDrawList* dl, ImVec2 pos, const char* text, float amount);
// `tint` more saturated and brighter by `amount` (0..1): an icon with light shining in it.
ImU32 lit(ImU32 tint, float amount);
// A soft round light behind an icon, brightest at `center`, gone at `radius`.
void iconLight(ImDrawList* dl, ImVec2 center, float radius, ImU32 tint, float amount);
// Whether a hint or a glow is fading or breathing: the app keeps drawing frames until it ends.
bool animating();

} // namespace enp::view::ui
