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

private:
    std::vector<UiElement> building_;
    std::vector<UiElement> current_;
};

// Shorthand for registering the last submitted ImGui item.
void registerLastItem(std::string id, std::string role, std::string label, std::function<void()> activate,
                      bool enabled = true);

} // namespace enp::view
