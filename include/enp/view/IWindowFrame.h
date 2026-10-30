// Contract between the view and the native window when the view draws its own title bar.
// Implemented by the platform entry point (GlfwWindowFrame); absent on Android and the web.
#pragma once

#include "imgui.h"

#include <vector>

namespace enp::view {

struct ScreenRect {
    ImVec2 min;
    ImVec2 max;
    bool contains(ImVec2 p) const { return p.x >= min.x && p.x < max.x && p.y >= min.y && p.y < max.y; }
};

class IWindowFrame {
public:
    virtual ~IWindowFrame() = default;

    // True when the OS title bar is hidden: the view draws the caption and window buttons.
    virtual bool customTitleBar() const = 0;
    virtual bool isMaximized() const = 0;
    virtual void minimize() = 0;
    virtual void toggleMaximize() = 0;
    virtual void close() = 0;

    // Empty caption areas, in ImGui coordinates, where pressing the mouse moves the window.
    // The view reports them every frame.
    virtual void setDragRegions(std::vector<ScreenRect> regions) = 0;
    // Starts a window move from the current mouse position; used where the OS cannot hit-test
    // the caption itself (on Windows the press never reaches the view).
    virtual void beginDrag() {}
    // ImGuiMouseCursor to show for a resize edge under the mouse, or ImGuiMouseCursor_None.
    virtual ImGuiMouseCursor resizeCursor() const { return ImGuiMouseCursor_None; }
    // Lets the OS-drawn parts (border, shadow tint) follow the theme.
    virtual void setDarkTheme(bool dark) { (void)dark; }
};

} // namespace enp::view
