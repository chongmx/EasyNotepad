// Headless UI test: renders the real MainView without a GPU, sweeps the mouse across the window in
// both workspaces, every ribbon tab, every review tab and a set of layout states (no documents,
// light theme, find bar, word wrap, hidden panels), and fails when
//   - Dear ImGui detects items with conflicting IDs (the "2 visible items with conflicting ID"
//     error tooltip),
//   - Dear ImGui reports any other usage error (e.g. "SetCursorPos() to extend window boundaries"), or
//   - a title-bar drag region covers an interactive item (on Windows the OS would swallow clicks).
// ID conflicts are only detected for hovered items, which is why the mouse visits the whole window.
#include "enp/os/Fs.h"
#include "enp/os/Os.h"
#include "enp/view/CodeGrid.h"
#include "enp/view/Fonts.h"
#include "enp/view/IWindowFrame.h"
#include "enp/view/MainView.h"
#include "enp/view/MathView.h"
#include "enp/view/UiAutomation.h"
#include "enp/viewmodel/DummyData.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "FakeTerminalService.h"
#include "ViewerFixtures.h"
#include "enp/interfaces/IFileService.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cctype>
#include <algorithm>
#include <cstdio>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
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

// Reads real files (the library view's fixtures) and nothing else: no folder is listed or written.
class ReadOnlyFiles final : public interfaces::IFileService {
public:
    // Files the test made up (the viewer's sample documents), served before the disk.
    std::map<std::string, std::string> memory;
    std::optional<std::string> read(const std::string& path, std::string* error) override
    {
        if (const auto it = memory.find(path); it != memory.end())
            return it->second;
        return os::fs::readFile(path, error);
    }
    std::optional<std::string> readRange(const std::string& path, std::uint64_t offset, size_t size, std::uint64_t* total) override
    {
        return os::fs::readFileRange(path, offset, size, total);
    }
    bool write(const std::string&, std::string_view, std::string*) override { return false; }
    std::vector<interfaces::DirectoryEntry> list(const std::string&) override { return {}; }
    bool isDirectory(const std::string&) override { return false; }
    std::string absolute(const std::string& path) override { return path; }
    bool createDirectories(const std::string&) override { return false; }
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
    ReadOnlyFiles& files() { return files_; }
    view::MainView& view() { return *view_; }
    const std::vector<Failure>& failures() const { return failures_; }
    int frames() const { return frames_; }

    void setContext(std::string where) { where_ = std::move(where); }
    // Following frames come from a finger (ImGui's touch-screen source) instead of a mouse.
    void setTouch(bool touch) { touch_ = touch; }

    // One frame with the mouse at `pos`; `button` is the left button state for this frame.
    void frame(ImVec2 pos, bool button = false)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMouseSourceEvent(touch_ ? ImGuiMouseSource_TouchScreen : ImGuiMouseSource_Mouse);
        io.AddMousePosEvent(pos.x, pos.y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, button);
        terminalService_.pump();
        ImGui::NewFrame();
        // Conflicts are evaluated in NewFrame() for the item hovered in the previous frame.
        ImGuiContext& g = *GImGui;
        if (g.DebugDrawIdConflictsId != 0 && reportedIds_.insert(g.DebugDrawIdConflictsId).second)
            failures_.push_back({"conflicting ID 0x" + hex(g.DebugDrawIdConflictsId), where_, lastPos_});
        view_->render();
        if (g.HoveredId != 0 && frame_.inDragRegion(pos) && reportedIds_.insert(g.HoveredId).second)
            failures_.push_back({"item 0x" + hex(g.HoveredId) + " (window \"" + std::string(g.HoveredWindow ? g.HoveredWindow->Name : "?") + "\") lies inside a title-bar drag region", where_, pos});
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

