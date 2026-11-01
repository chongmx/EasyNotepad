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
#include "enp/view/UiAutomation.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include "FakeTerminalService.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <cctype>
#include <cstdio>
#include <cmath>
#include <cstring>
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
    viewmodel::NotepadViewModel vm_{&terminalService_};
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
        bool layoutsWrong = false;
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
                    std::printf("  FAILED layout for %.0fx%.0f dp\n", s.w, s.h), layoutsWrong = true;
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
            // The scroll of the window whose own name (after the last '/') starts with `part`; "##editor#"
            // means "##editor" followed by a digit (the editor itself, not "##editorPanel").
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
                    std::printf("  FAILED touch: %s\n", what.c_str()), layoutsWrong = true;
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
            const float before = scrollOf("##editor#", 1);
            drag(ImVec2(200.0f, 600.0f), ImVec2(200.0f, 300.0f));
            const float after = scrollOf("##editor#", 1);
            check(before >= 0.0f && after - before > 250.0f && after - before < 350.0f,
                  "a finger dragging 300 px up should scroll the editor about 300 px (" + std::to_string(after - before) + ")");
            // Momentum: it glides on for a moment after the finger lifts.
            for (int i = 0; i < 30; ++i)
                h.frame(ImVec2(200.0f, 300.0f));
            check(scrollOf("##editor#", 1) >= after, "the glide should not move backwards");

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

            // A mouse drag over the text selects, as it always did, and does not scroll.
            h.setTouch(false);
            h.click(ImVec2(19.0f, 18.0f)); // hide the ribbon
            h.click(ImVec2(390.0f * 1.5f / 4.0f, 844.0f - 29.0f)); // Code
            h.settle();
            const float mouseBefore = scrollOf("##editor#", 1);
            drag(ImVec2(200.0f, 400.0f), ImVec2(200.0f, 300.0f));
            check(std::fabs(scrollOf("##editor#", 1) - mouseBefore) < 1.0f, "a mouse drag should not scroll");
            check(h.vm().activeDocument() && h.vm().activeDocument()->hasSelection(), "a mouse drag should select");
            ImGui::GetIO().DisplaySize = ImVec2(kWidth, kHeight);
            h.settle();
        }

        // Layout states that add, remove or move items; each is swept in both workspaces.
        struct State {
            const char* name;
            std::function<void(viewmodel::NotepadViewModel&)> apply;
        };
        const State states[] = {
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

        for (const Failure& f : h.failures())
            std::printf("  FAILED %s at (%.0f, %.0f) in %s\n", f.what.c_str(), f.mouse.x, f.mouse.y, f.where.c_str());
        std::printf("%d frames, %d failure(s)\n", h.frames(), int(h.failures().size()));
        result = h.failures().empty() && !layoutsWrong ? 0 : 1;
    }
    os::shutdown();
    return result;
}
