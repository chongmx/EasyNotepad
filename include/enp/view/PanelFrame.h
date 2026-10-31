// Panels (explorer, editor, agent, bottom panel) are rounded, bordered frames. Content inside a
// panel paints its backgrounds through these helpers: a background that reaches a corner of the
// panel is rounded like it, every other corner stays square. (ImGui clips to rectangles and rounds
// a child window's background on all four corners or none, so square content backgrounds would
// otherwise cover the panel's rounded corners and border.)
#pragma once

#include "imgui.h"

namespace enp::view::panel {

// MainView: the inside of a panel (within its 1px border) while its content is drawn.
void push(ImVec2 min, ImVec2 max, float rounding);
void pop();

// Corners of [min, max] that lie on corners of the current panel's inside (none outside panels).
ImDrawFlags cornersOf(ImVec2 min, ImVec2 max);
float rounding(); // of the current panel's inside; 0 outside panels

// A background rectangle with exactly the panel's corners rounded.
void fill(ImDrawList* drawList, ImVec2 min, ImVec2 max, ImU32 color);

// BeginChild whose background is painted with fill() (ImGuiCol_ChildBg is always square).
bool beginChild(const char* id, ImVec2 size, ImU32 background, ImGuiChildFlags childFlags = 0, ImGuiWindowFlags flags = 0);

} // namespace enp::view::panel
