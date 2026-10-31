// The file dialog (Open, Save As, Open Folder) bound to FileDialogViewModel: places on the left,
// the folder's entries, a path you can type, and the file name.
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