    tests::FakeTerminalService terminalService_; // declared before vm_, which uses it
    ReadOnlyFiles files_;
    viewmodel::NotepadViewModel vm_{&terminalService_, &files_};
    view::Fonts fonts_;
    FakeFrame frame_;
    std::unique_ptr<view::MainView> view_;
    std::vector<Failure> failures_;
    std::set<ImGuiID> reportedIds_;
    std::set<std::string> reportedErrors_;
    std::string where_;
    ImVec2 lastPos_{-1.0f, -1.0f};
    int frames_ = 0;
    bool touch_ = false;
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
        int checksFailed = 0; // behaviour checks (layout, touch, IntelliSense, ...) that failed
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
            for (viewmodel::PanelTab tab : {viewmodel::PanelTab::Problems, viewmodel::PanelTab::Diff,
                                             viewmodel::PanelTab::Comments, viewmodel::PanelTab::Checklist}) {
                h.vm().panelTab.set(tab);
                h.setContext(modeName + " workspace, review tab " + std::to_string(int(tab)));
                h.frame(ImVec2(-100.0f, -100.0f));
                h.frame(ImVec2(-100.0f, -100.0f));
                h.sweep(0.0f, kHeight - 260.0f, kWidth, kHeight);
            }
        }

        // Phone layouts: upright (bottom bar) and turned sideways (rail on the left), every screen.
        {
            using L = view::MainView::Layout;
            const struct {
                float w, h;
                L expected;
            } shapes[] = {{390, 844, L::Portrait}, {844, 390, L::Landscape}, {412, 915, L::Portrait}, {915, 412, L::Landscape},
                          {800, 1280, L::Desktop}, {1280, 800, L::Desktop}, {1440, 900, L::Desktop}, {560, 520, L::Landscape}};
            for (const auto& s : shapes)
                if (view::MainView::chooseLayout(s.w, s.h) != s.expected)
                    std::printf("  FAILED layout for %.0fx%.0f dp\n", s.w, s.h), ++checksFailed;
            h.vm().workspace.set(viewmodel::WorkspaceMode::Code);
            struct Phone {
                const char* name;
                ImVec2 size;
                bool rail;
            };
            for (const Phone& phone : {Phone{"phone upright", ImVec2(390.0f, 844.0f), false},
                                       Phone{"phone sideways", ImVec2(844.0f, 390.0f), true}}) {
                ImGui::GetIO().DisplaySize = phone.size;
                h.settle();
                for (int screen = 0; screen < 4; ++screen) {
                    // The navigation buttons: a bottom bar 58 high, or a rail 64 wide with 64-high cells.
                    const ImVec2 button = phone.rail ? ImVec2(32.0f, 36.0f + 32.0f + 64.0f * float(screen))
                                                     : ImVec2(phone.size.x * (float(screen) + 0.5f) / 4.0f, phone.size.y - 29.0f);
                    h.click(button);
                    h.setContext(std::string(phone.name) + ", screen " + std::to_string(screen));
                    h.settle();
                    h.sweep(0.0f, 0.0f, phone.size.x, phone.size.y, kCoarseStep);
                }
            }
            ImGui::GetIO().DisplaySize = ImVec2(kWidth, kHeight);
            h.settle();
        }

        // Touch: a finger drags content; a tap still clicks; a mouse drag selects as before.
        {
            // The scroll of the window whose own name (after the last '/') starts with `part`; a trailing
            // '#' means "followed by a digit" (a numbered window, not one with a longer name).
            auto scrollOf = [](const char* part, int axis) {
                std::string prefix = part;
                const bool digit = !prefix.empty() && prefix.back() == '#';
                if (digit)
                    prefix.pop_back();
                for (ImGuiWindow* w : GImGui->Windows) {
                    const char* slash = std::strrchr(w->Name, '/');
                    const char* own = slash ? slash + 1 : w->Name;
                    if (w->WasActive && std::strncmp(own, prefix.c_str(), prefix.size()) == 0 &&
                        (!digit || std::isdigit(static_cast<unsigned char>(own[prefix.size()]))))
                        return axis == 0 ? w->Scroll.x : w->Scroll.y;
                }
                return -1.0f;
            };
            auto drag = [&h](ImVec2 from, ImVec2 to) {
                h.frame(from);
                h.frame(from, true);
                for (int i = 1; i <= 12; ++i)
                    h.frame(ImVec2(from.x + (to.x - from.x) * float(i) / 12.0f, from.y + (to.y - from.y) * float(i) / 12.0f), true);
                h.frame(to, false);
            };
            auto check = [&](bool ok, const std::string& what) {
                if (!ok)
                    std::printf("  FAILED touch: %s\n", what.c_str()), ++checksFailed;
            };
            ImGui::GetIO().DisplaySize = ImVec2(390.0f, 844.0f);
            h.vm().execute(viewmodel::CommandId::NewFile); // 300 lines: plenty to scroll
            std::string text;
            for (int i = 1; i <= 300; ++i)
                text += "line " + std::to_string(i) + " of a long document\n";
            h.vm().insertText(text);
            h.vm().goToLine(0);
            h.settle();
            h.setTouch(true);
            h.click(ImVec2(390.0f * 1.5f / 4.0f, 844.0f - 29.0f)); // Code
            h.settle();
            const float before = scrollOf("##editorText", 1);
            drag(ImVec2(200.0f, 600.0f), ImVec2(200.0f, 300.0f));
            const float after = scrollOf("##editorText", 1);
            check(before >= 0.0f && after - before > 250.0f && after - before < 350.0f,
                  "a finger dragging 300 px up should scroll the editor about 300 px (" + std::to_string(after - before) + ")");
            // Momentum: it glides on for a moment after the finger lifts.
            for (int i = 0; i < 30; ++i)
                h.frame(ImVec2(200.0f, 300.0f));
            check(scrollOf("##editorText", 1) >= after, "the glide should not move backwards");

            // A tap on a navigation button still switches the screen.
            h.click(ImVec2(390.0f * 0.5f / 4.0f, 844.0f - 29.0f)); // Files
            h.settle();
            check(scrollOf("##explorer", 1) >= 0.0f, "a tap on Files should show the files");

            // A drag that starts on a ribbon button scrolls the ribbon and does not press the button.
            h.click(ImVec2(19.0f, 18.0f)); // the menu button: show the ribbon
            h.settle();
            const size_t documents = h.vm().documents().size();
            const float ribbonBefore = scrollOf("##ribbonGroups", 0);
            drag(ImVec2(130.0f, 95.0f), ImVec2(10.0f, 95.0f)); // starts on "New"
            h.settle();
            check(ribbonBefore >= 0.0f && scrollOf("##ribbonGroups", 0) > ribbonBefore + 50.0f, "a finger should scroll the ribbon");
            check(h.vm().documents().size() == documents, "scrolling from the New button should not create a document");

            // A sideways drag across the tab row (wider than a phone) scrolls the row, never the whole app.
            // (The chosen tab is scrolled into view, so the row may start at its end: drag back then.)
            const float tabsBefore = scrollOf("##ribbonTabs", 0);
            // (The row starts right of the workspace block, at about x = 104.)
            if (tabsBefore > 0.0f)
                drag(ImVec2(120.0f, 51.0f), ImVec2(370.0f, 51.0f));
            else
                drag(ImVec2(370.0f, 51.0f), ImVec2(120.0f, 51.0f));
            h.settle();
            check(tabsBefore >= 0.0f && std::fabs(scrollOf("##ribbonTabs", 0) - tabsBefore) > 50.0f,
                  "a finger should scroll the ribbon's tab row (" + std::to_string(tabsBefore) + " -> " +
                      std::to_string(scrollOf("##ribbonTabs", 0)) + ")");
            for (ImGuiWindow* w : GImGui->Windows)
                if (w->WasActive && !(w->Flags & ImGuiWindowFlags_ChildWindow) && std::strcmp(w->Name, "##main") == 0)
                    check(w->Scroll.x == 0.0f && w->Scroll.y == 0.0f, "the app itself must never be dragged sideways");

            // The plain mouse wheel (no Shift) scrolls the ribbon's rows sideways: the groups and the tabs.
            h.setTouch(false);
            auto wheel = [&](const char* window, ImVec2 at) {
                h.frame(at);
                const float before = scrollOf(window, 0);
                ImGui::GetIO().AddMouseWheelEvent(0.0f, before > 0.0f ? 3.0f : -3.0f); // towards the room there is
                h.frame(at);
                h.frame(at);
                check(before >= 0.0f && std::fabs(scrollOf(window, 0) - before) > 50.0f,
                      std::string("the mouse wheel should scroll ") + window + " sideways (" + std::to_string(before) + " -> " +
                          std::to_string(scrollOf(window, 0)) + ")");
            };
            wheel("##ribbonGroups", ImVec2(130.0f, 95.0f));
            wheel("##ribbonTabs", ImVec2(250.0f, 51.0f));
            for (ImGuiWindow* w : GImGui->Windows)
                if (w->WasActive && !(w->Flags & ImGuiWindowFlags_ChildWindow) && std::strcmp(w->Name, "##main") == 0)
                    check(w->Scroll.x == 0.0f && w->Scroll.y == 0.0f, "the mouse wheel must not scroll the app itself");

            // A mouse drag over the text selects, as it always did, and does not scroll.
            h.click(ImVec2(19.0f, 18.0f)); // hide the ribbon
            h.click(ImVec2(390.0f * 1.5f / 4.0f, 844.0f - 29.0f)); // Code
            h.settle();
            const float mouseBefore = scrollOf("##editorText", 1);
            drag(ImVec2(200.0f, 400.0f), ImVec2(200.0f, 300.0f));
            check(std::fabs(scrollOf("##editorText", 1) - mouseBefore) < 1.0f, "a mouse drag should not scroll");
            check(h.vm().activeDocument() && h.vm().activeDocument()->hasSelection(), "a mouse drag should select");
            ImGui::GetIO().DisplaySize = ImVec2(kWidth, kHeight);
            h.settle();
        }

        // Rendered Markdown is kept per document and must go when the document closes: every
        // opened folder shows its README, so this grew for as long as the app ran.
        {
            h.vm().workspace.set(viewmodel::WorkspaceMode::Code);
            h.vm().execute(viewmodel::CommandId::ShowCodeView);
            while (!h.vm().documents().empty())
                h.vm().closeDocument(0);
            for (int round = 0; round < 5; ++round) {
                h.vm().openFile(viewmodel::dummy::kWelcomePath); // Markdown: shown rendered
                h.settle();
                h.vm().closeDocument(h.vm().activeIndex.get());
                h.settle();
            }
            const size_t held = h.view().editor().renderedMarkdownCount();
            if (held != 0)
                std::printf("  FAILED rendered Markdown of %zu closed document(s) still held\n", held), ++checksFailed;

            // Two columns: drawn without ID conflicts, the keyboard moving between them; the
            // second column's view drops a closed document's caches, and goes when they join.
            h.setContext("two columns");
            h.vm().openFile(viewmodel::dummy::kWelcomePath); // Markdown, rendered
            h.vm().newDocument();                            // text, active
            h.vm().splitView.set(true);
            h.settle(); // the second column starts on the next document: the Markdown one
            if (!h.view().secondColumn())
                std::printf("  FAILED two columns: no second column\n"), ++checksFailed;
            const model::Document* text = h.vm().activeDocument();
            h.vm().execute(viewmodel::CommandId::FocusOtherColumn);
            h.settle();
            if (h.vm().activeDocument() == text)
                std::printf("  FAILED two columns: the other column's document did not become active\n"), ++checksFailed;
            h.vm().execute(viewmodel::CommandId::FocusOtherColumn);
            h.settle();
            if (h.vm().activeDocument() != text)
                std::printf("  FAILED two columns: the keyboard did not come back\n"), ++checksFailed;
            const size_t shownMarkdown = h.view().secondColumn() ? h.view().secondColumn()->renderedMarkdownCount() : 0;
            if (shownMarkdown != 1)
                std::printf("  FAILED two columns: the second column holds %zu rendered documents, not 1\n", shownMarkdown), ++checksFailed;
            h.vm().closeDocument(h.vm().documentIndexById(text->id) == 0 ? 1 : 0); // the Markdown one
            h.settle();
            if (h.view().secondColumn() && h.view().secondColumn()->renderedMarkdownCount() != 0)
                std::printf("  FAILED two columns: the second column kept a closed document's Markdown\n"), ++checksFailed;
            h.vm().splitView.set(false);
            h.settle();
            if (h.view().secondColumn())
                std::printf("  FAILED two columns: the second view was kept after joining\n"), ++checksFailed;
            while (!h.vm().documents().empty())
                h.vm().closeDocument(0);
            h.settle();

            // The title bar's layout buttons: each hides its pane and shows it again; the agent
            // panel's is off in the agent workspace, which has none.
            h.setContext("layout buttons");
            h.vm().workspace.set(viewmodel::WorkspaceMode::Code);
            h.settle();
            struct Pane {
                const char* id;
                viewmodel::Property<bool>& shown;
            };
            for (const Pane& pane : {Pane{"layout:explorer", h.vm().showExplorer}, Pane{"layout:panel", h.vm().showPanel},
                                     Pane{"layout:agent-panel", h.vm().showAgentPanel}}) {
                const bool before = pane.shown.get();
                for (int i = 0; i < 2; ++i) {
                    const view::UiElement* e = view::UiAutomation::instance().find(pane.id);
                    if (!e || !e->activate) {
                        std::printf("  FAILED layout buttons: no button %s\n", pane.id), ++checksFailed;
                        break;
                    }
                    const std::function<void()> action = e->activate; // the registry is rebuilt next frame
                    action();
                    h.settle();
                    if (pane.shown.get() != (i == 0 ? !before : before))
                        std::printf("  FAILED layout buttons: %s did not toggle its pane\n", pane.id), ++checksFailed;
                }
            }
            h.vm().workspace.set(viewmodel::WorkspaceMode::Agent);
            h.settle();
            if (const view::UiElement* e = view::UiAutomation::instance().find("layout:agent-panel"); !e || e->enabled)
                std::printf("  FAILED layout buttons: the agent panel's button is on in the agent workspace\n"), ++checksFailed;
            h.vm().workspace.set(viewmodel::WorkspaceMode::Code);
            h.settle();

            // The workspace switch: its icon goes on to the next workspace, and around; its chevron
            // lists them, and choosing one there switches to it.
            h.setContext("workspace switch");
            auto press = [&](const char* id) {
                const view::UiElement* e = view::UiAutomation::instance().find(id);
                if (!e || !e->activate) {
                    std::printf("  FAILED workspace switch: no control %s\n", id), ++checksFailed;
                    return;
                }
                const std::function<void()> action = e->activate;
                action();
                h.settle();
            };
            for (int i = 1; i <= viewmodel::kWorkspaceModeCount; ++i) {
                press("workspace-switch");
                const auto expected = viewmodel::WorkspaceMode((int(viewmodel::WorkspaceMode::Code) + i) % viewmodel::kWorkspaceModeCount);
                if (h.vm().workspace.get() != expected)
                    std::printf("  FAILED workspace switch: press %d did not go on to the next workspace\n", i), ++checksFailed;
            }
            press("workspace-menu");
            press("workspace:agent");
            if (h.vm().workspace.get() != viewmodel::WorkspaceMode::Agent)
                std::printf("  FAILED workspace switch: choosing Agent in the list did not switch to it\n"), ++checksFailed;
            press("workspace-menu");
            press("workspace:code");
            if (h.vm().workspace.get() != viewmodel::WorkspaceMode::Code)
                std::printf("  FAILED workspace switch: choosing Code in the list did not switch to it\n"), ++checksFailed;

            // Images, entities and formulas: drawn without ImGui errors, and the image textures
            // (decoded here from a data URL, as a file's bytes would be) go with the document.
            h.setContext("rendered Markdown with an image, entities and LaTeX");
            h.vm().newDocument();
            if (model::Document* doc = h.vm().activeDocument()) {
                // A 2x2 PPM: "P6 2 2 255" and twelve bytes of pixels, base64.
                const std::string ppmBase64 = "UDYKMiAyCjI1NQr/AAAA/wAAAP////8=";
                doc->buffer.setText("# Title &amp; &copy;\n\nInline $e^{i\\pi} + 1 = 0$, $\\frac{a}{b}$ and \\(\\sqrt{x}\\) text &rarr; done.\n\n"
                                    "$$\n\\sum_{i=0}^{n} i = \\frac{n(n+1)}{2} \\quad \\left( \\int_0^1 x\\,dx \\right)\n$$\n\n"
                                    "![dot](data:image/x-portable-pixmap;base64," + ppmBase64 + ")\n\n"
                                    "<img src=\"data:image/x-portable-pixmap;base64," + ppmBase64 + "\" alt=\"again\"> and ![missing](no/such.png)\n\n"
                                    "| $\\alpha$ | &beta; |\n|--|--|\n| ![x](data:image/x-portable-pixmap;base64," + ppmBase64 + ") | $x^2$ |\n");
                doc->language = model::Language::Markdown;
                doc->rendered = true;
            }
            h.settle();
            h.sweep(0.0f, 0.0f, kWidth, kHeight);
            const size_t textures = h.view().editor().imageTextureCount();
            if (textures == 0)
                std::printf("  FAILED the Markdown image was not decoded into a texture\n"), ++checksFailed;
            h.vm().closeDocument(h.vm().activeIndex.get());
            h.settle();
            const size_t left = h.view().editor().imageTextureCount();
            if (left != 0)
                std::printf("  FAILED %zu image texture(s) still held after the Markdown document closed\n", left), ++checksFailed;

            // A binary file and an image that cannot be decoded: the notices with "Open as text".
            h.setContext("binary and undecodable image documents");
            for (const auto kind : {model::FileKind::Binary, model::FileKind::Image}) {
                h.vm().newDocument();
                if (model::Document* doc = h.vm().activeDocument()) {
                    doc->kind = kind;
                    doc->format = kind == model::FileKind::Binary ? "ELF executable" : "WebP";
                    doc->path = kind == model::FileKind::Binary ? "bin/tool" : "img/photo.webp";
                    doc->diskSize = 123456;
                }
                h.settle();
                h.sweep(0.0f, 0.0f, kWidth, kHeight);
            }
            while (!h.vm().documents().empty())
                h.vm().closeDocument(0);
            h.settle();
            if (h.view().editor().imageDocumentCount() != 0 || h.view().editor().imageTextureCount() != 0)
                std::printf("  FAILED image documents still held after closing them\n"), ++checksFailed;

            // Emoji: drawn from the pack, and every picture (and the pack's index) let go after the
            // document closes and the app trims.
            h.setContext("rendered Markdown with emoji");
            h.vm().newDocument();
            if (model::Document* doc = h.vm().activeDocument()) {
                doc->buffer.setText("# Emoji \U0001F600\n\nArrow \u2B07\uFE0F, entity &#x2B07;&#xFE0F;, plain \u2B07, scientist "
                                    "\U0001F469\u200D\U0001F52C, thumbs \U0001F44D\U0001F3FD, flag \U0001F1F2\U0001F1FE, keycap #\uFE0F\u20E3, "
                                    "invented \U0001F600\u200D\U0001F680.\n\n| a | b |\n|--|--|\n| \u2764\uFE0F\u200D\U0001F525 | **\U0001F680** |\n");
                doc->language = model::Language::Markdown;
                doc->rendered = true;
            }
            h.settle();
            h.sweep(0.0f, 0.0f, kWidth, kHeight);
            if (h.view().emoji().textureCount() < 5)
                std::printf("  FAILED emoji: %zu pictures drawn, expected at least 5\n", h.view().emoji().textureCount()), ++checksFailed;
            h.vm().closeDocument(h.vm().activeIndex.get());
            h.settle();
            h.view().compactCaches(); // what the app's trim does after a close
            h.settle();
            if (h.view().emoji().textureCount() != 0 || h.view().emoji().indexLoaded())
                std::printf("  FAILED emoji: %zu picture(s) or the index still held after the document closed\n",
                            h.view().emoji().textureCount()),
                    ++checksFailed;

            // Code blocks: a grid of cells, as in a terminal. Box drawing and block elements are
            // drawn to fill their cells on whole pixels; wide characters and emoji take two cells;
            // tabs stop at multiples of 4; a long line wraps at a whole character.
            h.setContext("rendered Markdown code block on a grid");
            {
                auto expect = [&](bool ok, const char* what) {
                    if (!ok)
                        std::printf("  FAILED code grid: %s\n", what), ++checksFailed;
                };
                struct Placed {
                    int column, row, cells;
                    bool emoji;
                };
                std::string key;
                auto layout = [&](const std::string& line, int columns, int* rows = nullptr) {
                    std::vector<Placed> out;
                    const int r = view::layoutCodeLine(line, columns, key, [&](const view::CodeCell& c) {
                        out.push_back({c.column, c.row, c.cells, c.emoji != nullptr});
                    });
                    if (rows)
                        *rows = r;
                    return out;
                };
                std::vector<Placed> p = layout("┌─┐", 80);
                expect(p.size() == 3 && p[1].column == 1 && p[2].column == 2 && p[2].cells == 1, "box drawing should be one cell each");
                p = layout("中文ab", 80);
                expect(p.size() == 4 && p[1].column == 2 && p[2].column == 4 && p[3].column == 5, "CJK should be two cells each");
                p = layout("\U0001F600x", 80);
                expect(p.size() == 2 && p[0].emoji && p[0].cells == 2 && p[1].column == 2, "an emoji should be two cells");
                p = layout("ab\tx\ty", 80);
                expect(p.size() == 6 && p[3].column == 4 && p[5].column == 8, "tabs should stop at multiples of 4");
                int rows = 0;
                p = layout(std::string(10 * 3, ' ').replace(0, 30, "──────────"), 4, &rows);
                expect(rows == 3 && p.size() == 10 && p[4].row == 1 && p[4].column == 0, "ten cells on a 4-wide grid should wrap to 3 rows");
                p = layout("abc中", 4);
                expect(p.size() == 4 && p[3].row == 1 && p[3].column == 0, "a wide character should not be split by a wrap");
                int length = 0;
                expect(view::decodeUtf8("\xF0\x9F\x98\x80", 0, &length) == 0x1F600 && length == 4, "UTF-8 above U+FFFF should decode");

                // Drawn: every edge on a whole pixel (no half-covered pixel where cells or arms meet),
                // and a vertical line from the cell's top to its bottom, so rows join.
                ImDrawList dl(ImGui::GetDrawListSharedData());
                dl._ResetForNewFrame();
                dl._SetPixelDensity(1.0f); // as a window's list at 100% (FramebufferScale 1)
                dl.PushClipRect(ImVec2(0.0f, 0.0f), ImVec2(4096.0f, 4096.0f));
                for (uint32_t c = 0x2500; c <= 0x259F; ++c) {
                    if (c == 0x2571 || c == 0x2572 || c == 0x2573 || (c >= 0x256D && c <= 0x2570))
                        continue; // diagonals and arcs are anti-aliased strokes
                    const int vtx = dl.VtxBuffer.Size;
                    const ImVec2 cellMin(100.3f + 8.4f * float(c - 0x2500), 50.6f), cellMax(cellMin.x + 8.4f, 50.6f + 19.0f);
                    if (!view::drawBoxCharacter(&dl, c, cellMin, cellMax, IM_COL32(0, 0, 0, 255), 1.0f)) {
                        expect(false, "a box character was not drawn");
                        continue;
                    }
                    for (int i = vtx; i < dl.VtxBuffer.Size; ++i) {
                        const ImVec2 v = dl.VtxBuffer[i].pos;
                        if (v.x != std::floor(v.x) || v.y != std::floor(v.y)) {
                            std::printf("  FAILED code grid: U+%04X has an edge off the pixel grid (%.2f, %.2f)\n", c, v.x, v.y), ++checksFailed;
                            break;
                        }
                    }
                }
                const int vtx = dl.VtxBuffer.Size;
                view::drawBoxCharacter(&dl, 0x2502, ImVec2(10.0f, 20.2f), ImVec2(18.0f, 39.6f), IM_COL32(0, 0, 0, 255), 1.0f); // │
                float top = FLT_MAX, bottom = -FLT_MAX;
                for (int i = vtx; i < dl.VtxBuffer.Size; ++i)
                    top = std::min(top, dl.VtxBuffer[i].pos.y), bottom = std::max(bottom, dl.VtxBuffer[i].pos.y);
                expect(top == 20.0f && bottom == 40.0f, "a vertical line should span its whole (rounded) cell");
                expect(!view::drawBoxCharacter(&dl, 'A', ImVec2(0, 0), ImVec2(8, 16), IM_COL32(0, 0, 0, 255), 1.0f),
                       "a letter is not a box character");

                // In the app: a block of box drawing with an emoji, drawn without ImGui errors, the
                // emoji from the pack.
                h.vm().newDocument();
                if (model::Document* doc = h.vm().activeDocument()) {
                    doc->buffer.setText("# Grid\n\n```\n╔══╦══╗\n║ a║ b║ \U0001F680\n"
                                        "╚══╩══╝ █▓▒░ → 中\t|\n```\n");
                    doc->language = model::Language::Markdown;
                    doc->rendered = true;
                }
                h.settle();
                h.sweep(0.0f, 0.0f, kWidth, kHeight);
                expect(h.view().emoji().textureCount() >= 1, "the emoji in a code block should be drawn from the pack");
                h.vm().closeDocument(h.vm().activeIndex.get());
                h.settle();
                h.view().compactCaches();
                h.settle();
            }

            // Display math: \\ makes rows and the environments lay out their columns; a formula too
            // wide for its block breaks into lines before an operator instead of shrinking on one line.
            h.setContext("rendered Markdown math on several lines");
            {
                auto expect = [&](bool ok, const char* what) {
                    if (!ok)
                        std::printf("  FAILED math lines: %s\n", what), ++checksFailed;
                };
                view::MathStyle ms;
                ms.font = ImGui::GetIO().Fonts->Fonts[0];
                ms.size = 16.0f;
                ms.display = true;
                const view::MathBox one = view::measureMath(model::math::parse("a = b"), ms);
                const view::MathBox two = view::measureMath(model::math::parse("a = b \\\\ c = d"), ms);
                const view::MathBox al = view::measureMath(model::math::parse("\\begin{aligned} a &= b \\\\ cc &= d \\end{aligned}"), ms);
                expect(two.height() > one.height() * 1.8f && two.width < one.width * 1.2f, "\\\\ should start a new line");
                expect(al.height() > one.height() * 1.8f, "aligned should lay out its rows");
                const view::MathBox pm = view::measureMath(model::math::parse("\\begin{pmatrix} 1 & 2 \\\\ 3 & 4 \\\\ 5 & 6 \\end{pmatrix}"), ms);
                expect(pm.height() > one.height() * 2.5f, "a matrix should be as tall as its rows");
                std::string longSum = "S = a_1";
                for (int i = 2; i <= 40; ++i)
                    longSum += " + a_{" + std::to_string(i) + "}";
                const model::math::Node sum = model::math::parse(longSum);
                const view::MathBox flat = view::measureMath(sum, ms);
                const view::MathBox wrapped = view::measureMath(sum, ms, 300.0f);
                expect(flat.width > 300.0f && wrapped.width <= 300.0f, "a long formula should break to fit its width");
                expect(wrapped.height() > flat.height() * 2.0f, "a long formula should take several lines");
                expect(view::measureMath(model::math::parse("a = b"), ms, 300.0f).width == one.width, "a short formula should not change");

                // In the app: such formulas drawn without ImGui errors, in a narrow window too.
                h.vm().newDocument();
                if (model::Document* doc = h.vm().activeDocument()) {
                    doc->buffer.setText("# Math\n\n$$\n\\begin{aligned}\nf(x) &= (x+1)^2 \\\\\n&= x^2 + 2x + 1\n\\end{aligned}\n$$\n\n"
                                        "$$ |x| = \\begin{cases} x & x \\ge 0 \\\\ -x & \\text{otherwise} \\end{cases} $$\n\n"
                                        "$$ A = \\begin{bmatrix} 1 & 2 \\\\ 3 & 4 \\end{bmatrix} $$\n\n$$ " + longSum + " $$\n");
                    doc->language = model::Language::Markdown;
                    doc->rendered = true;
                }
                h.settle();
                h.sweep(0.0f, 0.0f, kWidth, kHeight);
                h.vm().closeDocument(h.vm().activeIndex.get());
                h.settle();
            }

            h.vm().openStartupDocuments({}); // the sample documents again for the states below
            h.settle();
        }

        // IntelliSense: the suggestion list is one window whatever the document, chosen with the keys
        // or the mouse; it gives the keyboard back to the text and nothing of it stays once its
        // documents close.
        {
            h.setContext("IntelliSense suggestions");
            h.vm().execute(viewmodel::CommandId::ShowCodeView);
            while (!h.vm().documents().empty())
                h.vm().closeDocument(0);
            viewmodel::IntellisenseViewModel& is = h.vm().intellisense();
            is.buildIndexNow(); // the sample workspace
            const ImVec2 away(-100.0f, -100.0f);
            auto windowNamed = [](const char* name) -> ImGuiWindow* {
                for (ImGuiWindow* w : GImGui->Windows) {
                    const char* slash = std::strrchr(w->Name, '/');
                    if (w->WasActive && std::strcmp(slash ? slash + 1 : w->Name, name) == 0)
                        return w;
                }
                return nullptr;
            };
            auto windowsContaining = [](const char* part) {
                int n = 0;
                for (ImGuiWindow* w : GImGui->Windows)
                    n += std::strstr(w->Name, part) ? 1 : 0;
                return n;
            };
            auto key = [&h, away](ImGuiKey k) {
                ImGui::GetIO().AddKeyEvent(k, true);
                h.frame(away);
                ImGui::GetIO().AddKeyEvent(k, false);
                h.frame(away);
            };
            auto fail = [&checksFailed](const char* what, int round) {
                std::printf("  FAILED IntelliSense (round %d): %s\n", round, what);
                ++checksFailed;
            };
            for (int round = 0; round < 6; ++round) {
                h.vm().newDocument();
                h.vm().activeDocument()->language = model::Language::Cpp;
                h.vm().insertText("int widthOfWindow = 3;\nint widthOfDoor = 2;\nint main()\n{\n    wid");
                h.vm().workspace.set(viewmodel::WorkspaceMode::Agent); // switching back focuses the editor
                h.vm().workspace.set(viewmodel::WorkspaceMode::Code);
                h.settle();
                h.vm().execute(viewmodel::CommandId::TriggerCompletion);
                h.settle();
                ImGuiWindow* popup = windowNamed("##completion");
                if (!popup || !is.active() || is.items().size() < 2) {
                    fail("the list did not show", round);
                    continue;
                }
                const ImVec2 at = popup->Pos, size = popup->Size;
                h.sweep(at.x, at.y, at.x + size.x, at.y + size.y, kCoarseStep); // hover every row
                if (round % 2 == 0) { // the keys
                    key(ImGuiKey_DownArrow);
                    if (is.selected() != 1)
                        fail("Down did not choose the next suggestion", round);
                    const std::string chosen = is.items()[1].label;
                    key(ImGuiKey_Enter);
                    if (is.active() || h.vm().activeDocument()->buffer.line(4) != "    " + chosen)
                        fail("Enter did not insert the chosen suggestion", round);
                } else { // the mouse
                    const std::string first = is.items()[0].label;
                    h.click(ImVec2(at.x + 40.0f, at.y + 14.0f));
                    h.settle();
                    if (is.active() || h.vm().activeDocument()->buffer.line(4) != "    " + first)
                        fail("a click did not insert the suggestion", round);
                    if (!GImGui->NavWindow || !std::strstr(GImGui->NavWindow->Name, "##editorText"))
                        fail("the keyboard did not go back to the text after a click", round);
                }
                // Escape closes a list; typing on afterwards works as before.
                h.vm().execute(viewmodel::CommandId::TriggerCompletion);
                h.settle();
                key(ImGuiKey_Escape);
                if (is.active())
                    fail("Escape did not close the list", round);
            }
            // Ctrl+click (Cmd+click on macOS) on a name goes to its definition; Alt+Left comes back
            // to the click. Run with this platform's key mapping and with macOS's on every platform:
            // with io.ConfigMacOSXBehaviors ImGui swaps the keys a backend reports (Cmd arrives as
            // Ctrl, the Ctrl key as Super) and turns a click with the Ctrl key into a right click,
            // so the test presses what a user presses there: Cmd (ImGuiMod_Super before the swap).
            for (const bool mac : {ImGui::GetIO().ConfigMacOSXBehaviors, !ImGui::GetIO().ConfigMacOSXBehaviors}) {
                h.setContext(mac ? "IntelliSense Cmd+click and Alt+Left (macOS keys)" : "IntelliSense Ctrl+click and Alt+Left");
                ImGuiIO& io = ImGui::GetIO();
                const bool platformMac = io.ConfigMacOSXBehaviors;
                io.ConfigMacOSXBehaviors = mac;
                const ImGuiKey command = mac ? ImGuiMod_Super : ImGuiMod_Ctrl; // the key the user presses
                h.vm().newDocument();
                h.vm().activeDocument()->language = model::Language::Cpp;
                h.vm().insertText("int x = helperFunction();\n\n\nint helperFunction() { return 1; }\n");
                h.vm().goToLine(0);
                h.vm().workspace.set(viewmodel::WorkspaceMode::Agent);
                h.vm().workspace.set(viewmodel::WorkspaceMode::Code);
                h.settle();
                const ImGuiWindow* text = nullptr; // a child window: its name is "<parent>/##editorText_<id>"
                for (ImGuiWindow* w : GImGui->Windows) {
                    const char* slash = std::strrchr(w->Name, '/');
                    if (w->WasActive && std::strncmp(slash ? slash + 1 : w->Name, "##editorText", 12) == 0)
                        text = w;
                }
                if (!text)
                    fail("the text window was not found", 0);
                bool jumped = false;
                io.AddKeyEvent(command, true);
                h.frame(away);
                for (float y = text ? text->Pos.y + 4.0f : 0.0f; text && y < text->Pos.y + 30.0f && !jumped; y += 4.0f)
                    for (float x = text->Pos.x + 30.0f; x < text->Pos.x + 420.0f && !jumped; x += 6.0f) {
                        h.click(ImVec2(x, y));
                        jumped = is.canGoBack();
                    }
                io.AddKeyEvent(command, false);
                h.frame(away);
                const model::Document* doc = h.vm().activeDocument();
                if (!jumped || !doc || doc->caret.line != 3)
                    fail("Ctrl+click did not go to the definition", 0);
                io.AddKeyEvent(ImGuiMod_Alt, true);
                key(ImGuiKey_LeftArrow);
                io.AddKeyEvent(ImGuiMod_Alt, false);
                h.frame(away);
                doc = h.vm().activeDocument();
                if (!doc || doc->caret.line != 0 || doc->caret.column != 8) // the start of the clicked name
                    fail("Alt+Left did not come back to where Ctrl+click started", 0);
                h.vm().closeDocument(h.vm().activeIndex.get());
                h.settle();
                io.ConfigMacOSXBehaviors = platformMac;
            }
            // Code from a comment: the "writing" row while a model works, then the code's preview;
            // Enter inserts it, indented like the comment. (A provider stands in for the model.)
            {
                h.setContext("IntelliSense code from a comment");
                std::vector<viewmodel::IntellisenseViewModel::Request> asked;
                viewmodel::IntellisenseViewModel::Provider provider;
                provider.request = [&asked](const viewmodel::IntellisenseViewModel::Request& r) { asked.push_back(r); };
                provider.completes = false;
                provider.implements = true;
                is.setProvider(provider);
                is.aiSuggestions.set(true);
                h.vm().newDocument();
                h.vm().activeDocument()->language = model::Language::Cpp;
                h.vm().insertText("int main()\n{\n    // print the numbers 1 to 3\n    \n}\n");
                h.vm().setCaret({3, 4}, false);
                h.vm().workspace.set(viewmodel::WorkspaceMode::Agent);
                h.vm().workspace.set(viewmodel::WorkspaceMode::Code);
                h.settle();
                h.vm().execute(viewmodel::CommandId::ImplementComment);
                h.settle();
                if (asked.empty() || !windowNamed("##completion"))
                    fail("the comment was not sent, or the writing row did not show", 0);
                if (const ImGuiWindow* row = windowNamed("##completion"))
                    h.sweep(row->Pos.x, row->Pos.y, row->Pos.x + row->Size.x, row->Pos.y + row->Size.y, kCoarseStep);
                viewmodel::IntellisenseViewModel::Item code;
                code.insertText = "for (int i = 1; i <= 3; ++i)\n    std::printf(\"%d\\n\", i);";
                if (!asked.empty())
                    is.provide(asked.back().serial, {code});
                h.settle();
                if (const ImGuiWindow* list = windowNamed("##completion"))
                    h.sweep(list->Pos.x, list->Pos.y, list->Pos.x + list->Size.x, list->Pos.y + list->Size.y, kCoarseStep);
                key(ImGuiKey_Enter);
                const model::Document* doc = h.vm().activeDocument();
                if (!doc || doc->buffer.line(3) != "    for (int i = 1; i <= 3; ++i)" || doc->buffer.line(4) != "        std::printf(\"%d\\n\", i);")
                    fail("Enter did not insert the code indented like the comment", 0);
                is.setProvider({});
                h.vm().codeAssistant().attach(); // the app's own assistant again
                is.aiSuggestions.set(false);
                h.vm().closeDocument(h.vm().activeIndex.get());
                h.settle();
            }
            // The IntelliSense settings: every control drawn and hovered without ImGui errors.
            {
                h.setContext("IntelliSense settings");
                for (int source = 0; source < 3; ++source) {
                    h.vm().codeAssistant().source.set(source);
                    h.vm().execute(viewmodel::CommandId::IntellisenseSettings);
                    h.settle();
                    if (const ImGuiWindow* dialog = windowNamed("IntelliSense Settings"))
                        h.sweep(dialog->Pos.x, dialog->Pos.y, dialog->Pos.x + dialog->Size.x, dialog->Pos.y + dialog->Size.y, kCoarseStep);
                    else
                        fail("the IntelliSense settings did not open", source);
                    key(ImGuiKey_Escape);
                    h.settle();
                }
                h.vm().codeAssistant().source.set(0);
            }
            if (windowsContaining("##completion") > 2) // the list and its scrolling child
                std::printf("  FAILED IntelliSense: %d windows for the suggestion list\n", windowsContaining("##completion")), ++checksFailed;
            h.vm().execute(viewmodel::CommandId::TriggerCompletion);
            h.settle();
            while (!h.vm().documents().empty())
                h.vm().closeDocument(0);
            h.settle();
            if (is.active() || !is.items().empty())
                std::printf("  FAILED IntelliSense: suggestions still held after their documents closed\n"), ++checksFailed;
            is.workspaceChanged(); // the index of the sample workspace goes too
            h.vm().openStartupDocuments({});
            h.settle();
        }

        // Data views: CSV table, JSON/YAML tree and accordion, a parse error; each document's parsed
        // form must go when it goes back to text, and when it closes.
        {
            h.setContext("data views");
            struct File { model::Language language; const char* text; };
            std::string csv = "id,name,note\n";
            for (int i = 0; i < 300; ++i)
                csv += std::to_string(i) + ",row " + std::to_string(i) + (i % 7 == 0 ? ",\"two\nlines\"\n" : ",plain\n");
            const std::string json = "{\"a\": [1, 2, {\"b\": \"x\"}], \"c\": {\"d\": null, \"e\": true}, \"long\": \"line one\\nline two\"}";
            const std::string yaml = "name: demo\nlist:\n  - 1\n  - two\nmap: {x: 1}\ntext: |\n  a\n  b\n";
            auto open = [&](model::Language language, const std::string& text) {
                h.vm().newDocument();
                model::Document* doc = h.vm().activeDocument();
                doc->buffer.setText(text);
                doc->language = language;
                return doc;
            };
            const view::DataViews& data = h.view().editor().dataViews();
            model::Document* doc = open(model::Language::Csv, csv);
            doc->rendered = true;
            h.settle();
            h.sweep(0.0f, 0.0f, kWidth, kHeight, kCoarseStep);
            for (const auto& [language, text] : {std::pair<model::Language, std::string>{model::Language::Json, json},
                                                 {model::Language::Yaml, yaml},
                                                 {model::Language::Json, std::string("{\n  \"a\": 1\n  \"b\": 2\n}")}}) {
                doc = open(language, text);
                for (const model::DataView view : {model::DataView::Tree, model::DataView::Accordion}) {
                    h.vm().setDataView(view);
                    h.settle();
                    h.sweep(0.0f, 0.0f, kWidth, kHeight, kCoarseStep);
                }
                const int id = doc->id;
                h.vm().setDataView(model::DataView::Text);
                h.settle();
                if (data.holds(id))
                    std::printf("  FAILED data views: a document back to text still holds its parsed form\n"), ++checksFailed;
            }
            if (data.entryCount() != 1) // the CSV table, still shown in its tab
                std::printf("  FAILED data views: %zu entries held, expected 1\n", data.entryCount()), ++checksFailed;
            while (!h.vm().documents().empty())
                h.vm().closeDocument(0);
            if (data.entryCount() != 0)
                std::printf("  FAILED data views: %zu entries held after every document closed\n", data.entryCount()), ++checksFailed;
            h.vm().openStartupDocuments({});
            h.settle();
        }

        // Word, Excel, PowerPoint and PDF documents (plan 25): each drawn and swept, its pages,
        // sheets and zoom gone through; everything it read and every picture it decoded must go
        // when it is opened as text, and when it closes.
        {
            h.setContext("document viewer");
            const view::ViewerView& viewer = h.view().editor().viewerView();
            std::vector<std::string> paths; // served from memory by the test's file service
            for (const auto& [name, bytes] : viewertest::sampleDocuments()) {
                paths.push_back("/enp-ui-viewer/" + name);
                h.files().memory[paths.back()] = bytes;
            }
            for (int round = 0; round < 2; ++round) {
                for (const std::string& path : paths) {
                    h.vm().openFile(path);
                    model::Document* doc = h.vm().activeDocument();
                    if (!doc || doc->kind != model::FileKind::Document) {
                        std::printf("  FAILED document viewer: %s did not open as a document\n", path.c_str());
                        ++checksFailed;
                        continue;
                    }
                    h.settle();
                    h.sweep(0.0f, 0.0f, kWidth, kHeight, kCoarseStep);
                    if (!viewer.holds(doc->id))
                        std::printf("  FAILED document viewer: %s was not read\n", path.c_str()), ++checksFailed;
                    for (const char* button : {"viewer:zoom-in", "viewer:zoom-out", "viewer:zoom-fit"})
                        if (const view::UiElement* e = view::UiAutomation::instance().find(button); e && e->activate) {
                            const std::function<void()> action = e->activate;
                            action();
                            h.settle();
                        }
                    h.sweep(0.0f, 0.0f, kWidth, kHeight, kCoarseStep);
                }
                if (viewer.entryCount() != paths.size())
                    std::printf("  FAILED document viewer: %zu documents held, expected %zu\n", viewer.entryCount(), paths.size()), ++checksFailed;
                if (round == 0) { // opened as text: what the viewer read goes at once
                    const int id = h.vm().activeDocument() ? h.vm().activeDocument()->id : -1;
                    h.vm().openAsText(h.vm().activeIndex.get());
                    h.settle();
                    if (viewer.holds(id))
                        std::printf("  FAILED document viewer: a document opened as text still holds what was read\n"), ++checksFailed;
                }
                while (!h.vm().documents().empty())
                    h.vm().closeDocument(0);
                h.settle();
                if (viewer.entryCount() != 0 || viewer.heldImageCount() != 0 || h.view().editor().imageTextureCount() != 0)
                    std::printf("  FAILED document viewer: %zu documents, %zu pictures, %zu textures held after closing them\n", viewer.entryCount(),
                                viewer.heldImageCount(), h.view().editor().imageTextureCount()),
                        ++checksFailed;
            }
            h.files().memory.clear();
            h.vm().openStartupDocuments({});
            h.settle();
        }

        // Libraries and programs: every list of each fixture drawn and swept, names shown both ways;
        // what a file contained must go when it is opened as text, and when it closes.
        {
            h.setContext("library view");
            const view::LibraryView& library = h.view().editor().libraryView();
            const std::string dir = std::string(ENP_SOURCE_DIR) + "/tests/data/binary/";
            for (const char* name : {"libfixture.so", "fixture.dll", "fixture.obj", "fixture.lib", "libfixture.a"}) {
                h.vm().newDocument();
                model::Document* doc = h.vm().activeDocument();
                doc->kind = model::FileKind::Binary;
                doc->path = dir + name;
                doc->title = name;
                doc->format = "library";
                h.settle();
                if (!library.holds(doc->id)) {
                    std::printf("  FAILED library view: %s was not read\n", name);
                    ++checksFailed;
                    continue;
                }
                // Every node of its tree, as a click on it would.
                std::vector<std::string> nodes;
                for (const view::UiElement& e : view::UiAutomation::instance().elements())
                    if (e.id.rfind("library:list:", 0) == 0)
                        nodes.push_back(e.id);
                if (nodes.size() < 3)
                    std::printf("  FAILED library view: %s shows %zu lists\n", name, nodes.size()), ++checksFailed;
                for (const std::string& id : nodes) {
                    const view::UiElement* e = view::UiAutomation::instance().find(id);
                    if (!e || !e->activate)
                        continue;
                    const std::function<void()> action = e->activate;
                    action();
                    h.settle();
                    h.sweep(0.0f, 0.0f, kWidth, kHeight, kCoarseStep);
                    if (const view::UiElement* demangle = view::UiAutomation::instance().find("library:demangle"); demangle && demangle->activate) {
                        const std::function<void()> toggle = demangle->activate;
                        toggle(); // names as stored
                        h.settle();
                        h.sweep(0.0f, 0.0f, kWidth, kHeight, kCoarseStep);
                        toggle();
                        h.settle();
                    }
                }
            }
            if (library.entryCount() != 5)
                std::printf("  FAILED library view: %zu entries held, expected 5\n", library.entryCount()), ++checksFailed;
            const int last = h.vm().activeDocument() ? h.vm().activeDocument()->id : -1;
            h.vm().openAsText(h.vm().activeIndex.get());
            h.settle();
            if (library.holds(last))
                std::printf("  FAILED library view: a file opened as text still holds what it contained\n"), ++checksFailed;
            while (!h.vm().documents().empty())
                h.vm().closeDocument(0);
            h.settle();
            if (library.entryCount() != 0)
                std::printf("  FAILED library view: %zu entries held after every document closed\n", library.entryCount()), ++checksFailed;
            h.vm().openStartupDocuments({});
            h.settle();
        }

        // Customize Ribbon: every section drawn and swept, and the Tabs section driven the way a
        // user would: hide a tab with its checkbox, move one, restore the defaults, close.
        {
            using viewmodel::WorkspaceMode;
            h.vm().workspace.set(WorkspaceMode::Code);
            auto activate = [&](const char* id) {
                const view::UiElement* e = view::UiAutomation::instance().find(id);
                if (!e || !e->activate) {
                    std::printf("  FAILED Customize Ribbon: no control %s\n", id);
                    ++checksFailed;
                    return;
                }
                const std::function<void()> action = e->activate; // the registry is rebuilt next frame
                action();
                h.settle();
            };
            h.setContext("Customize Ribbon dialog");
            h.vm().customizeRibbonRequested.emit();
            h.settle();
            for (const char* section : {"customize-ribbon:section:pinned", "customize-ribbon:section:actions", "customize-ribbon:section:tabs"}) {
                activate(section);
                h.sweep(0.0f, 0.0f, kWidth, kHeight, kCoarseStep);
            }
            activate("customize-ribbon:show:Read");
            const auto& code = h.vm().ribbonTabs(WorkspaceMode::Code);
            const bool readHidden = std::any_of(code.begin(), code.end(), [](const auto& t) { return t.name == "Read" && !t.visible; });
            activate("customize-ribbon:down:Home");
            const bool homeMoved = code.size() > 1 && code[0].name != "Home";
            if (!readHidden || !homeMoved)
                std::printf("  FAILED Customize Ribbon: hiding Read (%d) or moving Home (%d) did not apply\n", readHidden, homeMoved),
                    ++checksFailed;
            if (view::UiAutomation::instance().find("ribbon-tab:Read"))
                std::printf("  FAILED Customize Ribbon: the hidden Read tab is still on the ribbon\n"), ++checksFailed;
            activate("customize-ribbon:restore");
            if (!h.vm().ribbonTabs(WorkspaceMode::Code).empty() || !view::UiAutomation::instance().find("ribbon-tab:Read"))
                std::printf("  FAILED Customize Ribbon: Restore defaults did not bring the tabs back\n"), ++checksFailed;
            activate("customize-ribbon:done");
        }

        // The caret, selections and columns use the width the glyphs are drawn with: 120 cells end
        // where 120 drawn characters end (CalcTextSize rounds a width up to a whole pixel; a cell
        // measured with it put the caret a whole character right of the text at column 120).
        // Checked at every zoom the editor offers steps of.
        for (const float zoom : {1.0f, 1.1f, 1.25f, 1.5f, 2.0f, 0.9f}) {
            h.setContext("editor cell width");
            h.vm().newDocument();
            h.vm().zoom.set(zoom);
            h.settle();
            const float cell = h.view().editor().cellWidth();
            // The glyphs' own advances added up, as drawing does (no rounding).
            const std::string text(120, 'a');
            const float drawn = h.view().fonts().mono->CalcTextSizeA(h.view().editor().cellFontSize(), FLT_MAX, 0.0f, text.c_str()).x;
            if (cell <= 0.0f || std::fabs(cell * 120.0f - drawn) > 1.0f)
                std::printf("  FAILED editor cell width at %.0f%%: 120 cells %.1f px, 120 characters drawn %.1f px\n", zoom * 100.0f,
                            cell * 120.0f, drawn), ++checksFailed;
            h.vm().closeDocument(h.vm().activeIndex.get());
        }
        h.vm().zoom.set(1.0f);
        h.settle();

        // The map view's layout belongs to the map: once the map is gone (a folder closed), the
        // view frees it whichever view is shown, storage included; while the map exists it stays.
        {
            h.setContext("map view layout after the map is gone");
            h.vm().execute(viewmodel::CommandId::ShowMapView);
            h.settle();
            const size_t built = h.view().map().layoutBytes();
            h.vm().execute(viewmodel::CommandId::ShowCodeView);
            h.settle();
            const size_t kept = h.view().map().layoutBytes();
            if (built == 0 || kept != built)
                std::printf("  FAILED map view: layout %zu bytes when drawn, %zu after switching to code while the map exists\n", built, kept),
                    ++checksFailed;
            h.vm().logicalMap().workspaceChanged(); // what closing the folder does to the map
            h.settle();
            if (h.view().map().layoutBytes() != 0)
                std::printf("  FAILED map view: %zu bytes of the closed map's layout still held\n", h.view().map().layoutBytes()), ++checksFailed;
            h.vm().logicalMap().refresh(); // the demo map back for the layout states below
            h.settle();
        }

        // Layout states that add, remove or move items; each is swept in both workspaces.
        struct State {
            const char* name;
            std::function<void(viewmodel::NotepadViewModel&)> apply;
        };
        const State states[] = {
            {"developer mode (resource readout in the status bar)", [](auto& vm) { vm.developerMode.set(true); }},
            {"ribbon customized: Git first, Read and Review hidden",
             [](auto& vm) {
                 vm.setRibbonTabs(viewmodel::WorkspaceMode::Code, {{"Git", true}, {"Home", true}, {"Read", false}, {"Review", false}});
                 vm.setRibbonTabs(viewmodel::WorkspaceMode::Agent, {{"Read", false}, {"Review", false}});
             }},
            {"Overview detail (folded summaries)", [](auto& vm) { vm.detailLevel.set(2); }},
            {"Outline detail", [](auto& vm) { vm.detailLevel.set(3); }},
            {"layers: project scope, detailed, zoomed in", [](auto& vm) {
                 auto& layers = vm.layers();
                 layers.scope.set(viewmodel::LayerScope::Project);
                 layers.density.set(viewmodel::LayerDensity::Detailed);
                 for (int child : layers.nodes()[0].children)
                     if (layers.nodes()[size_t(child)].title == "src")
                         layers.zoomIn(child);
             }},
            {"layers: file scope, compact", [](auto& vm) {
                 vm.layers().scope.set(viewmodel::LayerScope::File);
                 vm.layers().density.set(viewmodel::LayerDensity::Compact);
             }},
            {"logical folding, logic scope", [](auto& vm) {
                 vm.execute(viewmodel::CommandId::FoldingLogical);
                 vm.layers().scope.set(viewmodel::LayerScope::Logic);
             }},
            {"map view", [](auto& vm) { vm.execute(viewmodel::CommandId::ShowMapView); }},
            {"map view zoomed into a module", [](auto& vm) {
                 auto& logic = vm.logicalMap();
                 logic.zoomTo(model::LogicalMap::kArchitectureRoot);
                 if (const model::LogicalNode* root = logic.map().find(logic.focus()); root && !root->children.empty())
                     logic.zoomIn(root->children.front());
             }},
            {"code view again", [](auto& vm) {
                 vm.execute(viewmodel::CommandId::ShowCodeView);
                 vm.execute(viewmodel::CommandId::FoldingSyntax);
             }},
            {"minimap side bar", [](auto& vm) { vm.execute(viewmodel::CommandId::ShowMinimap); }},
            {"side bar hidden", [](auto& vm) {
                 vm.showLayersMap.set(false);
                 vm.sideBar.set(viewmodel::NotepadViewModel::SideBar::Layers);
             }},
            {"light theme", [](auto& vm) {
                 vm.showLayersMap.set(true);
                 vm.detailLevel.set(1);
                 vm.darkTheme.set(false);
             }},
            // From here on every hint is drawn as glowing text and hovered controls glow: the
            // states after this one sweep the enhanced look, the ones before it the normal one.
            {"enhanced UI", [](auto& vm) { vm.enhancedUi.set(true); }},
            {"find and replace bar", [](auto& vm) {
                 vm.findQuery.set("word");
                 vm.execute(viewmodel::CommandId::Replace);
             }},
            {"word wrap + whitespace + 200% zoom", [](auto& vm) {
                 vm.wordWrap.set(true);
                 vm.showWhitespace.set(true);
                 vm.zoom.set(2.0f);
             }},
            {"terminal with two shells", [](auto& vm) {
                 vm.showPanelTab(viewmodel::PanelTab::Terminal);
                 vm.terminal().newTerminal(1);
             }},
            {"terminal popped out", [](auto& vm) { vm.execute(viewmodel::CommandId::PopOutTerminal); }},
            {"terminal back in the panel", [](auto& vm) { vm.execute(viewmodel::CommandId::PopOutTerminal); }},
            {"panel maximized", [](auto& vm) { vm.panelMaximized.set(true); }},
            {"side panels hidden", [](auto& vm) {
                 vm.panelMaximized.set(false);
                 vm.showExplorer.set(false);
                 vm.showAgentPanel.set(false);
                 vm.showPanel.set(false);
             }},
            {"many documents (scrolling tabs, namesakes)", [](auto& vm) {
                 vm.showExplorer.set(true);
                 for (int i = 0; i < 24; ++i)
                     vm.openFile("src/many/File" + std::to_string(i) + ".cpp");
                 vm.openFile("src/other/File3.cpp"); // a namesake
                 vm.activate(5);
             }},
            {"explorer: open documents tab", [](auto&) {
                 if (const view::UiElement* tab = view::UiAutomation::instance().find("explorer-tab:Open"))
                     tab->activate();
             }},
            {"explorer: git tab (demo data)", [](auto&) {
                 if (const view::UiElement* tab = view::UiAutomation::instance().find("explorer-tab:Git"))
                     tab->activate();
             }},
            {"git: several files selected, merge in progress", [](auto& vm) {
                 vm.git().setDemoMerge(true);
                 if (const view::UiElement* row = view::UiAutomation::instance().find("git:file:src/app/Main.cpp"))
                     row->activate();
             }},
            {"git: history log", [](auto& vm) {
                 vm.git().setDemoMerge(false);
                 vm.showPanelTab(viewmodel::PanelTab::History);
             }},
            {"git: history compare", [](auto&) {
                 if (const view::UiElement* tab = view::UiAutomation::instance().find("git:history-compare"))
                     tab->activate();
             }},
            {"git: status", [](auto& vm) { vm.execute(viewmodel::CommandId::ShowGitStatus); }},
            {"git: status, merge in progress, groups folded", [](auto& vm) {
                 vm.git().setDemoMerge(true);
                 if (const view::UiElement* group = view::UiAutomation::instance().find("git-status:group:Staged"))
                     (void)group;
             }},
            {"git: status back to normal", [](auto& vm) { vm.git().setDemoMerge(false); }},
            {"agent: local model set-up (open to everyone)", [](auto& vm) { vm.execute(viewmodel::CommandId::LocalModel); }},
            {"git: branch picker", [](auto& vm) { vm.execute(viewmodel::CommandId::GitBranches); }},
            {"git: learn git guide", [](auto& vm) { vm.execute(viewmodel::CommandId::GitGuide); }},
            {"git: guide closed", [](auto&) {
                 if (const view::UiElement* toggle = view::UiAutomation::instance().find("git:guide-toggle"))
                     toggle->activate();
             }},
            {"merge conflict markers in the editor", [](auto& vm) {
                 vm.newDocument();
                 vm.insertText("a\n<<<<<<< HEAD\nours\n=======\ntheirs\n>>>>>>> feature\nb\n<<<<<<< HEAD\nx\n||||||| base\ny\n=======\nz\n>>>>>>> other\n");
             }},
            {"search tab with results", [](auto& vm) {
                 vm.findInFiles().query = "int";
                 vm.findInFiles().searchNow();
                 vm.showPanelTab(viewmodel::PanelTab::Search);
             }},
            {"changed on disk bar", [](auto& vm) {
                 if (model::Document* doc = vm.activeDocument())
                     doc->diskState = model::Document::DiskState::Changed;
             }},
            {"deleted on disk bar", [](auto& vm) {
                 if (model::Document* doc = vm.activeDocument())
                     doc->diskState = model::Document::DiskState::Deleted;
             }},
            {"sha-256 dialog", [](auto& vm) { vm.hashResult.set(std::string(64, 'a')); }},
            {"language picker", [](auto& vm) {
                 vm.hashResult.set({});
                 if (model::Document* doc = vm.activeDocument())
                     doc->diskState = model::Document::DiskState::InSync;
                 vm.execute(viewmodel::CommandId::ChooseLanguage);
             }},
            {"open file dialog", [](auto& vm) { vm.fileDialog().show(viewmodel::FileDialogMode::Open, "."); }},
            {"save as dialog", [](auto& vm) { vm.fileDialog().show(viewmodel::FileDialogMode::SaveAs, ".", "name.txt"); }},
            {"save changes prompt", [](auto& vm) {
                 vm.fileDialog().cancel();
                 vm.openFile("src/prompt.cpp");
                 vm.insertText("x");
                 vm.requestCloseDocument(vm.activeIndex.get());
             }},
            {"quit prompt", [](auto& vm) {
                 vm.resolvePendingClose(viewmodel::NotepadViewModel::CloseChoice::Cancel);
                 vm.requestQuit();
             }},
            {"no documents open", [](auto& vm) {
                 vm.resolveQuit(viewmodel::NotepadViewModel::CloseChoice::Cancel);
                 while (!vm.documents().empty())
                     vm.closeDocument(0);
             }},
            {"slim ribbon", [](auto& vm) { vm.slimRibbon.set(true); }},
            {"syntax colours dialog (modal, last)", [](auto& vm) {
                 vm.openFile("CMakeLists.txt");
                 vm.execute(viewmodel::CommandId::SyntaxColors);
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

        // The automation registry's room is taken once (view::UiAutomation::kReservedElements): every
        // layout state drawn above must fit in it, or the room is too small and has to be raised.
        const std::size_t peak = view::UiAutomation::instance().peakElements();
        std::printf("automation registry: at most %zu elements in a frame, room for %zu\n", peak, view::UiAutomation::kReservedElements);
        if (peak > view::UiAutomation::kReservedElements)
            std::printf("  FAILED a frame registered %zu automation elements, more than the %zu reserved: raise kReservedElements\n", peak,
                        view::UiAutomation::kReservedElements),
                ++checksFailed;
        for (const Failure& f : h.failures())
            std::printf("  FAILED %s at (%.0f, %.0f) in %s\n", f.what.c_str(), f.mouse.x, f.mouse.y, f.where.c_str());
        std::printf("%d frames, %d ID failure(s), %d failed check(s)\n", h.frames(), int(h.failures().size()), checksFailed);
        result = h.failures().empty() && checksFailed == 0 ? 0 : 1;
    }
    os::shutdown();
    return result;
}
