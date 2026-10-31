// Layers side bar: scope switch (File / Project), density (Compact / Summary / Detailed), a
// breadcrumb of the zoomed-in layer, and cards for its sub-layers with AI summaries. Zoom in with
// a card's chevron or a double-click; zoom out with the breadcrumb, the back button or mouse-back;
// Ctrl+wheel changes the density. Clicking a card shows that part in the editor.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <functional>
#include <string>

namespace enp::view {

class LayersView {
public:
    LayersView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    // Everything below the side bar's mode header.
    void render(ImVec2 size);

private:
    void renderControls(float width);
    void renderBreadcrumb(float width);
    void renderCards(ImVec2 size);
    float aboutCard(const viewmodel::LayerNode& node, float x, float y, float width, bool draw);
    float card(int index, int maxSize, bool current, float x, float y, float width, bool draw);
    // Segmented control; calls `onSelect` with the clicked segment (also for automation clicks).
    // `width` receives the control's width.
    void segmented(const char* id, const char* const* labels, const char* const* tooltips, int count, int selected,
                   ImVec2 pos, float height, float& width, const std::function<void(int)>& onSelect);

    ImU32 kindColor(viewmodel::LayerKind kind) const;
    static const char* kindIcon(viewmodel::LayerKind kind);
    std::string meta(const viewmodel::LayerNode& node) const;

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    int animSerial_ = -1;
    double animStart_ = -10.0;
};

} // namespace enp::view
