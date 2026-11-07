// Registry of the UI's interactive elements, rebuilt every frame: a stable id, a role, a label,
// the on-screen rectangle and the action that "clicking" performs. Agents (through the MCP
// connector) and the agent cursor use it to find and operate controls without synthesizing OS
// input, so the user's own mouse and keyboard are never touched.
#pragma once

#include "imgui.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::view {

struct UiElement {
    std::string id;    // "command:ToggleTerminal", "ribbon-tab:Read", "document:src/main.cpp", ...
    std::string role;  // "button", "tab", "file", "region", "panel", "textbox"
    std::string label; // what the user sees
    ImVec2 min;
    ImVec2 max;
    bool enabled = true;
    std::function<void()> activate; // performs the element's click; may be empty
};

class UiAutomation {
public:
    static UiAutomation& instance();

    void beginFrame();
    void endFrame(); // the frame's elements become current()
    void add(UiElement element);

    // Elements of the last complete frame.
    const std::vector<UiElement>& elements() const { return current_; }
    const UiElement* find(std::string_view id) const;

    // Elements each buffer has room for from the first frame on, kept for good. The UI test fails
    // when a frame of any layout state needs more (peakElements): then this is raised.
    static constexpr std::size_t kReservedElements = 512;
    // The most elements one frame has registered since the start (get_state reports it).
    std::size_t peakElements() const { return peak_; }

    // The automation's pointer resting at `at` for `seconds` (the hover tool): ImGui sees the mouse
    // there, so hover effects and hints show, while the user's own mouse is not moved. Applied once a
    // frame; the real mouse takes over again when it ends.
    void hover(ImVec2 at, double seconds);
    bool hovering() const { return hoverUntil_ > 0.0; }
    void applyHover();

private:
    ImVec2 hoverAt_{0.0f, 0.0f};
    double hoverUntil_ = 0.0;
    std::size_t peak_ = 0;
    std::vector<UiElement> building_;
    std::vector<UiElement> current_;
};

// The current window's title-bar × as element `id`, so agents can close the window (the agent
// cursor shows the click). Returns true once after such a click: the caller closes the window.
bool registerWindowClose(const std::string& id, const std::string& label);

// Shorthand for registering the last submitted ImGui item. In the enhanced UI a button, tab, toggle
// or checkbox glows while hovered; `boxGlow` false leaves the glow to the caller (ui::hoverLight).
void registerLastItem(std::string id, std::string role, std::string label, std::function<void()> activate,
                      bool enabled = true, bool boxGlow = true);

} // namespace enp::view
