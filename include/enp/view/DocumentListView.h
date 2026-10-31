// The open documents as a list: one row per document (icon, name, folder, unsaved dot or close
// button, the active one highlighted), filtered, with middle-click close, drag to reorder and one
// context menu shared with the tabs. Used by the explorer's Open tab and the tabs' overflow list.
#pragma once

#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <functional>
#include <string>

namespace enp::view {

class DocumentListView {
public:
    struct Options {
        bool reorder = true; // drag rows up and down (only when not filtered)
    };

    DocumentListView(viewmodel::NotepadViewModel& vm, const Palette& palette, float uiScale);

    // Draws the rows of the documents whose path contains `filter` (case-insensitive). Returns the
    // index of the document the user picked this frame (clicked, or Enter on the first match when
    // `enterPicks`), else -1. Picking activates it.
    int render(const char* id, const std::string& filter, Options options, bool enterPicks = false);

    // The context menu of a document (tabs and rows alike): request it from anywhere, draw it once
    // per frame at the top level with renderMenu().
    void requestMenu(int index);
    void renderMenu();
    std::function<void(int index)> onReveal; // "Reveal in Explorer"

    static bool matches(const model::Document& doc, const std::string& filter);

private:
    viewmodel::NotepadViewModel& vm_;
    const Palette& palette_;
    float s_;
    int menuIndex_ = -1;
    bool openMenu_ = false;
};

} // namespace enp::view
