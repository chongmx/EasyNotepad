// The developer "Engine lab" page over EngineLabViewModel: the device, the model files (download,
// checksum), runs with or without the drafter on the CPU or GPU, and their results. A window on
// desktop, the whole screen (inside the safe area) on a phone. Reached from Help once developer
// mode is on.
#pragma once

#include "enp/view/Fonts.h"
#include "enp/view/Theme.h"
#include "enp/viewmodel/NotepadViewModel.h"

namespace enp::view {

class EngineLabView {
public:
    EngineLabView(viewmodel::NotepadViewModel& vm, const Fonts& fonts, const Palette& palette, float uiScale);

    void open() { open_ = true; }
    bool isOpen() const { return open_; }
    void close() { open_ = false; }
    // `area`: the usable screen (min, max), already inside the safe area.
    void render(bool fullScreen, ImVec2 areaMin, ImVec2 areaMax);

private:
    using Which = viewmodel::EngineLabViewModel::Which;
    viewmodel::EngineLabViewModel& lab() { return vm_.engineLab(); }

    void renderSummary(); // the fixed header: what is running, or the latest speeds
    void renderDevice();
    void renderFile(Which which);
    void renderRun();
    void renderLatest();
    void renderHistory();
    void renderMobileDataPrompt();
    // A collapsible section header (open at first); false while collapsed.
    bool section(const char* icon, const char* title);
    void stat(const char* label, const std::string& value, float width, bool big = false);

    viewmodel::NotepadViewModel& vm_;
    const Fonts& fonts_;
    const Palette& palette_;
    float s_;
    bool open_ = false;
    bool showOwnFiles_ = false;
};

} // namespace enp::view
