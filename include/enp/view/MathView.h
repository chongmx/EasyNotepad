// Draws a formula (model::math::Node) with Dear ImGui's draw list: fractions stacked on a rule,
// scripts raised and lowered at a smaller size, radicals drawn, big operators with limits,
// delimiters stretched to their content. Measures first so text can flow around it.
#pragma once

#include "enp/model/Math.h"

#include "imgui.h"

namespace enp::view {

struct MathBox {
    float width = 0.0f;
    float ascent = 0.0f;  // above the baseline
    float descent = 0.0f; // below the baseline
    float height() const { return ascent + descent; }
};

struct MathStyle {
    ImFont* font = nullptr;
    float size = 15.0f;   // pixels, of the body text
    float scale = 1.0f;   // UI scale (line thickness)
    ImU32 color = 0xFFFFFFFF;
    bool display = false; // display style: full-size fractions, limits above and below
};

MathBox measureMath(const model::math::Node& node, const MathStyle& style);
// `baseline` is the left end of the baseline.
void drawMath(ImDrawList* dl, const model::math::Node& node, ImVec2 baseline, const MathStyle& style);

} // namespace enp::view
