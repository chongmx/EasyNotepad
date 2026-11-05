// Ribbon banner: workspace switcher block, tab row with command search, and groups of
// large icon buttons with captions (in the style of CAD ribbons, kept flat and simple).
// The "My Tools" tab holds the user's pinned commands and custom shell actions. The user chooses
// which tabs show and their order (per workspace) in Customize Ribbon, or by right-clicking a tab.
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

    // A workspace's tabs, cached: rebuilt only when the view-model's ribbon revision changes
    // (tabs arranged, commands pinned, actions added), never per frame.
    struct Layout {
        int revision = -1;
        std::vector<Tab> all;                           // every tab the workspace defines
        std::vector<viewmodel::RibbonTabSetting> order; // the user's order and visibility, merged with `all`
        std::vector<int> orderIndex;                    // order[i] is all[orderIndex[i]]
        std::vector<int> shown;                         // the visible tabs (indices into `all`), in order
        std::vector<std::string> summary;               // per tab in `all`: its group names
    };
    std::vector<Tab> builtInTabs(bool agent) const;
    static int modeIndex(viewmodel::WorkspaceMode mode) { return mode == viewmodel::WorkspaceMode::Agent ? 1 : 0; }
    int modeIndex() const { return modeIndex(vm_.workspace.get()); }
    const Layout& layout(int mode);
    const Layout& layout() { return layout(modeIndex()); }
    int shownCount() { return int(layout().shown.size()); }
    const Tab& shownTab(int i) { const Layout& l = layout(); return l.all[size_t(l.shown[size_t(i)])]; }
    int& selectedTab();
    // Tab layout edits are queued while a list is being drawn and applied after it (applyTabEdit),
    // so the cached layout never changes under the loop that draws it.
    enum class TabEdit { None, Show, Hide, Step, MoveTo, StepAmongShown, Restore };
    void queueTabEdit(TabEdit kind, int mode, std::string name, int pos = 0, int arg = 0);
    void applyTabEdit();
    void tabMenu(int mode, const Layout& l, int shownIndex); // right-click on a ribbon tab
    void renderTabsSection();
    void renderPinnedSection();
    void renderActionsSection();
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
    // The group's "##more" popup below `at`, with the window padding the groups are drawn without.
    void moreMenu(const Group& group, ImVec2 at);
    // The small chevron at the ribbon's lower right that makes it slim, or full again.
    void renderSizeToggle(ImVec2 topLeft);
    bool slim() const { return vm_.slimRibbon.get(); }

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_; // UI scale
    int selectedTab_[2] = {0, 0}; // per workspace (modeIndex): position among the shown tabs
    Layout layouts_[2];           // per workspace (modeIndex)
    TabEdit editKind_ = TabEdit::None;
    int editMode_ = 0, editPos_ = 0, editArg_ = 0;
    std::string editName_;
    int customizeMode_ = 0;           // the workspace whose tabs the dialog edits
    int customizeSection_ = 0;        // 0 Tabs, 1 Pinned commands, 2 Custom actions
    int customizeSectionRequest_ = -1; // switch on the next frame (automation, "Add" button)
    std::string search_;
    bool searchFocused_ = false;
    bool customizeRequested_ = false;
    const char* openMore_ = nullptr; // a group whose menu an agent opened (its name), on the next frame
    int shownTab_ = -1; // the tab last scrolled into view in a too-narrow tab row
    std::string pinFilter_;
    std::string newActionName_;
    std::string newActionCommand_;
    std::vector<viewmodel::Subscription> subscriptions_;
};

} // namespace enp::view
