#include "Application.h"

#include "enp/os/Os.h"

namespace enp::app {

Application::Application()
    : viewModel_(std::make_unique<viewmodel::NotepadViewModel>())
{
}

Application::~Application()
{
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
    // "--workspace=agent|code" picks the starting workspace; other arguments are files.
    std::vector<std::string> files;
    for (const std::string& arg : os::arguments()) {
        if (arg == "--workspace=agent")
            viewModel_->workspace.set(viewmodel::WorkspaceMode::Agent);
        else if (arg == "--workspace=code")
            viewModel_->workspace.set(viewmodel::WorkspaceMode::Code);
        else if (arg.rfind("--", 0) != 0)
            files.push_back(arg);
    }
    viewModel_->openStartupDocuments(files);
}

void Application::openPath(const std::string& path)
{
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

bool Application::quitRequested() const
{
    return view_ && view_->quitRequested();
}

} // namespace enp::app
