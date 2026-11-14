// The file dialog (Open, Save As, Open Folder) bound to FileDialogViewModel: places, the folder's
// entries, a path you can type, and the file name. Wide: places on the left, the name and buttons
// on one line. Narrow (a phone): places as a row of buttons above the list, which then has the
// whole width, the name on a line of its own and the buttons below it.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/FileDialogViewModel.h"

#include <string>

namespace enp::view {

class FileDialogView {
public:
    FileDialogView(viewmodel::FileDialogViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);
    void render();

private:
    void renderToolbar();
    void renderPlaces(bool narrow, float footerHeight);
    void renderPlaceButtons();
    void renderEntries(bool narrow, float footerHeight);
    void renderEntry(int index, const viewmodel::FileDialogViewModel::Entry& entry, bool narrow);
    void renderFooter(bool narrow);
    float footerHeight(bool narrow, float width) const;
    const char* nameLabel() const;

    viewmodel::FileDialogViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    std::string pathEdit_;    // the folder box while typing
    std::string shownFolder_; // pathEdit_ follows the folder when it changes
    int selected_ = -1;
    bool focusName_ = false;
};

} // namespace enp::view
