// Review panel: problems from all review tools, the diff under review, comments, checklist.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <string>

namespace enp::view {

class ReviewPanel {
public:
    ReviewPanel(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    void render(ImVec2 size);

private:
    void renderProblems();
    void renderToolChips();
    void renderDiff();
    void renderComments();
    void renderChecklist();

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    viewmodel::ReviewTab shownTab_ = viewmodel::ReviewTab::Problems;
    std::string commentDraft_;
};

} // namespace enp::view
