#include "enp/viewmodel/PointerViewModel.h"

#include "enp/viewmodel/NotepadViewModel.h"

#include <cctype>

namespace enp::viewmodel {

namespace {

struct TourStep {
    const char* target;
    const char* message;
};

const TourStep kTour[] = {
    {"workspace", "Two workspaces: Agent, where I do the work and you review it, and Code, where you edit and I help."},
    {"ribbon-tab:Read", "The Read tab is about understanding code quickly."},
    {"command:DetailOverview", "Choose how much detail to see. Overview folds every function into a one-line AI summary."},
    {"layers-map", "Layers show the file, or the whole project, as parts with AI summaries. Zoom in for more detail, or click a part to jump there."},
    {"panel-tab:Problems", "Review tools report here: lint, static analysis, security, style, complexity and my own review."},
    {"panel-tab:Terminal", "A real terminal for your shell, like VS Code's."},
    {"ribbon-tab:My Tools", "Pin your favourite commands here, or add buttons that run your own shell commands."},
    {"agent-prompt", "Ask me anything. I can point at things like this, or click them for you if you want."},
};
constexpr int kTourSteps = int(sizeof(kTour) / sizeof(kTour[0]));
constexpr double kTourStepSeconds = 4.5;

} // namespace

PointerViewModel::PointerViewModel(NotepadViewModel& owner) : owner_(owner)
{
}

void PointerViewModel::pointAt(std::string target, std::string message, bool click, double seconds)
{
    CommandId id;
    if (target.rfind("command:", 0) == 0 && commandFromKey(std::string_view(target).substr(8), id))
        owner_.ribbonRevealRequested.emit(id);
    AgentPointer p;
    p.visible = true;
    p.target = std::move(target);
    p.message = std::move(message);
    p.click = click;
    p.serial = owner_.agentPointer.get().serial + 1;
    owner_.agentPointer.set(std::move(p));
    hideAt_ = seconds > 0.0 ? now_ + seconds : -1.0;
}

void PointerViewModel::hidePointer()
{
    AgentPointer p = owner_.agentPointer.get();
    p.visible = false;
    owner_.agentPointer.set(std::move(p));
    hideAt_ = -1.0;
}

void PointerViewModel::startTour()
{
    tourStep_ = 0;
    showTourStep();
}

void PointerViewModel::stopTour()
{
    tourStep_ = -1;
    hidePointer();
}

void PointerViewModel::showTourStep()
{
    const TourStep& step = kTour[tourStep_];
    if (std::string(step.target).rfind("panel-tab:", 0) == 0)
        owner_.showPanel.set(true);
    if (std::string(step.target) == "layers-map") {
        owner_.showLayersMap.set(true);
        owner_.sideBar.set(NotepadViewModel::SideBar::Layers);
    }
    if (std::string(step.target) == "agent-prompt" && owner_.workspace.get() == WorkspaceMode::Code)
        owner_.showAgentPanel.set(true);
    pointAt(step.target, std::string(step.message) + "  (" + std::to_string(tourStep_ + 1) + "/" + std::to_string(kTourSteps) + ")",
            false, 0.0);
    tourNextAt_ = now_ + kTourStepSeconds;
}

void PointerViewModel::guideFor(const std::string& prompt)
{
    // Stand-in for the agent deciding to show the user where things are.
    std::string p = prompt;
    for (char& c : p)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    struct Hint {
        const char* keyword;
        const char* target;
        const char* message;
    };
    static const Hint hints[] = {
        {"fold", "command:DetailOverview", "Here: pick a detail level and the code folds into AI summaries."},
        {"summar", "command:DetailOverview", "Here: Overview collapses every function into its summary."},
        {"detail", "command:DetailBalanced", "These buttons set how much detail you see."},
        {"map", "command:ToggleLayersMap", "This toggles the layers map beside the editor."},
        {"layer", "command:ToggleLayersMap", "This toggles the layers map beside the editor."},
        {"terminal", "command:ToggleTerminal", "Open the terminal here, or press Ctrl+`."},
        {"review", "command:RunAllReviews", "Run every review tool from here."},
        {"lint", "command:RunLint", "This runs the linter."},
        {"custom", "ribbon-tab:My Tools", "Your own buttons live in My Tools."},
        {"tour", "command:StartTour", "Start the tour here any time."},
    };
    for (const Hint& h : hints)
        if (p.find(h.keyword) != std::string::npos) {
            pointAt(h.target, h.message);
            return;
        }
}

void PointerViewModel::tick(double now)
{
    now_ = now;
    if (tourStep_ >= 0 && now >= tourNextAt_) {
        if (++tourStep_ >= kTourSteps)
            stopTour();
        else
            showTourStep();
    }
    if (hideAt_ > 0.0 && now >= hideAt_)
        hidePointer();
}

} // namespace enp::viewmodel
