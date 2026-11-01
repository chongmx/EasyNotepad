#include "Application.h"

#include "AutomationController.h"

#include "enp/os/Fs.h"
#include "enp/os/Os.h"

namespace enp::app {

Application::Application()
    : services_(services::ServiceContainer::createDefault()),
      viewModel_(std::make_unique<viewmodel::NotepadViewModel>(services_.terminal.get(), services_.files.get(),
                                                              services_.agentCli.get(), services_.processes.get()))
{
    if (const std::string dir = os::fs::appDataDirectory(); !dir.empty())
        viewModel_->setSettingsPath(os::fs::path::join(dir, "settings.json"));
    bool enabled = !os::isWeb();
    for (const std::string& arg : os::arguments())
        enabled = enabled && arg != "--no-automation";
    if (enabled) {
        automation_ = std::make_unique<services::AutomationServer>(
            [this](const std::string& method, const model::Json& params) { return handleAutomation(*viewModel_, method, params); });
        std::string error;
        if (!automation_->start(&error)) {
            ENP_LOG_WARN("automation endpoint unavailable: %s", error.c_str());
            automation_.reset();
        }
    }
}

Application::~Application()
{
    automation_.reset(); // no requests may reach the view-model while it is destroyed
    detachUi();
}

void Application::attachUi(float uiScale, std::string rendererInfo, view::IWindowFrame* frame)
{
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // layout state will live in the settings service
    // Keyboard navigation stays off: arrow keys and Tab belong to the editor.

    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.PopupRounding = 6.0f;
    style.ChildRounding = 0.0f;
    style.WindowRounding = 6.0f;
    style.TabRounding = 4.0f;
    style.ScrollbarSize = os::isMobile() ? 6.0f : 11.0f;
    style.ScrollbarRounding = 6.0f;
    style.FramePadding = ImVec2(8.0f, 4.0f);
    style.ItemSpacing = ImVec2(6.0f, 5.0f);
    style.PopupBorderSize = 1.0f;
    style.TouchExtraPadding = os::isMobile() ? ImVec2(4.0f, 4.0f) : ImVec2(0.0f, 0.0f);
    style.ScaleAllSizes(uiScale);
    style.FontScaleDpi = uiScale;
    style.FontSizeBase = view::Fonts::kUiSize;

    fonts_ = view::Fonts::load();
    view_ = std::make_unique<view::MainView>(*viewModel_, fonts_, uiScale, frame);
    view_->setRendererInfo(std::move(rendererInfo));
}

void Application::detachUi()
{
    view_.reset();
}

void Application::openInitialDocuments()
{
    // "--workspace=agent|code" picks the starting workspace, "--panel=terminal" opens the terminal,
    // "--detail=Full|Balanced|Overview|Outline" sets the reading detail; other arguments are files.
    std::vector<std::string> files;
    bool openedFolder = false;
    bool terminal = false;
    bool tour = false;
    int detailLevel = -1;
    for (const std::string& arg : os::arguments()) {
        if (arg == "--workspace=agent")
            viewModel_->workspace.set(viewmodel::WorkspaceMode::Agent);
        else if (arg == "--workspace=code")
            viewModel_->workspace.set(viewmodel::WorkspaceMode::Code);
        else if (arg == "--panel=terminal")
            terminal = true;
        else if (arg == "--tour")
            tour = true;
        else if (arg.rfind("--detail=", 0) == 0) {
            for (int level = 0; level < viewmodel::NotepadViewModel::kDetailLevels; ++level)
                if (arg.substr(9) == viewmodel::NotepadViewModel::detailLevelName(level) ||
                    arg.substr(9) == std::to_string(level))
                    detailLevel = level;
        } else if (arg.rfind("--", 0) != 0) {
            if (os::fs::isDirectory(arg))
                openedFolder = viewModel_->openFolder(arg); // a folder argument becomes the workspace
            else
                files.push_back(arg);
        }
    }
    // Nothing asked for: carry on where the last session ended (else the sample workspace).
    if (openedFolder || !files.empty() || !viewModel_->restoreSession())
        viewModel_->openStartupDocuments(files);
    if (terminal)
        viewModel_->showPanelTab(viewmodel::PanelTab::Terminal);
    if (detailLevel >= 0)
        viewModel_->detailLevel.set(detailLevel);
    if (tour)
        viewModel_->startTour();
}

void Application::openPath(const std::string& path)
{
    if (os::fs::isDirectory(path))
        viewModel_->openFolder(path);
    else
        viewModel_->openFile(path);
}

bool Application::navigateBack()
{
    if (viewModel_->findVisible.get()) {
        viewModel_->findVisible.set(false);
        return true;
    }
    return false;
}

void Application::frame()
{
    os::dispatch::pump();
    viewModel_->tick(os::now());
    if (view_)
        view_->render();
}

ImVec4 Application::clearColor() const
{
    return view_ ? view_->clearColor() : ImVec4(0, 0, 0, 1);
}

void Application::setSafeArea(float left, float top, float right, float bottom)
{
    if (view_)
        view_->setSafeArea(left, top, right, bottom);
}

bool Application::animating() const
{
    const viewmodel::NotepadViewModel& vm = *viewModel_;
    return vm.agent().busy.get() || vm.tourRunning() || vm.agentPointer.get().visible ||
           const_cast<viewmodel::NotepadViewModel&>(vm).logicalMap().analyzing.get();
}

int Application::maxFramesPerSecond() const
{
    return viewModel_->maxFramesPerSecond.get();
}

bool Application::blinkCaret() const
{
    return viewModel_->blinkCaret.get();
}

void Application::requestQuit()
{
    viewModel_->requestQuit();
}

bool Application::quitRequested() const
{
    return view_ && view_->quitRequested();
}

} // namespace enp::app
