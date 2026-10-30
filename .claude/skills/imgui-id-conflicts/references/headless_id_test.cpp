// Template: headless Dear ImGui ID-conflict test (ImGui 1.91+, dynamic fonts 1.92+).
// Renders the real UI without a window or GPU, sweeps the mouse across it in every UI state,
// and fails when ImGui reports items with conflicting IDs or any other usage error
// (unbalanced Push/Pop, "SetCursorPos() to extend window boundaries", ...).
//
// Adapt the three marked places: font/UI setup, renderUi(), and the list of UI states in main().
// Build: link your view library + imgui core (imgui.cpp, imgui_draw.cpp, imgui_tables.cpp,
// imgui_widgets.cpp). No backend files are needed.
#include "imgui.h"
#include "imgui_internal.h" // GImGui->DebugDrawIdConflictsId

#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {

constexpr float kWidth = 1440.0f;
constexpr float kHeight = 900.0f;
constexpr float kStep = 10.0f; // must be smaller than the smallest clickable item

// ---- ADAPT: your root view --------------------------------------------------------------------
void renderUi()
{
    // e.g. mainView->render();
    ImGui::Begin("Demo");
    for (int i = 0; i < 3; ++i) {
        ImGui::PushID(i);
        ImGui::Button("Item");
        ImGui::PopID();
    }
    ImGui::End();
}

struct Failure {
    std::string what;
    ImVec2 mouse;
    std::string state;
};

class Harness {
public:
    Harness()
    {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = ImVec2(kWidth, kHeight);
        io.ConfigDebugHighlightIdConflicts = true;
        io.ConfigErrorRecoveryEnableAssert = false;  // record usage errors instead of aborting
        io.ConfigErrorRecoveryEnableTooltip = true;  // drives ErrorCountCurrentFrame
        io.ConfigErrorRecoveryEnableDebugLog = true; // message is read back from DebugLogBuf
        // 1.92+: pretend to be a renderer that supports dynamic texture updates (see endFrame()).
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        // ---- ADAPT: load the same fonts and style as the app, so the layout matches ----------
        io.Fonts->AddFontDefault();
    }
    ~Harness() { ImGui::DestroyContext(); }

    void setState(std::string state) { state_ = std::move(state); }

    void frame(ImVec2 mouse, bool leftDown = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(mouse.x, mouse.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, leftDown);
        ImGui::NewFrame();
        // NewFrame() evaluates the item hovered during the previous frame.
        const ImGuiID conflict = GImGui->DebugDrawIdConflictsId;
        if (conflict != 0 && reported_.insert(std::to_string(conflict)).second)
            failures_.push_back({"conflicting ID " + std::to_string(conflict), lastMouse_, state_});
        renderUi();
        ImGui::Render();
        if (GImGui->ErrorCountCurrentFrame > 0) {
            const std::string message = lastError();
            if (reported_.insert(message + state_).second)
                failures_.push_back({"usage error: " + message, mouse, state_});
        }
        GImGui->DebugLogBuf.clear();
        GImGui->DebugLogIndex.clear();
        endFrame();
        lastMouse_ = mouse;
    }

    void click(ImVec2 p)
    {
        frame(p);
        frame(p, true);
        frame(p, false);
    }

    void sweep(float x0, float y0, float x1, float y1)
    {
        for (float y = y0 + kStep * 0.5f; y < y1; y += kStep)
            for (float x = x0 + kStep * 0.5f; x < x1; x += kStep)
                frame(ImVec2(x, y));
    }

    const std::vector<Failure>& failures() const { return failures_; }

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

    // Stands in for the renderer backend: acknowledge texture create/update/destroy requests.
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

    std::vector<Failure> failures_;
    std::set<std::string> reported_;
    std::string state_;
    ImVec2 lastMouse_{-1.0f, -1.0f};
};

} // namespace

int main()
{
    Harness h;
    h.frame(ImVec2(-100.0f, -100.0f)); // first frame: layout only

    // ---- ADAPT: visit every UI state that changes what is on screen -----------------------------
    // e.g. for each mode: set it on the view-model, sweep the window; for each tab: click it
    // (h.click(pos)) and sweep the area it changes.
    h.setState("default");
    h.sweep(0.0f, 0.0f, kWidth, kHeight);

    for (const Failure& f : h.failures())
        std::printf("FAILED %s near (%.0f, %.0f) in state '%s'\n", f.what.c_str(), f.mouse.x, f.mouse.y,
                    f.state.c_str());
    std::printf("%d failure(s)\n", int(h.failures().size()));
    return h.failures().empty() ? 0 : 1;
}
