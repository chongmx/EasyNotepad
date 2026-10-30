// Side-by-side diff rendering, shared by the agent change preview and the review panel.
#pragma once

#include "enp/model/Diff.h"
#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"

#include <vector>

namespace enp::view {

// Draws `rows` in a scrolling child window of `size` (0 = fill available space).
void renderSideBySideDiff(const char* id, const std::vector<model::DiffRow>& rows, const Fonts& fonts,
                          const Palette& palette, float zoom, float uiScale, ImVec2 size);

} // namespace enp::view
