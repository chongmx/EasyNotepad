// Platform-independent application shell: composes services, view-model and view, and runs one
// UI frame at a time for the platform main loops.
#pragma once

#include "enp/services/AutomationServer.h"
#include "enp/services/ServiceContainer.h"
#include "enp/view/Fonts.h"
#include "enp/view/IWindowFrame.h"
#include "enp/view/MainView.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <memory>
#include <string>

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

    ImVec4 clearColor() const;
    bool quitRequested() const;
    void requestQuit(); // asks about unsaved documents first
    // Something moves on screen by itself (agent at work, tour, pointer, analysis): keep drawing.
    bool animating() const;
    int maxFramesPerSecond() const; // 0: uncapped
    bool blinkCaret() const;

private:
    services::ServiceContainer services_; // outlives the view-model that uses it
    std::unique_ptr<viewmodel::NotepadViewModel> viewModel_;
    view::Fonts fonts_;
    std::unique_ptr<view::MainView> view_;
    // Local endpoint for the MCP connector (`EasyNotepad --mcp`); stopped before the view-model goes.
    std::unique_ptr<services::AutomationServer> automation_;
};

} // namespace enp::app
