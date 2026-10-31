#include "GlfwWindowFrame.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#if defined(_WIN32)
#   ifndef _WIN32_WINNT
#       define _WIN32_WINNT 0x0A00 // GetDpiForWindow / GetSystemMetricsForDpi
#   endif
// glfw3native.h includes <windows.h>: without NOMINMAX, MSVC's min/max macros break std::max.
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   define GLFW_EXPOSE_NATIVE_WIN32
#   include <GLFW/glfw3native.h>
#   include <dwmapi.h>
#   include <windowsx.h>
#endif

#include <algorithm>

#define ENP_GLFW_HAS_PLATFORM (GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4))

namespace enp::app {

namespace {

constexpr int kResizeBorder = 6; // pixels, manual mode
constexpr int kMinWidth = 640;
constexpr int kMinHeight = 400;

// Undecorated windows can only be moved by the application where GLFW can position them.
bool manualFrameSupported()
{
#if ENP_GLFW_HAS_PLATFORM && !defined(_WIN32) && !defined(__APPLE__)
    return glfwGetPlatform() == GLFW_PLATFORM_X11;
#else
    return false;
#endif
}

#if defined(_WIN32)

GlfwWindowFrame* g_frame = nullptr;
WNDPROC g_glfwProc = nullptr;

void frameThickness(HWND hwnd, int& x, int& y)
{
    const UINT dpi = GetDpiForWindow(hwnd);
    const int padding = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    x = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + padding;
    y = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + padding;
}

LRESULT CALLBACK captionWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_NCCALCSIZE:
        if (wParam == TRUE) {
            // Keep the left/right/bottom resize borders, give the caption to the client area.
            int fx = 0, fy = 0;
            frameThickness(hwnd, fx, fy);
            RECT& r = reinterpret_cast<NCCALCSIZE_PARAMS*>(lParam)->rgrc[0];
            r.left += fx;
            r.right -= fx;
            r.bottom -= fy;
            if (IsZoomed(hwnd)) // a maximized window overhangs the monitor by the frame size
                r.top += fy;
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        const LRESULT hit = CallWindowProcW(g_glfwProc, hwnd, message, wParam, lParam);
        if (hit != HTCLIENT)
            return hit; // side and bottom borders
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        ScreenToClient(hwnd, &pt);
        if (!IsZoomed(hwnd)) {
            int fx = 0, fy = 0;
            frameThickness(hwnd, fx, fy);
            RECT client;
            GetClientRect(hwnd, &client);
            if (pt.y < fy)
                return pt.x < fx * 2 ? HTTOPLEFT : pt.x >= client.right - fx * 2 ? HTTOPRIGHT : HTTOP;
        }
        if (g_frame && g_frame->inDragRegion(float(pt.x), float(pt.y)))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCACTIVATE:
        // Without a caption there is nothing to repaint; -1 stops the default frame flash.
        return DefWindowProcW(hwnd, message, wParam, -1);
    default:
        break;
    }
    return CallWindowProcW(g_glfwProc, hwnd, message, wParam, lParam);
}

#endif

} // namespace

void GlfwWindowFrame::prepareHints(bool wantCustomTitleBar)
{
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE); // shown once the frame is configured, without flicker
    if (wantCustomTitleBar && manualFrameSupported())
        glfwWindowHint(GLFW_DECORATED, GLFW_FALSE);
}

GlfwWindowFrame::GlfwWindowFrame(GLFWwindow* window, bool wantCustomTitleBar)
    : window_(window)
{
    if (wantCustomTitleBar) {
#if defined(_WIN32)
        HWND hwnd = glfwGetWin32Window(window_);
        g_frame = this;
        g_glfwProc = reinterpret_cast<WNDPROC>(
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&captionWindowProc)));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        mode_ = Mode::Win32Caption;
#else
        if (glfwGetWindowAttrib(window_, GLFW_DECORATED) == GLFW_FALSE)
            mode_ = Mode::Manual;
#endif
    }
    glfwShowWindow(window_);
}

GlfwWindowFrame::~GlfwWindowFrame()
{
#if defined(_WIN32)
    if (mode_ == Mode::Win32Caption) {
        SetWindowLongPtrW(glfwGetWin32Window(window_), GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(g_glfwProc));
        g_frame = nullptr;
    }
#endif
}

bool GlfwWindowFrame::isMaximized() const
{
    return glfwGetWindowAttrib(window_, GLFW_MAXIMIZED) == GLFW_TRUE;
}

void GlfwWindowFrame::minimize()
{
    glfwIconifyWindow(window_);
}

void GlfwWindowFrame::toggleMaximize()
{
    if (isMaximized())
        glfwRestoreWindow(window_);
    else
        glfwMaximizeWindow(window_);
}

void GlfwWindowFrame::close()
{
    glfwSetWindowShouldClose(window_, GLFW_TRUE);
}

