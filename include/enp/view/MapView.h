// The logical map as diagrams: the focused level's parts as cards, laid out in dependency layers
// with "uses" arrows (modules, components) or as a flow of steps (functions, features).
// Click a card to zoom in (or open its code at the bottom level); breadcrumb / back to zoom out;
// drag to pan, wheel to zoom; switch between the Architecture and Features lenses.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <string>
#include <vector>

namespace enp::view {

class MapView {
public:
    MapView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    void render(ImVec2 size);
    // Called every frame the map is not drawn: once the map is gone (the folder closed, or it was
    // cleared), the layout of it is freed too, storage included (docs/MEMORY.md rules 3 and 5).
    // While the map exists the layout is the open folder's and stays (pan and zoom kept).
    void releaseIfMapGone();
    size_t layoutBytes() const; // for tests: the layout's heap storage

private:
    struct Box {
        std::string id;
        ImVec2 pos;  // world coordinates (unscaled pixels), top-left
        ImVec2 size;
    };
    struct Arrow {
        int from;
        int to;
        int weight;
        std::string label;
    };

    void renderHeader(float width);
    void layout(); // boxes_ / arrows_ for the current focus
    void releaseLayout();
    void fit(ImVec2 canvas);
    void drawBox(const Box& box, ImVec2 origin, bool hovered, float alpha);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;

    std::vector<Box> boxes_;
    std::vector<Arrow> arrows_;
    bool flow_ = false; // steps: sequential flow layout
    std::string layoutKey_;
    ImVec2 pan_{0.0f, 0.0f};
    float zoom_ = 1.0f;
    bool needsFit_ = true;
    bool panning_ = false;
    int animSerial_ = -1;
    double animStart_ = -10.0;
};

} // namespace enp::view
