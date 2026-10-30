// Ribbon banner: workspace switcher block, tab row with command search, and groups of
// large icon buttons with captions (in the style of CAD ribbons, kept flat and simple).
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <string>
#include <vector>

namespace enp::view {

class RibbonView {
public:
    RibbonView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    // Draws the ribbon across the full width at the current cursor position.
    void render();
    float height() const;

private:
    struct Group {
        const char* name;
        std::vector<viewmodel::CommandId> buttons;
        std::vector<viewmodel::CommandId> more; // listed in the caption's drop-down
    };
    struct Tab {
        const char* name;
        std::vector<Group> groups;
    };

    const std::vector<Tab>& tabs() const;
    int& selectedTab();
    void renderWorkspaceBlock(float width, float height);
    void renderWorkspacePopup();
    void renderTabRow(float width);
    void renderCommandSearch(float width);
    void renderGroups(const Tab& tab);
    bool ribbonButton(viewmodel::CommandId id);
    void groupCaption(const Group& group, float x0, float x1, float y);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_; // UI scale
    int selectedTab_[2] = {0, 0}; // per workspace mode
    std::string search_;
    bool searchFocused_ = false;
};

} // namespace enp::view
