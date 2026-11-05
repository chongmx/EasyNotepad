// Entry point for Windows, Linux, macOS (GLFW + OpenGL 3) and the web (Emscripten GLFW + WebGL 2).
#include "Application.h"
#include "McpBridge.h"
#if !defined(__EMSCRIPTEN__)
#   include "GlfwWindowFrame.h"
#endif

#include "enp/os/Dispatch.h"
#include "enp/os/Memory.h"
#include "enp/services/debug/MemoryTrace.h"
#include "enp/os/Os.h"

#include "FrameCapture.h"

#include "imgui.h"
#include "imgui_internal.h" // the input queue: whether anything arrived while the window was covered
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"

#if defined(__EMSCRIPTEN__)
#   include <GLES3/gl3.h>
#   include <emscripten.h>
#else
#   include <glad/glad.h>
#endif
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#ifndef ENP_GLFW_HAS_PLATFORM
#define ENP_GLFW_HAS_PLATFORM (GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4))
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

struct AppState {
    GLFWwindow* window = nullptr;
#if !defined(__EMSCRIPTEN__)
    std::unique_ptr<enp::app::GlfwWindowFrame> frame;
#endif
    std::unique_ptr<enp::app::Application> app;
};

AppState* g_state = nullptr;

#if !defined(__EMSCRIPTEN__)
void onDrop(GLFWwindow*, int count, const char** paths)
{
    if (g_state && count > 0)
        g_state->app->openPath(paths[0]);
}
#endif