bool GlfwWindowFrame::inDragRegion(float x, float y) const
{
    return std::any_of(dragRegions_.begin(), dragRegions_.end(),
                       [&](const view::ScreenRect& r) { return r.contains(ImVec2(x, y)); });
}

void GlfwWindowFrame::setDarkTheme(bool dark)
{
#if defined(_WIN32)
    HWND hwnd = glfwGetWin32Window(window_);
    const BOOL useDark = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &useDark, sizeof(useDark));
    const COLORREF border = dark ? RGB(52, 57, 65) : RGB(214, 219, 225);
    DwmSetWindowAttribute(hwnd, 34 /* DWMWA_BORDER_COLOR, Windows 11 */, &border, sizeof(border));
#else
    (void)dark;
#endif
}

// ---- Manual move / resize (X11) -----------------------------------------------------------------

int GlfwWindowFrame::edgeAt(double x, double y) const
{
    int w = 0, h = 0;
    glfwGetWindowSize(window_, &w, &h);
    int edge = None;
    if (x < kResizeBorder)
        edge |= Left;
    else if (x >= w - kResizeBorder)
        edge |= Right;
    if (y < kResizeBorder)
        edge |= Top;
    else if (y >= h - kResizeBorder)
        edge |= Bottom;
    return edge;
}

ImGuiMouseCursor GlfwWindowFrame::resizeCursor() const
{
    const int edge = resizing_ != None ? resizing_ : hoverEdge_;
    switch (edge) {
    case Left:
    case Right: return ImGuiMouseCursor_ResizeEW;
    case Top:
    case Bottom: return ImGuiMouseCursor_ResizeNS;
    case Top | Left:
    case Bottom | Right: return ImGuiMouseCursor_ResizeNWSE;
    case Top | Right:
    case Bottom | Left: return ImGuiMouseCursor_ResizeNESW;
    default: return ImGuiMouseCursor_None;
    }
}

void GlfwWindowFrame::beginDrag()
{
    if (mode_ != Mode::Manual || resizing_ != None)
        return;
    double cx = 0.0, cy = 0.0;
    glfwGetCursorPos(window_, &cx, &cy);
    int wx = 0, wy = 0;
    if (isMaximized()) {
        // Restore under the cursor, keeping its relative horizontal position.
        int maxW = 0, h = 0;
        glfwGetWindowSize(window_, &maxW, &h);
        glfwGetWindowPos(window_, &wx, &wy);
        glfwRestoreWindow(window_);
        int w = 0;
        glfwGetWindowSize(window_, &w, &h);
        const double ratio = maxW > 0 ? cx / maxW : 0.5;
        glfwSetWindowPos(window_, int(wx + cx - ratio * w), wy);
        cx = ratio * w;
    }
    glfwGetWindowPos(window_, &wx, &wy);
    grabX_ = wx + cx;
    grabY_ = wy + cy;
    startX_ = wx;
    startY_ = wy;
    dragging_ = true;
}

void GlfwWindowFrame::update()
{
    if (mode_ != Mode::Manual)
        return;
    double cx = 0.0, cy = 0.0;
    glfwGetCursorPos(window_, &cx, &cy);
    int wx = 0, wy = 0;
    glfwGetWindowPos(window_, &wx, &wy);
    const double sx = wx + cx, sy = wy + cy;
    const bool down = glfwGetMouseButton(window_, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const bool pressed = down && !mouseWasDown_;
    mouseWasDown_ = down;

    if (!down) {
        dragging_ = false;
        resizing_ = None;
    }
    if (!dragging_ && resizing_ == None)
        hoverEdge_ = isMaximized() ? None : edgeAt(cx, cy);
    if (pressed && hoverEdge_ != None) {
        resizing_ = hoverEdge_;
        grabX_ = sx;
        grabY_ = sy;
        startX_ = wx;
        startY_ = wy;
        glfwGetWindowSize(window_, &startW_, &startH_);
    }

    const int dx = int(sx - grabX_), dy = int(sy - grabY_);
    if (dragging_) {
        glfwSetWindowPos(window_, startX_ + dx, startY_ + dy);
    } else if (resizing_ != None) {
        int x = startX_, y = startY_, w = startW_, h = startH_;
        if (resizing_ & Left) {
            w = std::max(kMinWidth, startW_ - dx);
            x = startX_ + startW_ - w;
        }
        if (resizing_ & Right)
            w = std::max(kMinWidth, startW_ + dx);
        if (resizing_ & Top) {
            h = std::max(kMinHeight, startH_ - dy);
            y = startY_ + startH_ - h;
        }
        if (resizing_ & Bottom)
            h = std::max(kMinHeight, startH_ + dy);
        glfwSetWindowPos(window_, x, y);
        glfwSetWindowSize(window_, w, h);
    }
}

} // namespace enp::app
