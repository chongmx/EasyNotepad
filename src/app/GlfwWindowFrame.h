// IWindowFrame for GLFW desktop windows.
//   Windows: native frame kept, caption removed via WM_NCCALCSIZE; the view's top bar is reported
//            as the caption through WM_NCHITTEST, so snapping, resizing and shadows stay native.
//   X11:     undecorated window; move and edge-resize are implemented here.
//   macOS, Wayland, older GLFW: the native title bar is kept (customTitleBar() == false).
#pragma once

#include "enp/view/IWindowFrame.h"

struct GLFWwindow;

namespace enp::app {

class GlfwWindowFrame final : public view::IWindowFrame {
public:
    // Call between glfwInit() and glfwCreateWindow(): sets the window hints the frame needs.
    static void prepareHints(bool wantCustomTitleBar);

    GlfwWindowFrame(GLFWwindow* window, bool wantCustomTitleBar);
    ~GlfwWindowFrame() override;
    GlfwWindowFrame(const GlfwWindowFrame&) = delete;
    GlfwWindowFrame& operator=(const GlfwWindowFrame&) = delete;

    // Once per frame after glfwPollEvents(): drives manual move/resize where used.
    void update();

    bool customTitleBar() const override { return mode_ != Mode::Native; }
    bool isMaximized() const override;
    void minimize() override;
    void toggleMaximize() override;
    void close() override;
    // After the first frame: resize by a pixel and back. Some drivers keep the OpenGL surface at the
    // size it had before the caption was removed (the window was hidden then) until the window is
    // resized while visible; until then the top strip is cut off and clicks land a caption lower.
    void settle();
    void setDragRegions(std::vector<view::ScreenRect> regions) override { dragRegions_ = std::move(regions); }
    void beginDrag() override;
    ImGuiMouseCursor resizeCursor() const override;
    void setDarkTheme(bool dark) override;

    bool inDragRegion(float x, float y) const;

private:
    enum class Mode { Native, Win32Caption, Manual };
    enum Edge { None = 0, Left = 1, Right = 2, Top = 4, Bottom = 8 };

    int edgeAt(double x, double y) const;

    GLFWwindow* window_;
    Mode mode_ = Mode::Native;
    std::vector<view::ScreenRect> dragRegions_;

    // Manual move / resize state (Mode::Manual).
    bool dragging_ = false;
    int resizing_ = None;
    int hoverEdge_ = None;
    bool mouseWasDown_ = false;
    double grabX_ = 0.0, grabY_ = 0.0;           // cursor in screen coordinates at the press
    int startX_ = 0, startY_ = 0, startW_ = 0, startH_ = 0;
};

} // namespace enp::app
