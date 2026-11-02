#include "Application.h"

#include "AutomationController.h"

#include "enp/services/ImageEncoder.h"
#include "enp/view/TouchScroll.h"

#include "enp/os/Fs.h"
#include "enp/os/Memory.h"
#include "enp/os/Os.h"

#include "imgui_internal.h"

namespace enp::app {

Application::Application()
    : services_(services::ServiceContainer::createDefault()),
      viewModel_(std::make_unique<viewmodel::NotepadViewModel>(services_.terminal.get(), services_.files.get(),
                                                              services_.agentCli.get(), services_.processes.get(),
                                                              services_.localEngine.get()))
{
    if (const std::string dir = os::fs::appDataDirectory(); !dir.empty())
        viewModel_->setSettingsPath(os::fs::path::join(dir, "settings.json"));
    bool enabled = !os::isWeb();
    for (const std::string& arg : os::arguments())
        enabled = enabled && arg != "--no-automation";
    if (enabled) {
        automation_ = std::make_unique<services::AutomationServer>(
            [this](const std::string& method, const model::Json& params) { return handleAutomation(*viewModel_, method, params); },
            [this](const std::string& method, const model::Json& params, services::AutomationServer::Reply reply) {
                if (method != "screenshot")
                    return false;
                captures_.push_back(Capture{params.getString("path"), std::move(reply)}); // answered after this frame is drawn
                return true;
            });
        std::string error;
        if (!automation_->start(&error)) {
            ENP_LOG_WARN("automation endpoint unavailable: %s", error.c_str());
            automation_.reset();
        }
    }
}

Application::~Application()
{
    for (Capture& c : captures_)
        c.reply(model::Json::object().set("error", "the application is closing"));
    os::mem::release(captures_);
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
    if (view_ && view_->navigateBack())
        return true;
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
    if (view_) {
        os::mem::Scope scope("views"); // view caches; nested scopes (markdown, images, imgui) keep theirs
        view_->render();
    }
}

void Application::endFrame()
{
    const double now = os::now();
    if (viewModel_->takeMemoryTrimRequest()) {
        trimFrames_ = 4; // (re)start: textures released this frame are destroyed by the backend next frame, freed the one after
        lateTrimAt_ = now + 1.5;
    }
    if (trimFrames_ > 0 && --trimFrames_ == 0) {
        trimFrames_ = -1;
        giveMemoryBack();
    }
    if (lateTrimAt_ > 0.0 && now >= lateTrimAt_) {
        lateTrimAt_ = -1.0;
        giveMemoryBack();
    }
}

void Application::giveMemoryBack()
{
    if (view_)
        view_->compactCaches(); // emoji pictures no longer on screen
    // A folder or the last document closed. ImGui keeps every window's vertex buffers at the size
    // they once needed and every glyph ever drawn: shrink them to what is in use now (they grow
    // back on demand), then give the freed heap back to the OS so the release shows.
    if (ImGuiContext* g = ImGui::GetCurrentContext()) {
        for (ImGuiWindow* window : g->Windows)
            ImGui::GcCompactTransientWindowBuffers(window);
        // Tables: one not drawn in the last frame (a closed CSV's table) is freed whole; it is
        // created again when next drawn. The ones on screen give back their spare buffers.
        for (int i = 0; i < g->Tables.GetMapSize(); ++i)
            if (ImGuiTable* table = g->Tables.TryGetMapData(i)) {
                if (table->LastFrameActive < g->FrameCount - 1)
                    ImGui::TableRemove(table);
                else
                    ImGui::TableGcCompactTransientBuffers(table);
            }
        for (ImGuiTableTempData& temp : g->TablesTempData)
            ImGui::TableGcCompactTransientBuffers(&temp);
        ImGui::GcCompactTransientMiscBuffers();
        ImGui::GetIO().Fonts->CompactCache();
    }
    os::trimMemory();
}

namespace {

std::string base64(std::string_view data)
{
    static const char digits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = std::uint32_t(static_cast<unsigned char>(data[i])) << 16 |
                                (i + 1 < data.size() ? std::uint32_t(static_cast<unsigned char>(data[i + 1])) << 8 : 0u) |
                                (i + 2 < data.size() ? std::uint32_t(static_cast<unsigned char>(data[i + 2])) : 0u);
        out += digits[n >> 18 & 63];
        out += digits[n >> 12 & 63];
        out += i + 1 < data.size() ? digits[n >> 6 & 63] : '=';
        out += i + 2 < data.size() ? digits[n & 63] : '=';
    }
    return out;
}

} // namespace

void Application::deliverCapture(bool ok, int width, int height, std::vector<unsigned char>& rgba, const std::string& failure)
{
    std::vector<Capture> captures;
    captures.swap(captures_);
    std::string png = ok ? services::encodePng(rgba.data(), width, height) : std::string();
    os::mem::release(rgba); // the pixels go before the answers are built
    for (Capture& c : captures) {
        if (png.empty()) {
            c.reply(model::Json::object().set("error", ok ? std::string("the picture could not be encoded") : failure));
            continue;
        }
        model::Json result = model::Json::object().set("width", width).set("height", height).set("bytes", double(png.size()));
        if (!c.path.empty()) {
            std::string error;
            if (!os::fs::writeFile(c.path, png, &error)) {
                c.reply(model::Json::object().set("error", "cannot write " + c.path + ": " + error));
                continue;
            }
            result.set("path", c.path);
        }
        result.set("png_base64", base64(png));
        c.reply(std::move(result));
    }
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
    return vm.agent().busy.get() || vm.tourRunning() || vm.agentPointer.get().visible || view::touch::gliding() ||
           const_cast<viewmodel::NotepadViewModel&>(vm).logicalMap().analyzing.get() || trimFrames_ > 0 || lateTrimAt_ > 0.0;
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
