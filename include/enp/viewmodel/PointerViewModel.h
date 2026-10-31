// The agent's own cursor: pointing at a control with a message, the guided tour, and the hints that
// show the user where things are when they ask.
#pragma once

#include <string>

namespace enp::viewmodel {

class NotepadViewModel;

class PointerViewModel {
public:
    explicit PointerViewModel(NotepadViewModel& owner);

    // Points at a UI element; `seconds` > 0 hides it again after that long.
    void pointAt(std::string target, std::string message, bool click = false, double seconds = 6.0);
    void hidePointer();
    void startTour();
    void stopTour();
    bool tourRunning() const { return tourStep_ >= 0; }
    // Shows where the controls a prompt is about are (a stand-in for the agent deciding to).
    void guideFor(const std::string& prompt);
    void tick(double now);

private:
    void showTourStep();

    NotepadViewModel& owner_;
    double now_ = 0.0;
    double hideAt_ = -1.0;
    int tourStep_ = -1;
    double tourNextAt_ = 0.0;
};

} // namespace enp::viewmodel
