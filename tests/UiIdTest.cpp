// Headless UI test: renders the real MainView without a GPU, sweeps the mouse across the window in
// both workspaces, every ribbon tab, every review tab and a set of layout states (no documents,
// light theme, find bar, word wrap, hidden panels), and fails when
//   - Dear ImGui detects items with conflicting IDs (the "2 visible items with conflicting ID"
//     error tooltip),
//   - Dear ImGui reports any other usage error (e.g. "SetCursorPos() to extend window boundaries"), or
//   - a title-bar drag region covers an interactive item (on Windows the OS would swallow clicks).
// ID conflicts are only detected for hovered items, which is why the mouse visits the whole window.
#include "enp/os/Os.h"
#include "enp/view/Fonts.h"
#include "enp/view/IWindowFrame.h"
#include "enp/view/MainView.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {

using namespace enp;

constexpr float kWidth = 1440.0f;
constexpr float kHeight = 900.0f;
constexpr float kStep = 10.0f;       // smaller than any clickable item
constexpr float kCoarseStep = 16.0f; // for the extra layout states

struct FakeFrame final : view::IWindowFrame {
    std::vector<view::ScreenRect> regions;
    bool customTitleBar() const override { return true; }
    bool isMaximized() const override { return false; }
    void minimize() override {}
    void toggleMaximize() override {}
    void close() override {}
    void setDragRegions(std::vector<view::ScreenRect> r) override { regions = std::move(r); }
    bool inDragRegion(ImVec2 p) const
    {
        for (const view::ScreenRect& r : regions)
            if (r.contains(p))
                return true;
        return false;
    }
};

struct Failure {
    std::string what;
    std::string where;
    ImVec2 mouse;
};

class Harness {
public:
    Harness()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(kWidth, kHeight);
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures; // textures are "uploaded" in endFrame()
        io.ConfigDebugHighlightIdConflicts = true;
        io.ConfigErrorRecoveryEnableAssert = false; // record usage errors instead of aborting
        io.ConfigErrorRecoveryEnableTooltip = true; // needed for ErrorCountCurrentFrame
        io.ConfigErrorRecoveryEnableDebugLog = true; // the message is read back from the debug log
        ImGui::GetStyle().FontSizeBase = view::Fonts::kUiSize;
        fonts_ = view::Fonts::load();
        vm_.openStartupDocuments({});
        view_ = std::make_unique<view::MainView>(vm_, fonts_, 1.0f, &frame_);
    }
    ~Harness()
    {
        view_.reset();
        ImGui::DestroyContext();
    }

    viewmodel::NotepadViewModel& vm() { return vm_; }
    const std::vector<Failure>& failures() const { return failures_; }
    int frames() const { return frames_; }

    void setContext(std::string where) { where_ = std::move(where); }

    // One frame with the mouse at `pos`; `button` is the left button state for this frame.
    void frame(ImVec2 pos, bool button = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(pos.x, pos.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, button);
        ImGui::NewFrame();
        // Conflicts are evaluated in NewFrame() for the item hovered in the previous frame.
        ImGuiContext& g = *GImGui;
        if (g.DebugDrawIdConflictsId != 0 && reportedIds_.insert(g.DebugDrawIdConflictsId).second)
            failures_.push_back({"conflicting ID 0x" + hex(g.DebugDrawIdConflictsId), where_, lastPos_});
        view_->render();
        if (g.HoveredId != 0 && frame_.inDragRegion(pos) && reportedIds_.insert(g.HoveredId).second)
            failures_.push_back({"item 0x" + hex(g.HoveredId) + " lies inside a title-bar drag region", where_, pos});
        ImGui::Render();
        if (g.ErrorCountCurrentFrame > 0) {
            const std::string message = lastError();
            if (reportedErrors_.insert(message + where_).second)
                failures_.push_back({"ImGui usage error: " + message, where_, pos});
        }
        g.DebugLogBuf.clear();
        g.DebugLogIndex.clear();
        endFrame();
        lastPos_ = pos;
        ++frames_;
    }

    void click(ImVec2 pos)
    {
        frame(pos);
        frame(pos, true);
        frame(pos, false);
    }

    // Visits every grid point of the rectangle, row by row.
    void sweep(float x0, float y0, float x1, float y1, float step = kStep)
    {
        for (float y = y0 + step * 0.5f; y < y1; y += step)
            for (float x = x0 + step * 0.5f; x < x1; x += step)
                frame(ImVec2(x, y));
    }

    void settle()
    {
        frame(ImVec2(-100.0f, -100.0f));
        frame(ImVec2(-100.0f, -100.0f));
    }

