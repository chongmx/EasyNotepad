// Ribbon banner: workspace switcher block, tab row with command search, and groups of
// large icon buttons with captions (in the style of CAD ribbons, kept flat and simple).
// The "My Tools" tab holds the user's pinned commands and custom shell actions.
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
        bool customActions = false;             // shows the user's custom actions instead
    };
    struct Tab {
        const char* name;
        std::vector<Group> groups;
    };

    std::vector<Tab> tabs() const;
    int& selectedTab();
    void renderWorkspaceBlock(float width, float height);
    void renderWorkspacePopup();
    void renderTabRow(float width);
    void renderCommandSearch(float width);
    void renderGroups(const Tab& tab);
    bool ribbonButton(viewmodel::CommandId id);
    // Large icon-over-label button; returns true when clicked and enabled.
    bool drawButton(const char* id, const char* icon, const char* label, ImU32 tint, bool enabled, bool checked,
                    const char* tooltip, const char* detail);
    void renderCustomActions(float& x, float top);
    void renderCustomizeDialog();
    void groupCaption(const Group& group, float x0, float x1, float y);
    void morePopup(const Group& group); // the group's extra commands (inside its "##more" popup)
    // The small chevron at the ribbon's lower right that makes it slim, or full again.
    void renderSizeToggle(ImVec2 topLeft);
    bool slim() const { return vm_.slimRibbon.get(); }

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_; // UI scale
    int selectedTab_[2] = {0, 0}; // per workspace mode
    std::string search_;
    bool searchFocused_ = false;
    bool customizeRequested_ = false;
    std::string pinFilter_;
    std::string newActionName_;
    std::string newActionCommand_;
    std::vector<viewmodel::Subscription> subscriptions_;
};

} // namespace enp::view