void mainLoopStep(void* arg)
{
    auto* state = static_cast<AppState*>(arg);
#if defined(__EMSCRIPTEN__)
    glfwPollEvents();
#else
    // Idle: no input, posted work or animation for a moment -> sleep until something happens
    // (input, a posted task, or the caret blink). Software OpenGL (VMs, WSLg, remote desktops)
    // otherwise burns whole cores redrawing an unchanged screen.
    // Pointer or keyboard input keeps drawing for a second (hover effects, drags); anything else
    // that wakes the loop (posted work such as terminal output, the blink timeout) draws a couple
    // of frames so ImGui settles, then sleeps again.
    static double lastInput = 0.0;
    static int settleFrames = 3;
    static bool unshown = false; // the last frame was built while the window was covered, not presented
    const ImGuiIO& io = ImGui::GetIO();
    bool input = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f || io.MouseWheel != 0.0f ||
                 io.MouseWheelH != 0.0f || io.InputQueueCharacters.Size > 0 || ImGui::IsAnyItemActive();
    for (bool down : io.MouseDown)
        input = input || down;
    if (input)
        lastInput = glfwGetTime();
    if (glfwGetTime() - lastInput < 1.0 || settleFrames > 0 || state->app->animating()) {
        // Busy: no faster than the user's cap (software OpenGL ignores vsync and would spin).
        static double lastFrame = 0.0;
        const int fps = state->app->maxFramesPerSecond();
        const double wait = fps > 0 ? lastFrame + 1.0 / fps - glfwGetTime() : 0.0;
        if (wait > 0.0)
            glfwWaitEventsTimeout(wait);
        else
            glfwPollEvents();
        lastFrame = glfwGetTime();
        settleFrames = std::max(0, settleFrames - 1);
    } else {
        // Idle: sleep until input or posted work, waking only for the caret blink if it is on and
        // the window has the keyboard (no caret blinks in a window behind others). A frame presented
        // to a window that cannot be seen is kept by the driver until the window shows again (the
        // NVIDIA OpenGL driver: about 300 KB a frame; two blinks a second behind a browser grew the
        // app by 2 GB an hour).
        if (unshown) {
            // Covered with a frame not shown: look twice a second, and build a frame only when part of
            // the window shows again or something new arrives (input, posted work).
            glfwWaitEventsTimeout(0.5);
            if (!enp::os::dispatch::hasPending() && ImGui::GetCurrentContext()->InputEventsQueue.Size == 0 &&
                state->frame && state->frame->covered())
                return;
        } else if (state->app->blinkCaret() && glfwGetWindowAttrib(state->window, GLFW_FOCUSED)) {
            glfwWaitEventsTimeout(0.5);
        } else {
            glfwWaitEvents();
        }
        settleFrames = 2;
    }
#endif
#if !defined(__EMSCRIPTEN__)
    state->frame->update();
#endif
    // Minimized: nothing is drawn, but posted work (automation requests such as a screenshot) still
    // gets a frame, which is built and, when asked for, captured off screen, not presented.
    const bool minimized = glfwGetWindowAttrib(state->window, GLFW_ICONIFIED) != 0;
    if (minimized && !enp::os::dispatch::hasPending()) {
        ImGui_ImplGlfw_Sleep(10);
        return;
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    state->app->frame();
    ImGui::Render();

    int width = 0, height = 0;
    glfwGetFramebufferSize(state->window, &width, &height);
    const ImVec4 clear = state->app->clearColor();
#if !defined(__EMSCRIPTEN__)
    // Covered by other windows: the frame is built (posted work, state) but not drawn or presented.
    const bool covered = !minimized && state->frame && state->frame->covered();
#else
    const bool covered = false;
#endif
    if (!minimized && !covered) {
        enp::os::mem::Scope scope("gl-driver"); // a clear may compile a shader too
        glViewport(0, 0, width, height);
        glClearColor(clear.x, clear.y, clear.z, clear.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    }
    if (state->app->captureWanted()) { // a screenshot asked for through the automation endpoint
        std::vector<unsigned char> rgba;
        std::string failure;
        bool ok = false;
        {
            enp::os::mem::Scope scope("gl-driver"); // the driver's part; the pixels are released below
            ok = enp::app::captureFrame(ImGui::GetDrawData(), clear, width, height, rgba, &failure);
        }
        state->app->deliverCapture(ok, width, height, rgba, failure);
        glViewport(0, 0, width, height);
    }
    if (minimized) {
        state->app->endFrame();
        return;
    }
#if !defined(__EMSCRIPTEN__)
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* context = glfwGetCurrentContext();
        enp::os::mem::Scope scope("gl-driver"); // windows of popped-out panels: their contexts and draws
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(context);
    }
#endif
#if !defined(__EMSCRIPTEN__)
    unshown = covered;
#endif
    if (!covered) {
        enp::os::mem::Scope scope("gl-driver");
        glfwSwapBuffers(state->window);
    }
    state->app->endFrame();
#if !defined(__EMSCRIPTEN__)
    static int framesShown = 0;
    if (++framesShown == 2 && state->frame) // once, after the window has really been drawn
        state->frame->settle();
#endif

    if (state->app->quitRequested())
        glfwSetWindowShouldClose(state->window, GLFW_TRUE);
}

#if !defined(__EMSCRIPTEN__)
// --headless: the app without a window or OpenGL, for machines that have neither (CI runners;
// macOS virtual machines have no GPU, and GLFW asks for an accelerated OpenGL context). The same
// application, frames, automation connector and memory; nothing is drawn, and textures count as
// uploaded. For tests such as scripts/memory_cycle.py; the GPU driver's memory is not part of it.
int runHeadless()
{
    IMGUI_CHECKVERSION();
    ImGui::SetAllocatorFunctions(
        [](size_t size, void*) -> void* {
            enp::os::mem::Scope scope("imgui");
            return ::operator new(size);
        },
        [](void* p, void*) { ::operator delete(p); });
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1440.0f, 900.0f);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
    std::unique_ptr<enp::app::Application> app;
    {
        enp::os::mem::Scope scope("app");
        app = std::make_unique<enp::app::Application>();
    }
    {
        enp::os::mem::Scope scope("views");
        app->attachUi(1.0f, "headless (no window, no OpenGL)", nullptr);
    }
    app->openInitialDocuments();
    std::fprintf(stderr, "EasyNotepad: running headless\n");
    auto last = std::chrono::steady_clock::now();
    while (!app->quitRequested()) {
        // About 30 frames a second: requests from the automation connector are handled each frame.
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
        const auto now = std::chrono::steady_clock::now();
        io.DeltaTime = std::max(1e-4f, std::chrono::duration<float>(now - last).count());
        last = now;
        ImGui::NewFrame();
        app->frame();
        ImGui::Render();
        // No renderer: texture requests are acknowledged as done (as the UI tests do).
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                tex->SetTexID(ImTextureID(1));
                tex->SetStatus(ImTextureStatus_OK);
            } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
        if (app->captureWanted()) {
            std::vector<unsigned char> none;
            app->deliverCapture(false, 0, 0, none, "no window to capture in --headless mode");
        }
        app->endFrame();
    }
    app.reset();
    ImGui::DestroyContext();
    enp::os::shutdown();
    return 0;
}
#endif

} // namespace