private:
    // Last "[imgui-error]" line of the debug log, without the prefix.
    static std::string lastError()
    {
        const std::string log = GImGui->DebugLogBuf.c_str();
        const std::string prefix = "[imgui-error] ";
        const size_t at = log.rfind(prefix + "In window");
        if (at == std::string::npos)
            return "(no message in debug log)";
        const size_t begin = at + prefix.size();
        const size_t end = log.find('\n', begin);
        return log.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    }

    static std::string hex(ImGuiID id)
    {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "%08X", id);
        return buffer;
    }

    // Stands in for a renderer backend: acknowledges texture create/update/destroy requests.
    static void endFrame()
    {
        for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
            if (tex->Status == ImTextureStatus_WantCreate || tex->Status == ImTextureStatus_WantUpdates) {
                tex->SetTexID(ImTextureID(1));
                tex->SetStatus(ImTextureStatus_OK);
            } else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0) {
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }
    }

    viewmodel::NotepadViewModel vm_;
    view::Fonts fonts_;
    FakeFrame frame_;
    std::unique_ptr<view::MainView> view_;
    std::vector<Failure> failures_;
    std::set<ImGuiID> reportedIds_;
    std::set<std::string> reportedErrors_;
    std::string where_;
    ImVec2 lastPos_{-1.0f, -1.0f};
    int frames_ = 0;
};

} // namespace

int main(int argc, char** argv)
{
    os::InitParams params;
    params.argc = argc;
    params.argv = argv;
    if (!os::init(params))
        return 1;

    int result = 0;
    {
        Harness h;
        h.frame(ImVec2(-100.0f, -100.0f)); // first frame lays everything out
        const float ribbonTop = 36.0f, ribbonBottom = 145.0f, tabRowY = ribbonTop + 15.0f;

        for (viewmodel::WorkspaceMode mode : {viewmodel::WorkspaceMode::Code, viewmodel::WorkspaceMode::Agent}) {
            h.vm().workspace.set(mode);
            const std::string modeName = mode == viewmodel::WorkspaceMode::Agent ? "Agent" : "Code";
            h.setContext(modeName + " workspace");
            h.frame(ImVec2(-100.0f, -100.0f));
            h.sweep(0.0f, 0.0f, kWidth, kHeight);

            // Ribbon tabs: click along the tab row, sweeping the ribbon whenever a new tab shows.
            for (float x = 110.0f; x < 760.0f; x += 30.0f) {
                h.click(ImVec2(x, tabRowY));
                h.setContext(modeName + " workspace, ribbon tab clicked at x=" + std::to_string(int(x)));
                h.sweep(0.0f, ribbonTop, kWidth, ribbonBottom);
            }
            for (viewmodel::ReviewTab tab : {viewmodel::ReviewTab::Problems, viewmodel::ReviewTab::Diff,
                                             viewmodel::ReviewTab::Comments, viewmodel::ReviewTab::Checklist}) {
                h.vm().reviewTab.set(tab);
                h.setContext(modeName + " workspace, review tab " + std::to_string(int(tab)));
                h.frame(ImVec2(-100.0f, -100.0f));
                h.frame(ImVec2(-100.0f, -100.0f));
                h.sweep(0.0f, kHeight - 260.0f, kWidth, kHeight);
            }
        }

        // Layout states that add, remove or move items; each is swept in both workspaces.
        struct State {
            const char* name;
            std::function<void(viewmodel::NotepadViewModel&)> apply;
        };
        const State states[] = {
            {"light theme", [](auto& vm) { vm.darkTheme.set(false); }},
            {"find and replace bar", [](auto& vm) {
                 vm.findQuery.set("word");
                 vm.execute(viewmodel::CommandId::Replace);
             }},
            {"word wrap + whitespace + 200% zoom", [](auto& vm) {
                 vm.wordWrap.set(true);
                 vm.showWhitespace.set(true);
                 vm.zoom.set(2.0f);
             }},
            {"side panels hidden", [](auto& vm) {
                 vm.showExplorer.set(false);
                 vm.showAgentPanel.set(false);
                 vm.showReviewPanel.set(false);
             }},
            {"no documents open", [](auto& vm) {
                 while (!vm.documents().empty())
                     vm.closeDocument(0);
             }},
        };
        for (const State& state : states) {
            state.apply(h.vm());
            for (viewmodel::WorkspaceMode mode : {viewmodel::WorkspaceMode::Code, viewmodel::WorkspaceMode::Agent}) {
                h.vm().workspace.set(mode);
                h.setContext(std::string(state.name) + ", " + (mode == viewmodel::WorkspaceMode::Agent ? "Agent" : "Code") +
                             " workspace");
                h.settle();
                h.sweep(0.0f, 0.0f, kWidth, kHeight, kCoarseStep);
            }
        }

        for (const Failure& f : h.failures())
            std::printf("  FAILED %s at (%.0f, %.0f) in %s\n", f.what.c_str(), f.mouse.x, f.mouse.y, f.where.c_str());
        std::printf("%d frames, %d failure(s)\n", h.frames(), int(h.failures().size()));
        result = h.failures().empty() ? 0 : 1;
    }
    os::shutdown();
    return result;
}
