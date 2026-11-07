// Platform-independent application shell: composes services, view-model and view, and runs one
// UI frame at a time for the platform main loops.
#pragma once

#include "enp/services/AutomationServer.h"
#include "enp/services/ServiceContainer.h"
#include "enp/view/DocumentFonts.h"
#include "enp/view/Fonts.h"
#include "enp/view/IWindowFrame.h"
#include "enp/view/MainView.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <memory>
#include <string>
#include <vector>

namespace enp::app {

class Application {
public:
    // Requires os::init() to have been called.
    Application();
    ~Application();

    // Creates UI resources for the current ImGui context (fonts, style, view).
    // On Android this happens again whenever the native window is recreated. `frame` is the
    // native window when the view draws its own title bar; it must outlive the UI.
    void attachUi(float uiScale, std::string rendererInfo, view::IWindowFrame* frame = nullptr);
    void detachUi();

    // Opens the sample documents plus any launch arguments.
    void openInitialDocuments();
    void openPath(const std::string& path);
    // Android back button: returns false when there is nothing to go back from.
    bool navigateBack();

    // Builds the UI (between ImGui::NewFrame() and ImGui::Render()).
    void frame();
    // After the frame was rendered and presented: work that must not touch what the frame drew
    // (giving memory back once a folder or the last document closed).
    void endFrame();
    // Window edges covered by the system (bars, notch, on-screen keyboard), in pixels.
    void setSafeArea(float left, float top, float right, float bottom);

    ImVec4 clearColor() const;
    bool quitRequested() const;
    void requestQuit(); // asks about unsaved documents first
    // Something moves on screen by itself (agent at work, tour, pointer, analysis): keep drawing.
    bool animating() const;
    int maxFramesPerSecond() const; // 0: uncapped
    bool blinkCaret() const;

    // A screenshot was asked for (the automation's "screenshot"): after drawing this frame, the main
    // loop captures it (FrameCapture) and hands the pixels to deliverCapture, which answers.
    bool captureWanted() const { return !captures_.empty(); }
    void deliverCapture(bool ok, int width, int height, std::vector<unsigned char>& rgba, const std::string& failure);

private:
    services::ServiceContainer services_; // outlives the view-model that uses it
    std::unique_ptr<viewmodel::NotepadViewModel> viewModel_;
    // Frames left before memory is given back after a close (-1: none pending). Some releases
    // land a frame or two later (GPU textures go through the backend first), so the trim waits
    // for them, and the app keeps drawing until it has run.
    int trimFrames_ = -1;
    // A second trim once things have settled: results of background work (git, processes) and
    // textures the backend destroys late are freed in the seconds after a close.
    double lateTrimAt_ = -1.0;
    void giveMemoryBack();
    view::Fonts fonts_;
    view::ScriptFonts scriptFonts_; // Chinese, Japanese and Korean, while a document shown needs them
    view::DocumentFonts documentFonts_; // Calibri, Times New Roman, ...: while documents set in them are drawn
    std::unique_ptr<view::MainView> view_;
    // Local endpoint for the MCP connector (`EasyNotepad --mcp`); stopped before the view-model goes.
    std::unique_ptr<services::AutomationServer> automation_;
    struct Capture {
        std::string path; // also written there when set
        services::AutomationServer::Reply reply;
    };
    std::vector<Capture> captures_; // screenshots waiting for the next frame
};

} // namespace enp::app
