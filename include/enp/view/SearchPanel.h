// The Search tab of the bottom panel: Find in Files (query, options, file filter, replace) and its
// results grouped by file, each match shown in its line.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <set>
#include <string>
#include <vector>

namespace enp::view {

class SearchPanel {
public:
    SearchPanel(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);
    void render();

private:
    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    bool focusQuery_ = false;
    bool showReplace_ = false;
    std::set<std::string> collapsed_; // result files folded away
    std::vector<viewmodel::Subscription> subscriptions_;
};

} // namespace enp::view
