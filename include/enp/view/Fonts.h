// Font loading (bundled TTFs read through the OS layer, Font Awesome icons merged in).
#pragma once

#include "imgui.h"

namespace enp::view {

struct Fonts {
    ImFont* ui = nullptr;   // proportional UI text + icons
    ImFont* mono = nullptr; // editor and diff text + icons
    bool hasIcons = false;

    static constexpr float kUiSize = 15.0f;
    static constexpr float kEditorSize = 15.0f;
    static constexpr float kRibbonIconSize = 22.0f;

    // Must be called after ImGui::CreateContext() and before the first frame.
    static Fonts load();
};

} // namespace enp::view