int main(int argc, char** argv)
{
    enp::services::debug::installMemoryTrace(); // no-op unless built with ENP_MEMORY_TRACE
    enp::os::InitParams params;
    params.argc = argc;
    params.argv = argv;
    if (!enp::os::init(params)) {
        std::fprintf(stderr, "EasyNotepad: the platform layer could not start\n");
        return 1;
    }
#if !defined(__EMSCRIPTEN__)
    // MCP connector mode: no window, JSON-RPC over stdio to the running app.
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--mcp") == 0) {
            const int code = enp::app::runMcpBridge(ENP_VERSION);
            enp::os::shutdown();
            return code;
        }
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--headless") == 0)
            return runHeadless();
#endif

#if ENP_GLFW_HAS_PLATFORM && !defined(_WIN32) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    // Prefer X11 (also Xwayland) where there is a display: there the app can draw its own title bar;
    // pure Wayland gets the compositor's decorations.
    if (std::getenv("DISPLAY") && glfwPlatformSupported(GLFW_PLATFORM_X11))
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    // Why a window or GL context cannot be had (no display, no GPU, a driver without OpenGL 3.2)
    // is GLFW's to say: printed, so a start that fails is never silent.
    glfwSetErrorCallback([](int code, const char* description) {
        std::fprintf(stderr, "EasyNotepad: GLFW error 0x%X: %s\n", unsigned(code), description ? description : "");
    });
    if (!glfwInit()) {
        std::fprintf(stderr, "EasyNotepad: GLFW could not start (no display?)\n");
        return 1;
    }
#if !defined(__EMSCRIPTEN__)
    // Background work (terminal output, agent answers) wakes the idle loop.
    enp::os::dispatch::setWakeCallback([] { glfwPostEmptyEvent(); });
#endif

#if defined(__EMSCRIPTEN__)
    const char* glslVersion = "#version 300 es";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
#elif defined(__APPLE__)
    const char* glslVersion = "#version 150";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 2);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    const char* glslVersion = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#endif

#if !defined(__EMSCRIPTEN__)
    // The app draws its own title bar unless started with --native-titlebar.
    bool customTitleBar = true;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--native-titlebar") == 0)
            customTitleBar = false;
    enp::app::GlfwWindowFrame::prepareHints(customTitleBar);
#endif

    const float scale = ImGui_ImplGlfw_GetContentScaleForMonitor(glfwGetPrimaryMonitor());
    // 1440 x 900 (scaled), but never larger than the screen: a window taller than the work area is
    // clamped by the system, and its size and the app's idea of it disagree until the user resizes.
    int width = int(1440 * scale), height = int(900 * scale);
    int areaX = 0, areaY = 0, areaW = 0, areaH = 0;
#if !defined(__EMSCRIPTEN__)
    if (GLFWmonitor* monitor = glfwGetPrimaryMonitor())
        glfwGetMonitorWorkarea(monitor, &areaX, &areaY, &areaW, &areaH);
    if (areaW > 0 && areaH > 0) {
        width = std::min(width, int(areaW * 0.92f));
        height = std::min(height, int(areaH * 0.88f));
    }
#endif
    // --window-size=390x844 (in dp): a phone-sized window, to try the phone layouts.
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--window-size=", 14) == 0) {
            int w = 0, h = 0;
            if (std::sscanf(argv[i] + 14, "%dx%d", &w, &h) == 2 && w > 0 && h > 0) {
                width = int(float(w) * scale);
                height = int(float(h) * scale);
            }
        }
    // The GL driver's own C++ allocations (Mesa's LLVM registers itself when the driver library
    // loads; shaders are compiled on first draws) are counted as "gl-driver", not "other"
    // (docs/MEMORY.md section 2).
    std::optional<enp::os::mem::Scope> driverScope(std::in_place, "gl-driver");
    GLFWwindow* window = glfwCreateWindow(width, height, "EasyNotepad", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "EasyNotepad: no window with an OpenGL %s context could be created\n", glslVersion);
        glfwTerminate();
        return 1;
    }
