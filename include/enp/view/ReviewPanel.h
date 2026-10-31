// Review tabs of the bottom panel: problems from all review tools, the diff under review,
// comments and the checklist. The panel's tab strip is drawn by MainView.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <string>
#include <vector>

namespace enp::view {

class ReviewPanel {
public:
    ReviewPanel(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    void renderTab(viewmodel::PanelTab tab);

private:
    void renderProblems();
    void renderToolChips();
    void renderDiff();
    std::vector<bool> chosen_; // git diff rows picked for staging
    std::string chosenFor_;
    void renderComments();
    void renderChecklist();

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    std::string commentDraft_;
};

} // namespace enp::view
