// Making things fit narrow spaces (a phone, a narrow panel): text cut short with an ellipsis
// instead of running into its neighbour, button rows that wrap, dialogs no larger than the screen.
#pragma once

#include "imgui.h"

#include <string>

namespace enp::view::fit {

// Draws `text` at `pos` in the current font, cut short with "..." so it ends before `maxX`.
// Returns the x where the drawn text ends.
float text(ImDrawList* dl, ImVec2 pos, float maxX, ImU32 color, const char* text, const char* end = nullptr);
inline float text(ImDrawList* dl, ImVec2 pos, float maxX, ImU32 color, const std::string& s)
{
    return text(dl, pos, maxX, color, s.c_str(), s.c_str() + s.size());
}

// The width of a Button / SmallButton showing `label` (the part before "##").
float buttonWidth(const char* label, bool small = false);

// Keeps the next item on this line when one `width` wide still fits; otherwise it starts the next
// line. Call after an item, instead of SameLine().
void sameLineIfFits(float width, float spacing = -1.0f);
inline void sameLineIfFitsButton(const char* label, bool small = false)
{
    sameLineIfFits(buttonWidth(label, small));
}

// A width for the next item: `wanted`, or less when the line has less room (never below `least`).
float itemWidth(float wanted, float least);

// A window size: `wanted`, but never larger than the screen minus a margin (dialogs on a phone).
ImVec2 windowSize(ImVec2 wanted);
// Before Begin / BeginPopupModal: the next window may not grow larger than the screen, also when
// it sizes itself to its content.
void constrainNextWindow();

// The room is narrow (a phone, a thin panel): `width` is less than `threshold` (unscaled dp * scale).
inline bool narrow(float width, float threshold) { return width < threshold; }

} // namespace enp::view::fit
