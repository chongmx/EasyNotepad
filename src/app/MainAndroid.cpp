// Entry point for Android (NativeActivity + EGL + OpenGL ES 3).
#include "Application.h"

#include "enp/os/Os.h"

#include "imgui.h"
#include "imgui_impl_android.h"
#include "imgui_impl_opengl3.h"

#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/configuration.h>
#include <android_native_app_glue.h>

#include <algorithm>
#include <memory>
#include <string>

namespace {

struct AndroidState {
    android_app* app = nullptr;
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    std::unique_ptr<enp::app::Application> application;
    bool uiReady = false;
    bool keyboardShown = false;
};

AndroidState g;

// Calls a no-argument method on our MainActivity (see android/app/src/main/java).
int callActivity(const char* method, const char* signature)
{
    JavaVM* vm = g.app->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        return 0;
    jobject activity = g.app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    jmethodID id = env->GetMethodID(cls, method, signature);
    int result = 0;
    if (id) {
        if (signature[2] == 'I')
            result = env->CallIntMethod(activity, id);
        else
            env->CallVoidMethod(activity, id);
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    vm->DetachCurrentThread();
    return result;
}

void pollUnicodeChars()
{
    ImGuiIO& io = ImGui::GetIO();
    while (int c = callActivity("pollUnicodeChar", "()I"))
        io.AddInputCharacter(unsigned(c));
}

float displayScale()
{
    int density = AConfiguration_getDensity(g.app->config);
    if (density <= 0 || density == ACONFIGURATION_DENSITY_ANY || density == ACONFIGURATION_DENSITY_NONE)
        return 2.0f;
    return float(density) / 160.0f;
}

void initUi()
{
    if (g.uiReady)
        return;
    ANativeWindow_acquire(g.app->window);

    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(g.display, nullptr, nullptr);
    const EGLint attributes[] = {EGL_BLUE_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_RED_SIZE, 8, EGL_DEPTH_SIZE, 0,
                                 EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT, EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    eglChooseConfig(g.display, attributes, &config, 1, &count);
    if (count == 0)
        return;
    EGLint format;
    eglGetConfigAttrib(g.display, config, EGL_NATIVE_VISUAL_ID, &format);
    ANativeWindow_setBuffersGeometry(g.app->window, 0, 0, format);
    const EGLint contextAttributes[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g.context = eglCreateContext(g.display, config, EGL_NO_CONTEXT, contextAttributes);
    g.surface = eglCreateWindowSurface(g.display, config, g.app->window, nullptr);
    eglMakeCurrent(g.display, g.surface, g.surface, g.context);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplAndroid_Init(g.app->window);
    ImGui_ImplOpenGL3_Init("#version 300 es");

    const char* renderer = reinterpret_cast<const char*>(glGetString(GL_RENDERER));
    g.application->attachUi(displayScale(), renderer ? renderer : "OpenGL ES 3");
    g.uiReady = true;
}

void shutdownUi()
{
    if (!g.uiReady)
        return;
    g.application->detachUi();
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplAndroid_Shutdown();
    ImGui::DestroyContext();

    eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g.context != EGL_NO_CONTEXT)
        eglDestroyContext(g.display, g.context);
    if (g.surface != EGL_NO_SURFACE)
        eglDestroySurface(g.display, g.surface);
    eglTerminate(g.display);
    g.display = EGL_NO_DISPLAY;
    g.context = EGL_NO_CONTEXT;
    g.surface = EGL_NO_SURFACE;
    ANativeWindow_release(g.app->window);
    g.uiReady = false;
}

// MainActivity.getSafeInsets(): {left, top, right, bottom} in pixels, -1 until Android reported them.
bool javaSafeInsets(int out[4])
{
    JavaVM* vm = g.app->activity->vm;
    JNIEnv* env = nullptr;
    if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
        return false;
    bool ok = false;
    jobject activity = g.app->activity->clazz;
    jclass cls = env->GetObjectClass(activity);
    if (jmethodID id = env->GetMethodID(cls, "getSafeInsets", "()[I")) {
        auto array = static_cast<jintArray>(env->CallObjectMethod(activity, id));
        if (array && env->GetArrayLength(array) == 4) {
            jint values[4];
            env->GetIntArrayRegion(array, 0, 4, values);
            ok = values[0] >= 0;
            for (int i = 0; i < 4; ++i)
                out[i] = values[i];
        }
    }
    if (env->ExceptionCheck())
        env->ExceptionClear();
    vm->DetachCurrentThread();
    return ok;
}

// The part of the window not covered by the status and navigation bars, a notch or the on-screen
// keyboard. Android 15 draws apps edge to edge, under the navigation bar, and NativeActivity's
// contentRect does not shrink for it: the window insets (MainActivity) are what tell. Checked every
// few frames; contentRect is the fallback until the insets are known.
// Rotation needs nothing else: the window is resized, ImGui reads its new size every frame, and
// the view picks the layout for that shape.
void updateSafeArea()
{
    static int frames = 0;
    static bool haveInsets = false;
    static int insets[4] = {0, 0, 0, 0};
    if (frames++ % 8 == 0)
        haveInsets = javaSafeInsets(insets);
    if (haveInsets) {
        g.application->setSafeArea(float(insets[0]), float(insets[1]), float(insets[2]), float(insets[3]));
        return;
    }
    const int width = ANativeWindow_getWidth(g.app->window);
    const int height = ANativeWindow_getHeight(g.app->window);
    const ARect& r = g.app->contentRect;
    if (r.right <= r.left || r.bottom <= r.top || width <= 0 || height <= 0) {
        g.application->setSafeArea(0, 0, 0, 0); // not reported yet
        return;
    }
    g.application->setSafeArea(float(std::max(0, r.left)), float(std::max(0, r.top)), float(std::max(0, width - r.right)),
                               float(std::max(0, height - r.bottom)));
}

void frame()
{
    if (!g.uiReady)
        return;
    updateSafeArea();
    ImGuiIO& io = ImGui::GetIO();
    pollUnicodeChars();
    if (io.WantTextInput != g.keyboardShown) {
        g.keyboardShown = io.WantTextInput;
        callActivity(g.keyboardShown ? "showSoftInput" : "hideSoftInput", "()V");
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplAndroid_NewFrame();
    ImGui::NewFrame();
    g.application->frame();
    ImGui::Render();

    glViewport(0, 0, int(io.DisplaySize.x), int(io.DisplaySize.y));
    const ImVec4 clear = g.application->clearColor();
    glClearColor(clear.x, clear.y, clear.z, clear.w);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    eglSwapBuffers(g.display, g.surface);
}

void onAppCommand(android_app*, int32_t command)
{
    switch (command) {
    case APP_CMD_INIT_WINDOW:
        initUi();
        break;
    case APP_CMD_TERM_WINDOW:
        shutdownUi();
        break;
    default:
        break;
    }
}

int32_t onInputEvent(android_app*, AInputEvent* event)
{
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY && AKeyEvent_getKeyCode(event) == AKEYCODE_BACK) {
        // Consume BACK while there is something to close; otherwise let the system close the activity.
        if (AKeyEvent_getAction(event) == AKEY_EVENT_ACTION_UP)
            return g.application->navigateBack() ? 1 : 0;
        return 1;
    }
    return g.uiReady ? ImGui_ImplAndroid_HandleInputEvent(event) : 0;
}

} // namespace

void android_main(android_app* app)
{
    g.app = app;
    app->onAppCmd = onAppCommand;
    app->onInputEvent = onInputEvent;

    // Each activity instance gets a fresh android_app, so (re)initialise everything per call;
    // the process may host several activity lifetimes.
    enp::os::InitParams params;
    params.nativeHandle = app;
    enp::os::init(params);
    g.application = std::make_unique<enp::app::Application>();
    g.application->openInitialDocuments();

    for (;;) {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(g.uiReady ? 0 : -1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0) {
            if (source)
                source->process(app, source);
            if (app->destroyRequested) {
                shutdownUi();
                g.application.reset();
                enp::os::shutdown();
                return;
            }
        }
        frame();
    }
}
