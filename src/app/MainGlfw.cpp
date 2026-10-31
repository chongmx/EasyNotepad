// Entry point for Windows, Linux, macOS (GLFW + OpenGL 3) and the web (Emscripten GLFW + WebGL 2).
#include "Application.h"
#include "McpBridge.h"
#if !defined(__EMSCRIPTEN__)
#   include "GlfwWindowFrame.h"
#endif

#include "enp/os/Dispatch.h"
#include "enp/os/Os.h"

#include "imgui.h"
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
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

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
        // Idle: sleep until input or posted work, waking only for the caret blink if it is on.
        if (state->app->blinkCaret())
            glfwWaitEventsTimeout(0.5);
        else
            glfwWaitEvents();
        settleFrames = 2;
    }
#endif
#if !defined(__EMSCRIPTEN__)
    state->frame->update();
#endif
    if (glfwGetWindowAttrib(state->window, GLFW_ICONIFIED) != 0) {
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
    glViewport(0, 0, width, height);
    const ImVec4 clear = state->app->clearColor();
    glClearColor(clear.x, clear.y, clear.z, clear.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
#if !defined(__EMSCRIPTEN__)
    if (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
        GLFWwindow* context = glfwGetCurrentContext();
        ImGui::UpdatePlatformWindows();
        ImGui::RenderPlatformWindowsDefault();
        glfwMakeContextCurrent(context);
    }
#endif
    glfwSwapBuffers(state->window);

    if (state->app->quitRequested())
        glfwSetWindowShouldClose(state->window, GLFW_TRUE);
}

} // namespace

int main(int argc, char** argv)
{
    enp::os::InitParams params;
    params.argc = argc;
    params.argv = argv;
    if (!enp::os::init(params))
        return 1;
#if !defined(__EMSCRIPTEN__)
    // MCP connector mode: no window, JSON-RPC over stdio to the running app.
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--mcp") == 0) {
            const int code = enp::app::runMcpBridge(ENP_VERSION);
            enp::os::shutdown();
            return code;
        }
#endif

#if ENP_GLFW_HAS_PLATFORM && !defined(_WIN32) && !defined(__APPLE__) && !defined(__EMSCRIPTEN__)
    // Prefer X11 (also Xwayland) where there is a display: there the app can draw its own title bar;
    // pure Wayland gets the compositor's decorations.
    if (std::getenv("DISPLAY") && glfwPlatformSupported(GLFW_PLATFORM_X11))
        glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_X11);
#endif
    if (!glfwInit())
        return 1;
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
    GLFWwindow* window = glfwCreateWindow(int(1440 * scale), int(900 * scale), "EasyNotepad", nullptr, nullptr);
    if (!window) {
        glfwTerminate();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

#if !defined(__EMSCRIPTEN__)
    if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
        return 1;
#endif

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
#if !defined(__EMSCRIPTEN__)
    // Windows such as the popped-out terminal become real OS windows (multi-viewport).
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;
#endif
    ImGui_ImplGlfw_InitForOpenGL(window, true);
#if defined(__EMSCRIPTEN__)
    ImGui_ImplGlfw_InstallEmscriptenCallbacks(window, "#canvas");
#endif
    ImGui_ImplOpenGL3_Init(glslVersion);

    // Heap-allocated so it survives main() returning under emscripten_set_main_loop.
    auto* state = new AppState();
    g_state = state;
    state->window = window;
    enp::view::IWindowFrame* frame = nullptr;
#if !defined(__EMSCRIPTEN__)
    state->frame = std::make_unique<enp::app::GlfwWindowFrame>(window, customTitleBar);
    frame = state->frame.get();
#endif
    state->app = std::make_unique<enp::app::Application>();
    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    state->app->attachUi(scale, std::string(renderer ? renderer : "?") + " / " + (version ? version : "?"), frame);
    state->app->openInitialDocuments();
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
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    state->frame.reset();
    glfwDestroyWindow(window);
    glfwTerminate();
    g_state = nullptr;
    delete state;
    enp::os::shutdown();
#endif
    return 0;
}