#if !defined(__EMSCRIPTEN__)
    if (areaW > 0 && areaH > 0)
        glfwSetWindowPos(window, areaX + (areaW - width) / 2, areaY + (areaH - height) / 2);
#endif
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

#if !defined(__EMSCRIPTEN__)
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
        std::fprintf(stderr, "EasyNotepad: the OpenGL functions could not be loaded\n");
        return 1;
    }
#endif
    driverScope.reset();

    IMGUI_CHECKVERSION();
    ImGui::SetAllocatorFunctions(
        [](size_t size, void*) -> void* {
            enp::os::mem::Scope scope("imgui");
            return ::operator new(size);
        },
        [](void* p, void*) { ::operator delete(p); });
    ImGui::CreateContext();
#if !defined(__EMSCRIPTEN__)
    // Windows such as the popped-out terminal become real OS windows (multi-viewport).
    // --no-viewports keeps everything in the main window (to rule the platform windows out when
    // tracking a problem down).
    bool viewports = true;
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--no-viewports") == 0)
            viewports = false;
    if (viewports)
        ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
#endif
    ImGui_ImplGlfw_InitForOpenGL(window, true);
#if defined(__EMSCRIPTEN__)
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif
    {
        enp::os::mem::Scope scope("gl-driver");
        ImGui_ImplOpenGL3_Init(glslVersion);
    }

    // Owned by a static so it survives main() returning under emscripten_set_main_loop.
    static std::unique_ptr<AppState> stateOwner = std::make_unique<AppState>();
    AppState* state = stateOwner.get();
    g_state = state;
    state->window = window;
    enp::view::IWindowFrame* frame = nullptr;
#if !defined(__EMSCRIPTEN__)
    state->frame = std::make_unique<enp::app::GlfwWindowFrame>(window, customTitleBar);
    frame = state->frame.get();
#endif
    {
        // The view-model, its services, commands and settings: named, not "other" (subsystems
        // that have their own scope keep it inside this one).
        enp::os::mem::Scope scope("app");
        state->app = std::make_unique<enp::app::Application>();
    }
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    {
        enp::os::mem::Scope scope("views"); // the views are built here
        state->app->attachUi(scale, std::string(renderer ? renderer : "?") + " / " + (version ? version : "?"), frame);
    }
    state->app->openInitialDocuments();
    // --safe-area=left,top,right,bottom (in dp): pretend the system covers those edges, as a phone's
    // bars, notch and keyboard do.
    for (int i = 1; i < argc; ++i)
        if (std::strncmp(argv[i], "--safe-area=", 12) == 0) {
            float l = 0, t = 0, r = 0, b = 0;
            if (std::sscanf(argv[i] + 12, "%f,%f,%f,%f", &l, &t, &r, &b) == 4)
                state->app->setSafeArea(l * scale, t * scale, r * scale, b * scale);
        }
#if !defined(__EMSCRIPTEN__) // the browser GLFW ports do not all implement file drop
    glfwSetDropCallback(window, onDrop);
#endif

#if defined(__EMSCRIPTEN__)
    emscripten_set_main_loop_arg(mainLoopStep, state, 0, true);
#else
    // The OS close (native title bar, Alt+F4, taskbar) asks about unsaved documents first.
    glfwSetWindowCloseCallback(window, [](GLFWwindow* w) {
        if (g_state && !g_state->app->quitRequested()) {
            glfwSetWindowShouldClose(w, GLFW_FALSE);
            g_state->app->requestQuit();
        }
    });
    while (!glfwWindowShouldClose(window))
        mainLoopStep(state);

    enp::os::dispatch::setWakeCallback(nullptr);
    state->app.reset();
    // Undo in reverse order of set-up: the frame wrapped ImGui's window procedure (installed by
    // ImGui_ImplGlfw_Init), so it goes first. Restoring it after ImGui's shutdown would put back a
    // procedure whose context is gone, and destroying the window then crashes (on Windows a crash
    // report makes closing take seconds).
    state->frame.reset();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    g_state = nullptr;
    stateOwner.reset();
    enp::os::shutdown();
#endif
    return 0;
}
