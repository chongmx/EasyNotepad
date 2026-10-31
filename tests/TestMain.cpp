// Unit tests for the model, view-models and the pseudo-terminal backend (no framework dependency).
#include "FakeTerminalService.h"

#include "enp/model/Diff.h"
#include "enp/model/Document.h"
#include "enp/model/Highlighter.h"
#include "enp/model/Indent.h"
#include "enp/model/Language.h"
#include "enp/model/Outline.h"
#include "enp/model/TerminalScreen.h"
#include "enp/model/TextBuffer.h"
#include "enp/os/Process.h"
#include "enp/os/Pty.h"
#include "enp/viewmodel/DummyData.h"
#include "enp/viewmodel/NotepadViewModel.h"
#include "enp/model/Json.h"
#include "enp/model/LogicalMap.h"
#include "enp/interfaces/IAgentCliService.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Os.h"
#include "enp/services/AutomationServer.h"
#include "enp/services/FileService.h"

#include "AutomationController.h"
#include "McpBridge.h"

#include <atomic>
#include <map>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        ++g_checks;                                                                 \
        if (!(cond)) {                                                              \
            ++g_failures;                                                           \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond);         \
        }                                                                           \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        ++g_checks;                                                                                 \
        auto va = (a);                                                                              \
        auto vb = (b);                                                                              \
        if (!(va == vb)) {                                                                          \
            ++g_failures;                                                                           \
            std::printf("  FAILED %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);                 \
        }                                                                                           \
    } while (0)

using namespace enp;
using model::TextPos;
using viewmodel::CommandId;

std::vector<std::pair<const char*, std::function<void()>>>& registry()
{
    static std::vector<std::pair<const char*, std::function<void()>>> tests;
    return tests;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().emplace_back(name, std::move(fn)); }
};

#define TEST(name)                                       \
    static void name();                                  \
    static Register register_##name(#name, &name);       \
    static void name()

// A view-model with a single fresh document containing `text`.
struct Fixture {
    viewmodel::NotepadViewModel vm;
    explicit Fixture(const std::string& text)
    {
        vm.newDocument();
        vm.insertText(text);
        vm.setCaret({0, 0}, false);
    }
    std::string text() { return vm.activeDocument()->buffer.text(); }
};

// ---- TextBuffer ---------------------------------------------------------------------------------

TEST(buffer_splits_all_line_endings)
{
    model::TextBuffer b("a\r\nb\nc\rd");
    CHECK_EQ(b.lineCount(), 4);
    CHECK_EQ(b.line(3), std::string("d"));
    CHECK_EQ(b.text("\n"), std::string("a\nb\nc\nd"));
}

TEST(buffer_insert_and_erase_across_lines)
{
    model::TextBuffer b("hello world");
    TextPos end = b.insert({0, 5}, ",\nbig");
    CHECK_EQ(b.text(), std::string("hello,\nbig world"));
    CHECK(end == (TextPos{1, 3}));
    TextPos at = b.erase({0, 5}, {1, 3});
    CHECK_EQ(b.text(), std::string("hello world"));
    CHECK(at == (TextPos{0, 5}));
}

TEST(buffer_navigation_is_utf8_aware)
{
    model::TextBuffer b("a\xC3\xA4" "b"); // a, a-umlaut (2 bytes), b
    CHECK(b.next({0, 1}) == (TextPos{0, 3}));
    CHECK(b.prev({0, 3}) == (TextPos{0, 1}));
    CHECK(b.clamp({0, 2}) == (TextPos{0, 1})); // inside a code point
    CHECK_EQ(b.characterCount(), size_t(3));
    CHECK(b.next({0, 4}) == (TextPos{0, 4})); // end of buffer
}

TEST(buffer_word_boundaries)
{
    model::TextBuffer b("int value_1 = 42;");
    CHECK(b.wordStart({0, 7}) == (TextPos{0, 4}));
    CHECK(b.wordEnd({0, 7}) == (TextPos{0, 11}));
}

// ---- Diff ---------------------------------------------------------------------------------------

TEST(diff_pairs_changes_and_counts)
{
    const auto rows = model::diffLines({"a", "b", "c", "d"}, {"a", "B", "c", "d", "e"});
    const model::DiffStats stats = model::diffStats(rows);
    CHECK_EQ(stats.added, 2);
    CHECK_EQ(stats.removed, 1);
    CHECK(rows[1].kind == model::DiffKind::Changed);
    CHECK_EQ(rows[1].leftText, std::string("b"));
    CHECK_EQ(rows[1].rightText, std::string("B"));
    CHECK(rows.back().kind == model::DiffKind::Added);
    CHECK_EQ(rows.back().rightLine, 4);
}

TEST(diff_of_identical_text_has_no_changes)
{
    const auto rows = model::diffLines({"x", "y"}, {"x", "y"});
    CHECK_EQ(rows.size(), size_t(2));
    CHECK_EQ(model::diffStats(rows).added, 0);
}

TEST(language_detection_from_path)
{
    CHECK(model::languageFromPath("src/Main.CPP") == model::Language::Cpp);
    CHECK(model::languageFromPath("CMakeLists.txt") == model::Language::CMake);
    CHECK(model::languageFromPath("notes.txt") == model::Language::PlainText);
    CHECK(model::languageFromPath("tool.py") == model::Language::Python);
}

// ---- Editing ------------------------------------------------------------------------------------

TEST(typing_coalesces_into_one_undo_step)
{
    Fixture f("");
    for (char c : std::string("abc"))
        f.vm.insertText(std::string(1, c));
    CHECK_EQ(f.text(), std::string("abc"));
    f.vm.execute(CommandId::Undo);
    CHECK_EQ(f.text(), std::string(""));
    f.vm.execute(CommandId::Redo);
    CHECK_EQ(f.text(), std::string("abc"));
}

TEST(newline_keeps_indentation)
{
    Fixture f("    int x;");
    f.vm.setCaret({0, 10}, false);
    f.vm.newline();
    CHECK_EQ(f.text(), std::string("    int x;\n    "));
    CHECK(f.vm.activeDocument()->caret == (TextPos{1, 4}));
}

TEST(backspace_joins_lines_and_replaces_selection)
{
    Fixture f("ab\ncd");
    f.vm.setCaret({1, 0}, false);
    f.vm.backspace();
    CHECK_EQ(f.text(), std::string("abcd"));
    f.vm.setSelection({0, 1}, {0, 3});
    f.vm.insertText("X");
    CHECK_EQ(f.text(), std::string("aXd"));
}

TEST(line_operations)
{
    Fixture f("one\ntwo\nthree");
    f.vm.setCaret({1, 0}, false);
    f.vm.execute(CommandId::DuplicateLine);
    CHECK_EQ(f.text(), std::string("one\ntwo\ntwo\nthree"));
    f.vm.execute(CommandId::MoveLineUp);
    CHECK_EQ(f.vm.activeDocument()->caret.line, 1);
    f.vm.execute(CommandId::DeleteLine);
    CHECK_EQ(f.text(), std::string("one\ntwo\nthree"));
    f.vm.selectAll();
    f.vm.execute(CommandId::SortLines);
    CHECK_EQ(f.text(), std::string("one\nthree\ntwo"));
    f.vm.execute(CommandId::UpperCase);
    CHECK_EQ(f.text(), std::string("ONE\nTHREE\nTWO"));
}

TEST(toggle_comment_round_trips)
{
    Fixture f("int a;\n  int b;");
    f.vm.activeDocument()->language = model::Language::Cpp;
    f.vm.selectAll();
    f.vm.execute(CommandId::ToggleComment);
    CHECK_EQ(f.text(), std::string("// int a;\n//   int b;")); // aligned at the block's minimum indent
    f.vm.execute(CommandId::ToggleComment);
    CHECK_EQ(f.text(), std::string("int a;\n  int b;"));
}

TEST(find_and_replace_all)
{
    Fixture f("cat dog Cat\ncat");
    f.vm.findQuery.set("cat");
    CHECK_EQ(f.vm.findMatches().size(), size_t(3)); // case-insensitive by default
    f.vm.matchCase.set(true);
    CHECK_EQ(f.vm.findMatches().size(), size_t(2));
    f.vm.findNext(+1);
    CHECK_EQ(f.vm.currentMatch.get(), 0);
    CHECK_EQ(f.vm.selectedText(), std::string("cat"));
    f.vm.replaceText.set("bird");
    f.vm.replaceAll();
    CHECK_EQ(f.text(), std::string("bird dog Cat\nbird"));
}

TEST(save_clears_modified_and_compare_uses_saved_text)
{
    Fixture f("a\nb");
    CHECK(f.vm.activeDocument()->modified);
    f.vm.execute(CommandId::Save);
    CHECK(!f.vm.activeDocument()->modified);
    f.vm.setCaret({1, 1}, false);
    f.vm.insertText("c");
    f.vm.execute(CommandId::CompareWithSaved);
    CHECK(f.vm.panelTab.get() == viewmodel::PanelTab::Diff);
    CHECK_EQ(model::diffStats(f.vm.review().diff()).added, 1);
}

TEST(bookmarks_navigate_and_wrap)
{
    Fixture f("0\n1\n2\n3");
    f.vm.setCaret({1, 0}, false);
    f.vm.execute(CommandId::ToggleBookmark);
    f.vm.setCaret({3, 0}, false);
    f.vm.execute(CommandId::ToggleBookmark);
    f.vm.setCaret({0, 0}, false);
    f.vm.execute(CommandId::NextBookmark);
    CHECK_EQ(f.vm.activeDocument()->caret.line, 1);
    f.vm.execute(CommandId::NextBookmark);
    CHECK_EQ(f.vm.activeDocument()->caret.line, 3);
    f.vm.execute(CommandId::NextBookmark);
    CHECK_EQ(f.vm.activeDocument()->caret.line, 1);
}

// ---- Workspace, review, agent -------------------------------------------------------------------

TEST(workspace_mode_toggles_and_reports_checked_state)
{
    viewmodel::NotepadViewModel vm;
    CHECK(vm.workspace.get() == viewmodel::WorkspaceMode::Code);
    vm.execute(CommandId::ToggleWorkspaceMode);
    CHECK(vm.workspace.get() == viewmodel::WorkspaceMode::Agent);
    CHECK(vm.isChecked(CommandId::SwitchToAgentMode));
    CHECK(!vm.isChecked(CommandId::SwitchToCodeMode));
}

TEST(startup_opens_sample_documents)
{
    viewmodel::NotepadViewModel vm;
    vm.openStartupDocuments({});
    CHECK_EQ(vm.documents().size(), size_t(2));
    CHECK_EQ(vm.activeDocument()->path, std::string("src/main.cpp"));
    CHECK(vm.activeDocument()->language == model::Language::Cpp);
    vm.openFile("src/main.cpp"); // already open: activates instead of duplicating
    CHECK_EQ(vm.documents().size(), size_t(2));
}

TEST(dummy_findings_point_at_their_markers)
{
    for (model::ReviewTool tool : model::kAllReviewTools)
        for (const model::ReviewFinding& f : viewmodel::dummy::findings(tool)) {
            const model::TextBuffer buffer(viewmodel::dummy::fileContent(f.file));
            CHECK(f.line > 0 || f.file == "config.json");
            CHECK(f.line < buffer.lineCount());
        }
}

TEST(review_tools_filter_and_navigate)
{
    viewmodel::NotepadViewModel vm;
    viewmodel::ReviewViewModel& review = vm.review();
    const size_t all = review.visibleFindings().size();
    CHECK(all > 0);
    review.setToolVisible(model::ReviewTool::Security, false);
    CHECK(review.visibleFindings().size() < all);
    CHECK(review.count(model::Severity::Error) > 0);

    review.nextFinding(+1); // errors first
    const model::ReviewFinding& f = review.findings()[size_t(review.selectedFinding.get())];
    CHECK(f.severity == model::Severity::Error);
    CHECK_EQ(vm.activeDocument()->path, f.file);
    CHECK_EQ(vm.activeDocument()->caret.line, f.line);

    review.clearFindings();
    CHECK(review.visibleFindings().empty());
    vm.execute(CommandId::RunLint);
    CHECK(review.hasRun(model::ReviewTool::Lint));
    CHECK(review.count(model::ReviewTool::Lint) > 0);
}

TEST(accepting_an_agent_change_updates_the_document)
{
    viewmodel::NotepadViewModel vm;
    vm.openStartupDocuments({});
    const int pending = vm.agent().pendingChangeCount();
    CHECK(pending > 0);
    vm.agent().acceptChange(0);
    CHECK_EQ(vm.agent().pendingChangeCount(), pending - 1);
    const model::FileChange& change = vm.agent().changes()[0];
    CHECK_EQ(vm.activeDocument()->path, change.path);
    CHECK_EQ(vm.activeDocument()->buffer.text(), model::TextBuffer(change.after).text());
    CHECK(vm.activeDocument()->modified);
    vm.execute(CommandId::Undo); // accepted changes are undoable
    CHECK_EQ(vm.activeDocument()->buffer.text(), model::TextBuffer(change.before).text());
}

TEST(agent_replies_after_simulated_delay)
{
    viewmodel::AgentViewModel agent;
    const size_t before = agent.messages().size();
    agent.tick(10.0);
    agent.submitPrompt("Explain this", "src/main.cpp:1");
    CHECK(agent.busy.get());
    CHECK_EQ(agent.messages().size(), before + 1);
    agent.tick(10.5);
    CHECK(agent.busy.get());
    agent.tick(12.0);
    CHECK(!agent.busy.get());
    CHECK_EQ(agent.messages().size(), before + 2);
    CHECK(agent.messages().back().role == model::ChatRole::Agent);
}

// ---- Outline and folding ------------------------------------------------------------------------

TEST(outline_of_cpp_sample)
{
    const model::TextBuffer buffer(viewmodel::dummy::fileContent("src/main.cpp"));
    const auto nodes = model::buildOutline(buffer, model::Language::Cpp);
    auto find = [&](const char* title) -> const model::OutlineNode* {
        for (const auto& n : nodes)
            if (n.title.find(title) != std::string::npos)
                return &n;
        return nullptr;
    };
    const model::OutlineNode* ns = find("namespace");
    const model::OutlineNode* parse = find("Options parseArguments");
    const model::OutlineNode* options = find("struct Options");
    const model::OutlineNode* main = find("int main");
    CHECK(ns && parse && options && main);
    if (!ns || !parse || !options || !main)
        return;
    CHECK(ns->kind == model::OutlineKind::Namespace);
    CHECK(options->kind == model::OutlineKind::Type);
    CHECK(parse->kind == model::OutlineKind::Function);
    CHECK_EQ(parse->depth, 1); // inside the anonymous namespace
    CHECK_EQ(main->depth, 0);
    CHECK_EQ(buffer.line(main->endLine), std::string("}"));
    for (const auto& n : nodes) // control blocks are not functions
        if (n.title.rfind("for (", 0) == 0 || n.title.rfind("if (", 0) == 0)
            CHECK(n.kind == model::OutlineKind::Block);
}

TEST(outline_of_python_and_markdown)
{
    const auto py = model::buildOutline(model::TextBuffer(viewmodel::dummy::fileContent("scripts/build_index.py")),
                                        model::Language::Python);
    CHECK_EQ(py.size(), size_t(4)); // three functions and the __main__ guard
    CHECK(py[0].kind == model::OutlineKind::Function);
    const auto md = model::buildOutline(model::TextBuffer("# A\ntext\n## B\nmore\n## C\nend\n# D\nx"), model::Language::Markdown);
    CHECK_EQ(md.size(), size_t(4));
    CHECK_EQ(md[0].endLine, 5); // "# A" runs until "# D"
    CHECK_EQ(md[1].depth, 1);
    CHECK_EQ(md[1].endLine, 3);
}

TEST(detail_levels_fold_into_summaries)
{
    viewmodel::NotepadViewModel vm;
    vm.openFile("src/main.cpp");
    const model::Document* doc = vm.activeDocument();
    CHECK(doc->folded.empty());
    vm.detailLevel.set(2); // Overview: every function body
    int folded = 0;
    for (const auto& n : vm.outline()) {
        if (n.kind == model::OutlineKind::Function)
            CHECK(vm.isFolded(n));
        folded += vm.isFolded(n) ? 1 : 0;
        CHECK(!vm.summary(n).empty());
    }
    CHECK(folded >= 3);
    // Navigating into a folded region reveals it.
    const model::OutlineNode* main = nullptr;
    for (const auto& n : vm.outline())
        if (n.title.find("int main") != std::string::npos)
            main = &n;
    CHECK(main && vm.isFolded(*main));
    vm.goToLine(main->startLine + 3);
    CHECK(!vm.isFolded(*main));
    vm.detailLevel.set(0);
    CHECK(vm.activeDocument()->folded.empty());
    vm.execute(CommandId::FoldAll);
    CHECK_EQ(vm.activeDocument()->folded.size(), vm.outline().size());
    vm.execute(CommandId::UnfoldAll);
    CHECK(vm.activeDocument()->folded.empty());
}

TEST(custom_actions_run_in_the_terminal)
{
    tests::FakeTerminalService service;
    viewmodel::NotepadViewModel vm(&service);
    vm.setPinned(CommandId::FoldAll, true);
    CHECK(vm.isPinned(CommandId::FoldAll));
    vm.setPinned(CommandId::FoldAll, false);
    CHECK(!vm.isPinned(CommandId::FoldAll));
    vm.addCustomAction("Build", "make all");
    const int index = int(vm.customActions().size()) - 1;
    vm.runCustomAction(index);
    service.pump();
    CHECK(vm.panelTab.get() == viewmodel::PanelTab::Terminal);
    CHECK(service.sessions.back()->written.find("make all\r") != std::string::npos);
    vm.removeCustomAction(index);
    CHECK_EQ(int(vm.customActions().size()), index);
}

// ---- Layers -------------------------------------------------------------------------------------

TEST(layers_file_scope_zoom)
{
    viewmodel::NotepadViewModel vm;
    vm.openFile("src/main.cpp");
    viewmodel::LayersViewModel& layers = vm.layers();
    const auto& nodes = layers.nodes();
    CHECK(!nodes.empty());
    CHECK(nodes[0].kind == viewmodel::LayerKind::File);
    CHECK_EQ(layers.focus(), 0);
    CHECK_EQ(layers.breadcrumb().size(), size_t(1));
    // Zoom into the namespace, then out again.
    int ns = -1;
    for (int child : nodes[0].children)
        if (nodes[size_t(child)].kind == viewmodel::LayerKind::Namespace)
            ns = child;
    CHECK(ns >= 0);
    const int serial = layers.zoomSerial();
    layers.zoomIn(ns);
    CHECK_EQ(layers.focus(), ns);
    CHECK_EQ(layers.zoomDirection(), 1);
    CHECK(layers.zoomSerial() != serial);
    CHECK_EQ(layers.breadcrumb().size(), size_t(2));
    layers.zoomOut();
    CHECK_EQ(layers.focus(), 0);
    CHECK_EQ(layers.zoomDirection(), -1);
    // Opening a region expands it and moves the caret there.
    vm.detailLevel.set(2);
    int fn = -1;
    for (const auto& n : layers.nodes())
        if (n.signature.find("int main") != std::string::npos)
            fn = int(&n - layers.nodes().data());
    CHECK(fn >= 0);
    layers.open(fn);
    CHECK_EQ(vm.activeDocument()->caret.line, layers.nodes()[size_t(fn)].startLine);
    CHECK(vm.activeDocument()->folded.count(layers.nodes()[size_t(fn)].startLine) == 0);
    CHECK_EQ(layers.current(), fn); // the caret is inside it now
}

TEST(layers_project_scope_loads_files_lazily)
{
    viewmodel::NotepadViewModel vm;
    vm.openFile("src/main.cpp");
    viewmodel::LayersViewModel& layers = vm.layers();
    layers.scope.set(viewmodel::LayerScope::Project);
    const auto& nodes = layers.nodes();
    CHECK(nodes[0].kind == viewmodel::LayerKind::Project);
    int src = -1;
    for (int child : nodes[0].children)
        if (nodes[size_t(child)].title == "src")
            src = child;
    CHECK(src >= 0);
    CHECK_EQ(layers.current(), src); // the active file lives in src/
    layers.zoomIn(src);
    int file = -1;
    for (int child : layers.nodes()[size_t(src)].children)
        if (layers.nodes()[size_t(child)].title == "main.cpp")
            file = child;
    CHECK(file >= 0 && layers.nodes()[size_t(file)].children.empty() && layers.nodes()[size_t(file)].expandable);
    layers.zoomIn(file); // loads its parts
    const auto& after = layers.nodes();
    CHECK(!after[size_t(layers.focus())].children.empty());
    CHECK_EQ(after[size_t(layers.focus())].title, std::string("main.cpp"));
    // Scopes keep their own focus.
    layers.scope.set(viewmodel::LayerScope::File);
    CHECK_EQ(layers.focus(), 0);
}

TEST(unsaved_changes_are_asked_about)
{
    using Choice = viewmodel::NotepadViewModel::CloseChoice;
    viewmodel::NotepadViewModel vm;
    while (!vm.documents().empty())
        vm.closeDocument(0);
    int quits = 0;
    auto sub = vm.quitRequested.subscribe([&]() { ++quits; });

    // Nothing unsaved: close and quit at once.
    vm.openFile("a/clean.txt");
    vm.requestCloseDocument(0);
    CHECK(vm.documents().empty() && vm.pendingClose.get() == -1);
    vm.requestQuit();
    CHECK_EQ(quits, 1);

    // A modified document: the close waits for an answer.
    vm.openFile("a/one.txt");
    vm.insertText("edit");
    vm.execute(CommandId::CloseFile);
    CHECK_EQ(vm.pendingClose.get(), vm.activeDocument()->id);
    vm.resolvePendingClose(Choice::Cancel);
    CHECK_EQ(vm.documents().size(), size_t(1));
    vm.execute(CommandId::CloseFile);
    vm.resolvePendingClose(Choice::Save); // sample workspace: saved in memory
    CHECK(vm.documents().empty());

    vm.openFile("a/two.txt");
    vm.insertText("edit");
    vm.requestCloseDocument(0);
    vm.resolvePendingClose(Choice::Discard);
    CHECK(vm.documents().empty());

    // Untitled documents cannot be saved in place: Save keeps them open.
    vm.newDocument();
    vm.insertText("draft");
    vm.requestCloseDocument(0);
    vm.resolvePendingClose(Choice::Save);
    CHECK_EQ(vm.documents().size(), size_t(1));

    // Quitting with unsaved documents asks; Cancel stays, Discard quits.
    vm.execute(CommandId::Exit);
    CHECK(vm.quitConfirmPending.get() && quits == 1);
    CHECK_EQ(vm.unsavedDocuments().size(), size_t(1));
    vm.resolveQuit(Choice::Cancel);
    CHECK(!vm.quitConfirmPending.get() && quits == 1);
    vm.requestQuit();
    vm.resolveQuit(Choice::Save); // the untitled one cannot be saved: no quit
    CHECK_EQ(quits, 1);
    vm.requestQuit();
    vm.resolveQuit(Choice::Discard);
    CHECK_EQ(quits, 2);
}

// Test workspaces live in the system temp folder, never in the checkout.
std::string scratchDirectory()
{
    for (const char* name : {"TMPDIR", "TEMP", "TMP"})
        if (const char* dir = std::getenv(name); dir && *dir)
            return os::fs::path::normalize(dir);
    return "/tmp";
}

TEST(settings_store_round_trips_and_preserves)
{
    services::FileService files;
    const std::string dir = scratchDirectory() + "/enp-test-settings";
    const std::string path = dir + "/store.json";
    os::fs::createDirectories(dir);
    // An unknown section (from another version) must survive.
    files.write(path, "{\"future\":{\"x\":1},\"view\":{\"darkTheme\":false,\"zoom\":1.5}}", nullptr);
    {
        viewmodel::NotepadViewModel vm(nullptr, &files);
        vm.setSettingsPath(path);
        CHECK(!vm.darkTheme.get());
        CHECK(vm.zoom.get() > 1.49f && vm.zoom.get() < 1.51f);
        vm.wordWrap.set(true);
        vm.workspace.set(viewmodel::WorkspaceMode::Agent);
        vm.setPinned(CommandId::FindInFiles, true);
        vm.addCustomAction("Build", "cmake --build build");
        CHECK(vm.settings().dirty());
        // Debounced: not written at once, written after a quiet moment.
        vm.tick(100.0);
        CHECK(files.read(path)->find("wordWrap") == std::string::npos);
        vm.tick(101.0);
        CHECK(!vm.settings().dirty());
        CHECK(files.read(path)->find("wordWrap") != std::string::npos);
    }
    viewmodel::NotepadViewModel vm(nullptr, &files);
    vm.setSettingsPath(path);
    CHECK(vm.wordWrap.get() && vm.workspace.get() == viewmodel::WorkspaceMode::Agent);
    CHECK(vm.isPinned(CommandId::FindInFiles));
    CHECK(!vm.customActions().empty() && vm.customActions().back().command == "cmake --build build");
    CHECK(files.read(path)->find("\"future\"") != std::string::npos);

    // A corrupt file is kept aside and the defaults are used.
    files.write(path, "{not json", nullptr);
    viewmodel::NotepadViewModel fresh(nullptr, &files);
    fresh.setSettingsPath(path);
    CHECK(fresh.darkTheme.get());
    CHECK_EQ(*files.read(path + ".bad"), std::string("{not json"));
    std::remove(path.c_str());
    std::remove((path + ".bad").c_str());
}

TEST(recent_files_and_session_restore)
{
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-session";
    const std::string path = scratchDirectory() + "/enp-test-settings/session.json";
    os::fs::createDirectories(root + "/src");
    for (int i = 0; i < 20; ++i)
        files.write(root + "/src/f" + std::to_string(i) + ".txt", "line 0\nline 1\nline 2\nline 3\n", nullptr);
    std::remove(path.c_str());
    {
        viewmodel::NotepadViewModel vm(nullptr, &files);
        vm.setSettingsPath(path);
        CHECK(vm.openFolder(root));
        for (int i = 0; i < 20; ++i)
            vm.openFile("src/f" + std::to_string(i) + ".txt");
        vm.openFile("src/f3.txt"); // again: moves to the front, no duplicate
        CHECK_EQ(vm.recentFiles().size(), viewmodel::NotepadViewModel::kMaxRecentFiles);
        CHECK(vm.recentFiles().front().size() > 10 &&
              vm.recentFiles().front().substr(vm.recentFiles().front().size() - 10) == "src/f3.txt");
        CHECK(!vm.recentFolders().empty());
        // Session: keep two documents, the second active, its caret on line 2.
        while (vm.documents().size() > 2)
            vm.closeDocument(0);
        vm.activate(1);
        vm.setCaret({2, 0}, false);
    }
    // A new start without arguments carries on.
    viewmodel::NotepadViewModel vm(nullptr, &files);
    vm.setSettingsPath(path);
    CHECK(vm.restoreSession());
    CHECK(vm.workspaceRoot().size() > 16 && vm.workspaceRoot().substr(vm.workspaceRoot().size() - 16) == "enp-test-session");
    CHECK_EQ(vm.documents().size(), size_t(2));
    CHECK_EQ(vm.activeIndex.get(), 1);
    CHECK_EQ(vm.activeDocument()->caret.line, 2);
    CHECK_EQ(vm.recentFiles().size(), viewmodel::NotepadViewModel::kMaxRecentFiles);
    // A recent file deleted since is dropped when chosen.
    const std::string gone = vm.recentFiles().back();
    std::remove(gone.c_str());
    vm.openRecent(gone);
    CHECK(std::find(vm.recentFiles().begin(), vm.recentFiles().end(), gone) == vm.recentFiles().end());
    vm.clearRecent();
    CHECK(vm.recentFiles().empty() && vm.recentFolders().empty());
    // No session: nothing to restore.
    viewmodel::NotepadViewModel blank(nullptr, &files);
    CHECK(!blank.restoreSession());
    std::remove(path.c_str());
}

// The state at the start of every line by highlighting from the top (what the cache replaces).
std::vector<int> fullScan(const model::TextBuffer& b, const model::LanguageSpec& spec)
{
    std::vector<int> states;
    int state = 0;
    std::vector<model::Token> toks;
    for (int l = 0; l < b.lineCount(); ++l) {
        states.push_back(state);
        model::highlightLine(b.line(l), spec, state, toks);
    }
    return states;
}

TEST(files_changed_on_disk)
{
    using Disk = model::Document::DiskState;
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-disk";
    os::fs::createDirectories(root);
    files.write(root + "/a.txt", "one\ntwo\nthree\n", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    vm.openFile("a.txt");
    model::Document& doc = *vm.activeDocument();
    vm.setCaret({2, 0}, false);

    // Unmodified: reloads by itself, caret line kept.
    files.write(root + "/a.txt", "one\ntwo\nthree\nfour\n", nullptr);
    vm.checkDiskChanges();
    CHECK(doc.buffer.text() == "one\ntwo\nthree\nfour\n" && !doc.modified && doc.caret.line == 2);
    CHECK(doc.diskState == Disk::InSync);

    // Modified: marked; saving does not overwrite the other program's change.
    vm.insertText("mine ");
    files.write(root + "/a.txt", "theirs\n", nullptr);
    vm.checkDiskChanges();
    CHECK(doc.diskState == Disk::Changed && doc.buffer.text().find("mine") != std::string::npos);
    vm.execute(CommandId::Save);
    CHECK_EQ(*files.read(root + "/a.txt"), std::string("theirs\n"));
    // Compare shows both sides; Keep mine lets the next save through.
    vm.compareWithDisk(doc);
    CHECK(vm.panelTab.get() == viewmodel::PanelTab::Diff && !vm.review().diff().empty());
    vm.keepMine(doc);
    vm.execute(CommandId::Save);
    CHECK(files.read(root + "/a.txt")->find("mine") != std::string::npos && !doc.modified);

    // Reload drops the edits for the file's text.
    vm.insertText("again ");
    files.write(root + "/a.txt", "fresh text\n", nullptr);
    vm.checkDiskChanges();
    CHECK(doc.diskState == Disk::Changed);
    vm.reloadFromDisk(doc);
    CHECK(doc.buffer.text() == "fresh text\n" && !doc.modified && doc.diskState == Disk::InSync);

    // Deleted: marked; saving recreates it.
    std::remove((root + "/a.txt").c_str());
    vm.checkDiskChanges();
    CHECK(doc.diskState == Disk::Deleted);
    vm.keepMine(doc);
    vm.insertText("x");
    vm.execute(CommandId::Save);
    CHECK(files.read(root + "/a.txt").has_value());
}

TEST(one_language_picker_for_every_language)
{
    viewmodel::NotepadViewModel vm;
    vm.newDocument();
    int requested = 0;
    auto sub = vm.languagePickerRequested.subscribe([&]() { ++requested; });
    vm.execute(CommandId::ChooseLanguage);
    CHECK_EQ(requested, 1);
    // The connector sets any language of the table by key or name, and lists the keys otherwise.
    for (const model::LanguageSpec& spec : model::languages()) {
        const model::Json r = app::handleAutomation(vm, "set_language", model::Json::object().set("language", spec.key));
        CHECK(!r.find("error"));
        CHECK(vm.activeDocument()->language == spec.id);
    }
    app::handleAutomation(vm, "set_language", model::Json::object().set("language", "CMake"));
    CHECK(vm.activeDocument()->language == model::Language::CMake);
    const model::Json bad = app::handleAutomation(vm, "set_language", model::Json::object().set("language", "klingon"));
    CHECK(bad.find("error") && bad.getString("error").find("csv") != std::string::npos);
}

TEST(undo_redo_round_trips_every_edit)
{
    viewmodel::NotepadViewModel vm;
    vm.newDocument();
    vm.setDocumentLanguage(model::Language::Cpp);
    model::Document& doc = *vm.activeDocument();
    vm.insertText("int main()\n{\n    return 0;   \n}\n// b\n// a\n");
    // Each action: the text before, the action, then undo must give the text before and redo the text after.
    struct Action {
        const char* name;
        std::function<void()> run;
    };
    const Action actions[] = {
        {"typing", [&] { vm.setCaret({2, 4}, false); for (char c : std::string("x = 1; ")) vm.insertText(std::string(1, c)); }},
        {"smart newline", [&] { vm.setCaret({1, 1}, false); vm.newline(); }},
        {"backspace", [&] { vm.setCaret({2, 6}, false); vm.backspace(); vm.backspace(); }},
        {"delete", [&] { vm.setCaret({0, 0}, false); vm.deleteForward(); }},
        {"duplicate line", [&] { vm.setCaret({0, 0}, false); vm.execute(CommandId::DuplicateLine); }},
        {"delete line", [&] { vm.setCaret({1, 0}, false); vm.execute(CommandId::DeleteLine); }},
        {"move line down", [&] { vm.setCaret({0, 0}, false); vm.execute(CommandId::MoveLineDown); }},
        {"move line up", [&] { vm.setCaret({3, 0}, false); vm.execute(CommandId::MoveLineUp); }},
        {"sort", [&] { vm.setSelection({4, 0}, {6, 0}); vm.execute(CommandId::SortLines); }},
        {"upper case", [&] { vm.setSelection({0, 0}, {0, 3}); vm.execute(CommandId::UpperCase); }},
        {"toggle comment", [&] { vm.setCaret({1, 0}, false); vm.execute(CommandId::ToggleComment); }},
        {"indent", [&] { vm.setSelection({0, 0}, {3, 0}); vm.execute(CommandId::Indent); }},
        {"outdent", [&] { vm.setSelection({0, 0}, {3, 0}); vm.execute(CommandId::Outdent); }},
        {"trim trailing", [&] { vm.execute(CommandId::TrimTrailing); }},
        {"convert indentation", [&] { vm.execute(CommandId::IndentWithTabs); vm.execute(CommandId::ConvertIndentation); }},
        {"replace selection", [&] { vm.selectAll(); vm.insertText("all\nnew\ntext"); }},
    };
    std::vector<std::string> states{doc.buffer.text()};
    for (const Action& a : actions) {
        const std::string before = doc.buffer.text();
        a.run();
        const std::string after = doc.buffer.text();
        if (after == before)
            continue;
        vm.execute(CommandId::Undo);
        const bool undone = doc.buffer.text() == before;
        vm.execute(CommandId::Redo);
        const bool redone = doc.buffer.text() == after;
        if (!undone || !redone)
            std::printf("  %s: undo %s, redo %s\n", a.name, undone ? "ok" : "WRONG", redone ? "ok" : "WRONG");
        CHECK(undone && redone);
        states.push_back(after);
    }
    // All the way back, and all the way forward again.
    for (size_t i = states.size() - 1; i > 0; --i) {
        vm.execute(CommandId::Undo);
        if (doc.buffer.text() != states[i - 1])
            std::printf("  back to state %d: got [%s]\n  expected [%s]\n", int(i - 1), doc.buffer.text().c_str(), states[i - 1].c_str());
        CHECK(doc.buffer.text() == states[i - 1]);
    }
    while (vm.command(CommandId::Redo).canExecute())
        vm.execute(CommandId::Redo);
    CHECK(doc.buffer.text() == states.back());

    // A 1 MB document: a hundred small edits keep a small history (no copies of the document).
    viewmodel::NotepadViewModel big;
    big.newDocument();
    std::string text;
    while (text.size() < 1000000)
        text += "some line of text that is long enough to add up quickly\n";
    big.applyText(big.activeDocument()->title, text); // one step holding the text once
    const size_t base = big.historyBytes();
    for (int i = 0; i < 100; ++i) {
        big.setCaret({i * 50, 0}, false);
        big.insertText("edit ");
    }
    CHECK(big.historyBytes() - base < 2000);
    CHECK(!big.activeDocument()->buffer.equals(text));
    for (int i = 0; i < 100; ++i)
        big.execute(CommandId::Undo);
    CHECK(big.activeDocument()->buffer.equals(text));
}

TEST(highlight_cache_matches_full_scan_after_edits)
{
    const model::LanguageSpec& cpp = model::languageSpec(model::Language::Cpp);
    model::TextBuffer b("int a;\n/* open\nstill comment\n*/ int b;\nint c;\nint d;\n");
    model::HighlightCache cache;
    auto agrees = [&](const model::LanguageSpec& spec) {
        const std::vector<int> expected = fullScan(b, spec);
        for (int l = b.lineCount() - 1; l >= 0; --l) // asked bottom-up and top-down
            if (cache.stateAt(b, spec, l) != expected[size_t(l)])
                return false;
        return true;
    };
    CHECK(agrees(cpp));
    b.erase({3, 0}, {3, 2}); // the comment no longer closes: everything below changes
    CHECK(agrees(cpp));
    b.insert({5, 0}, "*/");  // closes again further down
    CHECK(agrees(cpp));
    b.insert({0, 0}, "/*");  // an edit at the top
    CHECK(agrees(cpp));
    b.eraseLine(1);
    b.insertLine(2, "x");
    CHECK(agrees(cpp));
    b.setText("\"\"\"doc\nstill\n\"\"\"\nx = 1\n");
    CHECK(agrees(model::languageSpec(model::Language::Python))); // language change: rebuilt

    // An edit near the end of a long file keeps every state above it.
    std::string big;
    for (int i = 0; i < 20000; ++i)
        big += "int value" + std::to_string(i) + " = " + std::to_string(i) + "; // line\n";
    model::TextBuffer large(big);
    model::HighlightCache lc;
    lc.stateAt(large, cpp, 19999);
    CHECK_EQ(lc.cachedLines(), size_t(20000));
    large.insert({19900, 0}, "x");
    lc.stateAt(large, cpp, 0); // revalidates without extending
    CHECK_EQ(lc.cachedLines(), size_t(19901));
    CHECK_EQ(large.firstLineChangedSince(large.version()), large.lineCount());
}

TEST(search_text_options_and_replace)
{
    model::SearchOptions o;
    auto m = model::searchText("Foo foo food\nFOO", "foo", o);
    CHECK_EQ(m.size(), size_t(4)); // case-insensitive by default
    o.matchCase = true;
    CHECK_EQ(model::searchText("Foo foo food\nFOO", "foo", o).size(), size_t(2));
    o.wholeWord = true;
    m = model::searchText("Foo foo food\nFOO", "foo", o);
    CHECK(m.size() == 1 && m[0].column == 4);
    o = {};
    o.regex = true;
    m = model::searchText("a1 b22\r\nc333", "[a-z](\\d+)", o);
    CHECK(m.size() == 3 && m[2].line == 1 && m[2].length == 4);
    std::string error;
    CHECK(model::searchText("x", "(", o, &error).empty() && !error.empty());
    // Replace keeps line endings; regex groups; plain '$' stays literal.
    int count = 0;
    CHECK_EQ(model::replaceText("a1 b22\r\nc333", "([a-z])(\\d+)", "$2$1", o, &count), std::string("1a 22b\r\n333c"));
    CHECK_EQ(count, 3);
    model::SearchOptions plain;
    CHECK_EQ(model::replaceText("cost x", "x", "$5", plain), std::string("cost $5"));
    // File filters.
    CHECK(model::matchesFilter("Main.CPP", "*.cpp;*.h"));
    CHECK(model::matchesFilter("a.h", "*.cpp, *.h"));
    CHECK(!model::matchesFilter("a.txt", "*.cpp;*.h"));
    CHECK(model::matchesFilter("anything", ""));
    CHECK(model::matchesFilter("test_a.py", "test_?.py"));
}

TEST(find_in_files_searches_workspace_and_replaces)
{
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-search";
    os::fs::createDirectories(root + "/src");
    os::fs::createDirectories(root + "/build");
    files.write(root + "/src/a.cpp", "int widget = 1;\nint other = widget;\n", nullptr);
    files.write(root + "/src/b.h", "// no match here\n", nullptr);
    files.write(root + "/notes.md", "The widget is blue.\n", nullptr);
    files.write(root + "/build/gen.cpp", "widget widget widget\n", nullptr); // build output: skipped
    files.write(root + "/bin.dat", std::string("widget\0\0\0", 9), nullptr);  // binary: skipped

    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    viewmodel::FindInFilesViewModel& find = vm.findInFiles();
    // An open document is searched in its current (unsaved) text.
    vm.openFile("notes.md");
    vm.setCaret({0, 0}, false);
    vm.insertText("widget ");
    find.query = "widget";
    find.searchNow();
    CHECK(find.error.get().empty());
    CHECK_EQ(find.results().size(), size_t(2));
    int total = 0;
    for (const auto& r : find.results())
        total += int(r.hits.size());
    CHECK_EQ(total, 4); // a.cpp: 2, notes.md: 2 (one unsaved)
    CHECK_EQ(find.hitCount(), 4);

    // The asynchronous search gives the same answer.
    find.search();
    for (int i = 0; i < 200 && find.running.get(); ++i) {
        os::dispatch::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(!find.running.get() && find.hitCount() == 4);

    // Include filter and whole word.
    find.include = "*.cpp";
    find.searchNow();
    CHECK(find.results().size() == 1 && find.results()[0].path == "src/a.cpp");
    find.include.clear();

    // Opening a result selects the match.
    find.searchNow();
    const auto& first = find.results()[0];
    find.open(first.path, first.hits[0]);
    CHECK(vm.activeDocument()->hasSelection() && vm.selectedText() == "widget");

    // Replace All: closed files are written, open documents edited (undoable).
    find.replacement = "gadget";
    CHECK_EQ(find.replaceAll(), 2);
    CHECK_EQ(*files.read(root + "/src/a.cpp"), std::string("int gadget = 1;\nint other = gadget;\n"));
    vm.openFile("notes.md");
    CHECK(vm.activeDocument()->buffer.text().find("widget") == std::string::npos);
    CHECK_EQ(*files.read(root + "/notes.md"), std::string("The widget is blue.\n")); // not saved behind the editor
    CHECK_EQ(find.hitCount(), 0);
}

TEST(file_dialog_open_and_save_as)
{
    using viewmodel::FileDialogMode;
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-dialog";
    os::fs::createDirectories(root + "/src/deep");
    os::fs::createDirectories(root + "/zeta");
    files.write(root + "/src/a.cpp", "int a;\n", nullptr);
    files.write(root + "/notes.md", "# Notes\n", nullptr);
    std::remove((root + "/saved.py").c_str());
    std::remove((root + "/src/renamed.cpp").c_str());

    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    viewmodel::FileDialogViewModel& dialog = vm.fileDialog();

    // Open: folders first, a typed name filters, folders navigate.
    vm.execute(CommandId::OpenFile);
    CHECK(dialog.visible.get() && dialog.mode() == FileDialogMode::Open);
    auto entries = dialog.visibleEntries();
    CHECK(entries.size() >= 3 && entries[0].isDirectory && entries[0].name == "src" && entries[1].name == "zeta");
    dialog.fileName = "note";
    CHECK(dialog.visibleEntries().size() == 1 && dialog.visibleEntries()[0].name == "notes.md");
    dialog.fileName.clear();
    dialog.choose({"src", true}, false);
    CHECK(dialog.folder().size() > 4 && dialog.folder().substr(dialog.folder().size() - 4) == "/src");
    dialog.up();
    dialog.back();
    CHECK(dialog.folder().substr(dialog.folder().size() - 4) == "/src");
    dialog.choose({"a.cpp", false}, true); // double click
    CHECK(!dialog.visible.get());
    CHECK(vm.activeDocument() && vm.activeDocument()->path == "src/a.cpp"); // workspace-relative
    // A path typed into the box works too; a missing file is an error.
    vm.execute(CommandId::OpenFile);
    dialog.fileName = "nope.txt";
    CHECK(!dialog.confirm() && !dialog.error.get().empty());
    dialog.cancel();

    // Save As: renames the tab, changes the language, asks before overwriting.
    vm.execute(CommandId::SaveAs);
    CHECK(dialog.mode() == FileDialogMode::SaveAs && dialog.fileName == "a.cpp");
    dialog.fileName = "renamed.cpp";
    CHECK(dialog.confirm());
    CHECK_EQ(vm.activeDocument()->path, std::string("src/renamed.cpp"));
    CHECK(files.read(root + "/src/renamed.cpp").has_value());
    vm.execute(CommandId::SaveAs);
    dialog.goTo(root);
    dialog.fileName = "notes.md";
    CHECK(!dialog.confirm() && dialog.confirmOverwrite); // exists: asks first
    dialog.cancel();
    CHECK_EQ(*files.read(root + "/notes.md"), std::string("# Notes\n"));

    // Untitled: Save goes to Save As; the language follows the new name.
    vm.newDocument();
    vm.insertText("print(1)");
    vm.execute(CommandId::Save);
    CHECK(dialog.visible.get() && dialog.mode() == FileDialogMode::SaveAs);
    dialog.goTo(root);
    dialog.fileName = "saved.py";
    CHECK(dialog.confirm());
    CHECK(vm.activeDocument()->language == model::Language::Python && !vm.activeDocument()->modified);
    CHECK_EQ(*files.read(root + "/saved.py"), std::string("print(1)"));

    // "Save" in the close prompt of an untitled document: Save As, then the document closes.
    vm.newDocument();
    vm.insertText("draft");
    const size_t before = vm.documents().size();
    vm.requestCloseDocument(vm.activeIndex.get());
    vm.resolvePendingClose(viewmodel::NotepadViewModel::CloseChoice::Save);
    CHECK(dialog.visible.get());
    dialog.goTo(root);
    dialog.fileName = "draft.txt";
    CHECK(dialog.confirm());
    CHECK_EQ(vm.documents().size(), before - 1);
    std::remove((root + "/draft.txt").c_str());
    std::remove((root + "/saved.py").c_str());
    std::remove((root + "/src/renamed.cpp").c_str());

    // Open Folder.
    vm.execute(CommandId::OpenFolder);
    CHECK(dialog.mode() == FileDialogMode::OpenFolder);
    dialog.goTo(root + "/src");
    CHECK(dialog.confirm());
    CHECK(vm.workspaceRoot().size() > 4 && vm.workspaceRoot().substr(vm.workspaceRoot().size() - 4) == "/src");
}

TEST(many_documents_close_reorder_and_disambiguate)
{
    viewmodel::NotepadViewModel vm;
    while (!vm.documents().empty())
        vm.closeDocument(0);
    for (const char* path : {"a/x/Main.h", "b/x/Main.h", "c/Other.h", "d/Main.h", "e/Last.h"})
        vm.openFile(path);
    CHECK_EQ(vm.documents().size(), size_t(5));
    // Namesakes get the shortest folder suffix that tells them apart.
    CHECK_EQ(vm.documentHint(0), std::string("a/x"));
    CHECK_EQ(vm.documentHint(1), std::string("b/x"));
    CHECK_EQ(vm.documentHint(2), std::string(""));
    CHECK_EQ(vm.documentHint(3), std::string("d"));
    CHECK_EQ(vm.documentIndex("c/Other.h"), 2);

    // Cycling wraps around.
    CHECK_EQ(vm.activeIndex.get(), 4);
    vm.execute(CommandId::NextDocument);
    CHECK_EQ(vm.activeIndex.get(), 0);
    vm.execute(CommandId::PreviousDocument);
    CHECK_EQ(vm.activeIndex.get(), 4);

    // Reordering keeps the active document active.
    vm.activate(1);
    vm.moveDocument(1, 3);
    CHECK_EQ(vm.documents()[3]->path, std::string("b/x/Main.h"));
    CHECK_EQ(vm.activeIndex.get(), 3);

    // Bulk closing keeps unsaved documents.
    vm.activate(0);
    vm.insertText("edit");
    vm.closeToTheRight(2); // closes indices 3, 4
    CHECK_EQ(vm.documents().size(), size_t(3));
    vm.activate(2);
    vm.closeOthers(2); // index 0 is modified: kept
    CHECK_EQ(vm.documents().size(), size_t(2));
    CHECK(vm.statusText.get().find("unsaved") != std::string::npos);
    CHECK(vm.activeDocument() && vm.activeDocument()->path == "d/Main.h");
    vm.closeAll();
    CHECK_EQ(vm.documents().size(), size_t(1));
    CHECK(vm.activeDocument()->modified);
}

TEST(open_real_folder_reads_and_saves_files)
{
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-workspace";
    os::fs::createDirectories(root + "/src");
    files.write(root + "/README.md", "# Test\r\nhello\r\n", nullptr);
    files.write(root + "/src/a.cpp", "\xEF\xBB\xBFint f()\n{\n    return 1;\n}\n", nullptr);
    os::fs::createDirectories(root + "/build");
    files.write(root + "/build/skip.txt", "x", nullptr);

    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    CHECK_EQ(vm.explorer().name, std::string("enp-test-workspace"));
    bool sawBuild = false, sawSrc = false;
    for (const auto& item : vm.explorer().children) {
        sawBuild = sawBuild || item.name == "build";
        sawSrc = sawSrc || item.name == "src";
    }
    CHECK(!sawBuild && sawSrc);
    CHECK_EQ(vm.documents().size(), size_t(1)); // README.md opens by default
    CHECK(vm.activeDocument()->eol == model::EolMode::CrLf);

    vm.openFile("src/a.cpp");
    model::Document* doc = vm.activeDocument();
    CHECK(doc->onDisk && doc->encoding == model::Encoding::Utf8Bom && doc->eol == model::EolMode::Lf);
    CHECK_EQ(doc->buffer.line(0), std::string("int f()"));
    vm.setCaret({2, 12}, false); // after "return 1"
    vm.insertText(" + 1");
    vm.execute(CommandId::Save);
    CHECK(!doc->modified);
    CHECK_EQ(*files.read(root + "/src/a.cpp"), std::string("\xEF\xBB\xBFint f()\n{\n    return 1 + 1;\n}\n"));
    CHECK(vm.readWorkspaceFile("README.md").find("hello") != std::string::npos);
}

// ---- Languages ----------------------------------------------------------------------------------

TEST(language_table_recognises_files)
{
    using L = model::Language;
    CHECK(model::languageFromPath("src/CMakeLists.txt") == L::CMake);
    CHECK(model::languageFromPath("cmake/Tool.cmake") == L::CMake);
    CHECK(model::languageFromPath("docs/Guide.MD") == L::Markdown);
    CHECK(model::languageFromPath(".github/workflows/build.yml") == L::Yaml);
    CHECK(model::languageFromPath("config.yaml") == L::Yaml);
    CHECK(model::languageFromPath("data/sales.csv") == L::Csv);
    CHECK(model::languageFromPath("data/sales.tsv") == L::Csv);
    CHECK(model::languageFromPath("package.json") == L::Json);
    CHECK(model::languageFromPath("App.tsx") == L::TypeScript);
    CHECK(model::languageFromPath("main.rs") == L::Rust);
    CHECK(model::languageFromPath("/home/u/.bashrc") == L::Shell);
    CHECK(model::languageFromPath("Cargo.toml") == L::Toml);
    CHECK(model::languageFromPath("index.html") == L::Html);
    CHECK(model::languageFromPath("app.vcxproj") == L::Xml);
    CHECK(model::languageFromPath("query.sql") == L::Sql);
    CHECK(model::languageFromPath("unknown.xyz") == L::PlainText);
    // Every language has exactly one entry, found by id and by key.
    CHECK_EQ(model::languages().size(), size_t(L::Count));
    for (const model::LanguageSpec& spec : model::languages()) {
        CHECK(model::languageSpec(spec.id).id == spec.id);
        CHECK(model::languageFromKey(spec.key) == spec.id);
        CHECK(!model::languageTokens(spec.id).empty());
    }
    CHECK(model::languages().front().id == L::PlainText);
}

std::vector<model::Token> tokensOf(const std::string& line, model::Language language, int& state)
{
    std::vector<model::Token> out;
    model::highlightLine(line, model::languageSpec(language), state, out);
    return out;
}

TEST(highlighter_code_languages)
{
    using T = model::Token;
    int st = 0;
    auto t = tokensOf("int x = 42; // note", model::Language::Cpp, st);
    CHECK(t[0] == T::Type && t[8] == T::Number && t[12] == T::Comment);
    t = tokensOf("/* open", model::Language::Cpp, st);
    CHECK(st != 0 && t[0] == T::Comment);
    t = tokensOf("still */ return", model::Language::Cpp, st);
    CHECK(st == 0 && t[0] == T::Comment && t[9] == T::Keyword);

    // CMake: commands, control keywords, variables, upper-case constants, comments.
    st = 0;
    t = tokensOf("if(WIN32) # windows", model::Language::CMake, st);
    CHECK(t[0] == T::Keyword && t[3] == T::Type && t[12] == T::Comment);
    t = tokensOf("target_link_libraries(core PRIVATE ${LIBS})", model::Language::CMake, st);
    CHECK(t[0] == T::Function && t[27] == T::Type && t[35] == T::Variable);

    // Python triple-quoted strings span lines.
    st = 0;
    tokensOf("x = \"\"\"doc", model::Language::Python, st);
    CHECK(st != 0);
    t = tokensOf("end\"\"\" + 1", model::Language::Python, st);
    CHECK(st == 0 && t[0] == T::String && t[9] == T::Number);

    // Keys: JSON, YAML, TOML.
    st = 0;
    t = tokensOf("  \"name\": \"x\",", model::Language::Json, st);
    CHECK(t[2] == T::Key && t[10] == T::String);
    t = tokensOf("  runs-on: ubuntu # os", model::Language::Yaml, st);
    CHECK(t[2] == T::Key && t[11] == T::Text && t[20] == T::Comment);
    t = tokensOf("  - run: make", model::Language::Yaml, st);
    CHECK(t[2] == T::Keyword && t[4] == T::Key);
    t = tokensOf("[package]", model::Language::Toml, st);
    CHECK(t[0] == T::Heading);
    t = tokensOf("name = \"demo\"", model::Language::Toml, st);
    CHECK(t[0] == T::Key && t[7] == T::String);

    // Shell variables; a '#' inside a word is not a comment.
    st = 0;
    t = tokensOf("echo \"$HOME\" ${#arr} # done", model::Language::Shell, st);
    CHECK(t[0] == T::Type && t[13] == T::Variable && t[21] == T::Comment);
}

TEST(highlighter_markdown_markup_and_csv)
{
    using T = model::Token;
    int st = 0;
    auto t = tokensOf("# Title", model::Language::Markdown, st);
    CHECK(t[2] == T::Heading);
    t = tokensOf("Some **bold** and `code` [docs](http://x)", model::Language::Markdown, st);
    CHECK(t[0] == T::Text && t[5] == T::Emphasis && t[19] == T::Code && t[26] == T::Link);
    t = tokensOf("```cpp", model::Language::Markdown, st);
    CHECK(st != 0);
    t = tokensOf("# not a heading", model::Language::Markdown, st);
    CHECK(t[0] == T::Code);
    tokensOf("```", model::Language::Markdown, st);
    CHECK(st == 0);
    t = tokensOf("snake_case_word", model::Language::Markdown, st);
    CHECK(t[6] == T::Text); // underscores inside words are not emphasis

    st = 0;
    t = tokensOf("<a href=\"x\">Fish &amp; chips</a>", model::Language::Html, st);
    CHECK(t[0] == T::Tag && t[1] == T::Tag && t[3] == T::Attribute && t[8] == T::String && t[11] == T::Tag &&
          t[12] == T::Text && t[17] == T::Variable && t[28] == T::Tag);
    t = tokensOf("<item id=\"1\"", model::Language::Xml, st); // the tag continues on the next line
    CHECK(st != 0);
    t = tokensOf("   enabled=\"true\"/>", model::Language::Xml, st);
    CHECK(st == 0 && t[3] == T::Attribute && t[17] == T::Tag);

    st = 0;
    t = tokensOf("1,\"Oslo, NO\",88", model::Language::Csv, st);
    CHECK(t[0] == T::Column1 && t[2] == T::Column2 && t[7] == T::Column2 && t[13] == T::Column3);
    t = tokensOf("a\tb\tc", model::Language::Csv, st);
    CHECK(t[0] == T::Column1 && t[2] == T::Column2 && t[4] == T::Column3);
}

// "start-end" ranges of an outline, for compact checks.
std::vector<std::string> ranges(const std::string& text, model::Language language)
{
    std::vector<std::string> out;
    for (const auto& n : model::buildOutline(model::TextBuffer(text), language))
        out.push_back(std::to_string(n.startLine) + "-" + std::to_string(n.endLine));
    return out;
}

bool hasRange(const std::vector<std::string>& r, const char* range)
{
    return std::find(r.begin(), r.end(), range) != r.end();
}

TEST(folding_for_every_style)
{
    using L = model::Language;
    // CMake: block commands (case-insensitive) and multi-line argument lists.
    auto r = ranges("function(f)\n  message(hi)\nendfunction()\nIF(WIN32)\n  set(A 1)\nENDIF()\nset(SRC\n  a.cpp\n  b.cpp)\n", L::CMake);
    CHECK(hasRange(r, "0-2") && hasRange(r, "3-5") && hasRange(r, "6-8"));
    CHECK(model::buildOutline(model::TextBuffer("function(f)\n x()\nendfunction()"), L::CMake)[0].kind == model::OutlineKind::Function);
    // Markdown: sections, fenced code; a "#" line inside a fence is not a heading.
    r = ranges("# A\ntext\n```sh\n# comment\nls\n```\n# B\nmore\n", L::Markdown);
    CHECK(hasRange(r, "0-5") && hasRange(r, "2-5") && hasRange(r, "6-7") && r.size() == 3);
    // YAML by indentation.
    r = ranges("jobs:\n  build:\n    runs-on: x\n    steps:\n      - a\n      - b\nname: y\n", L::Yaml);
    CHECK(hasRange(r, "0-5") && hasRange(r, "1-5") && hasRange(r, "3-5"));
    // HTML elements spanning lines; void elements do not open regions.
    r = ranges("<html>\n<body>\n<br>\n<p>x</p>\n</body>\n</html>\n", L::Html);
    CHECK(hasRange(r, "0-5") && hasRange(r, "1-4") && r.size() == 2);
    // TOML sections.
    r = ranges("[a]\nx = 1\n\n[b]\ny = 2\n", L::Toml);
    CHECK(hasRange(r, "0-1") && hasRange(r, "3-4"));
    // Lua and shell keywords; shell function braces.
    r = ranges("local function f()\n  if x then\n    y()\n  end\nend\n", L::Lua);
    CHECK(hasRange(r, "0-4") && hasRange(r, "1-3"));
    r = ranges("build() {\n  if [ -f a ]; then\n    echo ${x}\n  fi\n}\n", L::Shell);
    CHECK(hasRange(r, "0-4") && hasRange(r, "1-3"));
    // #region markers in a brace language; JSON brackets.
    r = ranges("#region Fields\nint a;\nint b;\n#endregion\nclass C {\n  void f() {\n  }\n}\n", L::CSharp);
    CHECK(hasRange(r, "0-3") && hasRange(r, "4-7") && hasRange(r, "5-6"));
    r = ranges("{\n  \"a\": [\n    1\n  ]\n}\n", L::Json);
    CHECK(hasRange(r, "0-4") && hasRange(r, "1-3"));
    // Languages without structure do not fold.
    CHECK(ranges("a,b\n1,2\n", L::Csv).empty());
}

TEST(syntax_colours_are_saved_and_reloaded)
{
    services::FileService files;
    const std::string path = scratchDirectory() + "/enp-test-settings/settings.json";
    std::remove(path.c_str());
    {
        viewmodel::NotepadViewModel vm(nullptr, &files);
        vm.setSettingsPath(path);
        vm.setSyntaxColor(true, "cmake", model::Token::Function, 0x112233);
        vm.setSyntaxColor(true, viewmodel::NotepadViewModel::kAllLanguages, model::Token::Comment, 0x445566);
        vm.setSyntaxColor(false, "markdown", model::Token::Heading, 0xABCDEF);
        CHECK(vm.hasSyntaxColors(true, "cmake") && !vm.hasSyntaxColors(true, "markdown"));
    }
    viewmodel::NotepadViewModel vm(nullptr, &files);
    vm.setSettingsPath(path);
    CHECK(vm.syntaxColor(true, "cmake", model::Token::Function) == std::optional<uint32_t>(0x112233));
    CHECK(vm.syntaxColor(true, "*", model::Token::Comment) == std::optional<uint32_t>(0x445566));
    CHECK(vm.syntaxColor(false, "markdown", model::Token::Heading) == std::optional<uint32_t>(0xABCDEF));
    CHECK(!vm.syntaxColor(false, "cmake", model::Token::Function)); // per theme
    vm.resetSyntaxColors(true, "cmake");
    CHECK(!vm.syntaxColor(true, "cmake", model::Token::Function));
    CHECK(vm.syntaxColor(true, "*", model::Token::Comment).has_value());
    vm.resetSyntaxColors(true, "");
    CHECK(!vm.hasSyntaxColors(true, "*") && vm.hasSyntaxColors(false, "markdown"));
    std::remove(path.c_str());
}

// ---- Smart indentation --------------------------------------------------------------------------

// Types `keys` into a new document of `language` like a user: '\n' is Enter, the rest one key each.
std::string typeIn(model::Language language, const std::string& keys, bool smart = true)
{
    viewmodel::NotepadViewModel vm;
    vm.newDocument();
    vm.setDocumentLanguage(language);
    vm.smartIndent.set(smart);
    for (char c : keys) {
        if (c == '\n')
            vm.newline();
        else
            vm.insertText(std::string(1, c));
    }
    std::string text = vm.activeDocument()->buffer.text();
    return text;
}

TEST(smart_indent_brace_languages)
{
    using L = model::Language;
    CHECK_EQ(typeIn(L::Cpp, "int f() {\nreturn 1;\n}"), std::string("int f() {\n    return 1;\n}"));
    CHECK_EQ(typeIn(L::Cpp, "void g()\n{\nif (x) {\ny();\n}\n}"), std::string("void g()\n{\n    if (x) {\n        y();\n    }\n}"));
    CHECK_EQ(typeIn(L::JavaScript, "f([\n1,\n]);"), std::string("f([\n    1,\n]);"));
    CHECK_EQ(typeIn(L::Go, "func f() {\nx()\n}"), std::string("func f() {\n\tx()\n}")); // Go indents with tabs
    // A comment after the opener does not hide it; a block comment continues with " * ".
    CHECK_EQ(typeIn(L::Cpp, "if (a) { // why\nb;"), std::string("if (a) { // why\n    b;"));
    CHECK_EQ(typeIn(L::Cpp, "/**\nDoc.\n*/\nint x;"), std::string("/**\n * Doc.\n */\nint x;"));
    CHECK_EQ(typeIn(L::Cpp, "struct A {\n/**\nDoc.\n*/\nint x;"), std::string("struct A {\n    /**\n     * Doc.\n     */\n    int x;"));
    // Without smart indent, Enter only keeps the indentation.
    CHECK_EQ(typeIn(L::Cpp, "int f() {\nreturn 1;\n}", false), std::string("int f() {\nreturn 1;\n}"));

    // Enter between braces puts the closer on its own line, caret on the indented line between.
    viewmodel::NotepadViewModel vm;
    vm.newDocument();
    vm.setDocumentLanguage(L::Cpp);
    vm.insertText("f() {}");
    vm.setCaret({0, 5}, false);
    vm.newline();
    CHECK_EQ(vm.activeDocument()->buffer.text(), std::string("f() {\n    \n}"));
    CHECK(vm.activeDocument()->caret == (TextPos{1, 4}));
    // ...and it is one undo step.
    vm.execute(CommandId::Undo);
    CHECK_EQ(vm.activeDocument()->buffer.text(), std::string("f() {}"));
}

TEST(smart_indent_keyword_and_indent_languages)
{
    using L = model::Language;
    CHECK_EQ(typeIn(L::Python, "def f(x):\nif x:\nreturn 1\nelse:\npass\n"),
             std::string("def f(x):\n    if x:\n        return 1\n    else:\n        pass\n    "));
    // "else:" typed at the body's level moves out to its "if".
    CHECK_EQ(typeIn(L::Python, "if a:\nb = 1\nelse:"), std::string("if a:\n    b = 1\nelse:"));
    CHECK_EQ(typeIn(L::CMake, "if(WIN32)\nset(A 1)\nelse()\nset(A 2)\nendif()\n"),
             std::string("if(WIN32)\n    set(A 1)\nelse()\n    set(A 2)\nendif()\n"));
    CHECK_EQ(typeIn(L::CMake, "set(SRC\na.cpp\n)"), std::string("set(SRC\n    a.cpp\n)"));
    CHECK_EQ(typeIn(L::Lua, "function f()\nif x then\nreturn 1\nend\nend\n"),
             std::string("function f()\n    if x then\n        return 1\n    end\nend\n"));
    CHECK_EQ(typeIn(L::Shell, "for f in *; do\necho $f\ndone\n"), std::string("for f in *; do\n    echo $f\ndone\n"));
    CHECK_EQ(typeIn(L::Yaml, "jobs:\nbuild:\nruns: x"), std::string("jobs:\n  build:\n    runs: x"));
    CHECK_EQ(typeIn(L::Html, "<ul>\n<li>a</li>\n</ul>"), std::string("<ul>\n  <li>a</li>\n</ul>"));
    CHECK_EQ(typeIn(L::Html, "<p>\n<br>\nx"), std::string("<p>\n  <br>\n  x")); // void elements stay open-free
}

TEST(two_space_indentation)
{
    // Google-style C++ is detected as 2 spaces.
    CHECK(model::detectIndent(model::TextBuffer("int f() {\n  if (x) {\n    y();\n  }\n  return 0;\n}\n")) ==
          std::optional<model::IndentStyle>({false, 2}));

    // A 2-space default for every new document (saved with the settings).
    services::FileService files;
    const std::string path = scratchDirectory() + "/enp-test-settings/indent.json";
    std::remove(path.c_str());
    {
        viewmodel::NotepadViewModel vm(nullptr, &files);
        vm.setSettingsPath(path);
        vm.setPreferredIndent(model::IndentStyle{false, 2});
    }
    viewmodel::NotepadViewModel vm(nullptr, &files);
    vm.setSettingsPath(path);
    CHECK(vm.preferredIndent() == std::optional<model::IndentStyle>({false, 2}));
    vm.newDocument();
    vm.setDocumentLanguage(model::Language::Cpp);
    model::Document& doc = *vm.activeDocument();
    CHECK(doc.indent == (model::IndentStyle{false, 2}));
    for (char c : std::string("int f() {\nif (x) {\ny();\n}\n}")) {
        if (c == '\n')
            vm.newline();
        else
            vm.insertText(std::string(1, c));
    }
    CHECK_EQ(doc.buffer.text(), std::string("int f() {\n  if (x) {\n    y();\n  }\n}"));
    // Tab and Backspace move by 2.
    vm.newline();
    vm.insertIndent();
    vm.insertIndent();
    CHECK_EQ(doc.buffer.line(doc.caret.line), std::string("    "));
    vm.backspace();
    CHECK_EQ(doc.buffer.line(doc.caret.line), std::string("  "));
    // Back to language defaults: Python is 4.
    vm.setPreferredIndent(std::nullopt);
    vm.setDocumentLanguage(model::Language::Python);
    CHECK_EQ(doc.indent.width, 4);
    std::remove(path.c_str());
}

TEST(smart_indent_markdown_lists)
{
    using L = model::Language;
    CHECK_EQ(typeIn(L::Markdown, "- a\nb"), std::string("- a\n- b"));
    CHECK_EQ(typeIn(L::Markdown, "- a\n\nText"), std::string("- a\nText")); // Enter on an empty item ends the list
    CHECK_EQ(typeIn(L::Markdown, "1. a\nb"), std::string("1. a\n2. b"));
    CHECK_EQ(typeIn(L::Markdown, "- [x] done\nnext"), std::string("- [x] done\n- [ ] next"));
}

TEST(indent_style_detection_tab_and_backspace)
{
    CHECK(model::detectIndent(model::TextBuffer("a:\n  b\n    c\n  d\n")) == std::optional<model::IndentStyle>({false, 2}));
    CHECK(model::detectIndent(model::TextBuffer("f() {\n\tx;\n\t\ty;\n}\n")).value().tabs);
    CHECK(model::detectIndent(model::TextBuffer("/*\n * doc\n */\nint f() {\n    x;\n}\n")) == std::optional<model::IndentStyle>({false, 4}));
    CHECK(!model::detectIndent(model::TextBuffer("flat\ntext\n")).has_value());
    CHECK_EQ(model::makeIndent(10, {true, 4}), std::string("\t\t  "));
    CHECK_EQ(model::outdented("        ", {false, 4}), std::string("    "));

    viewmodel::NotepadViewModel vm;
    vm.newDocument();
    model::Document& doc = *vm.activeDocument();
    // Tab goes to the next stop; Backspace in leading spaces goes back one stop.
    vm.insertText("  ");
    vm.insertIndent();
    CHECK_EQ(doc.buffer.line(0), std::string("    "));
    vm.insertIndent();
    CHECK_EQ(doc.buffer.line(0), std::string("        "));
    vm.backspace();
    CHECK_EQ(doc.buffer.line(0), std::string("    "));
    vm.insertText("x ");
    vm.backspace(); // after text: one character
    CHECK_EQ(doc.buffer.line(0), std::string("    x"));

    // Switching to tabs and converting re-writes the indentation.
    vm.selectAll();
    vm.insertText("    a\n        b\n  c");
    vm.execute(CommandId::IndentWithTabs);
    vm.execute(CommandId::ConvertIndentation);
    CHECK_EQ(doc.buffer.text(), std::string("\ta\n\t\tb\n  c"));
    CHECK(doc.indent.tabs && !doc.indentDetected);
    vm.execute(CommandId::DetectIndentation);
    CHECK(doc.indent.tabs && doc.indentDetected);
}

// ---- Logical map --------------------------------------------------------------------------------

TEST(logical_folding_splits_functions_into_steps)
{
    const model::TextBuffer buffer("int run(int n)\n"
                                   "{\n"
                                   "    // Validate the input.\n"
                                   "    if (n < 0)\n"
                                   "        return -1;\n"
                                   "\n"
                                   "    // Accumulate.\n"
                                   "    int sum = 0;\n"
                                   "    for (int i = 0; i < n; ++i)\n"
                                   "        sum += i;\n"
                                   "    return sum;\n"
                                   "}\n");
    const auto syntax = model::buildOutline(buffer, model::Language::Cpp, model::OutlineMode::Syntax);
    const auto logical = model::buildOutline(buffer, model::Language::Cpp, model::OutlineMode::Logical);
    int steps = 0;
    for (const auto& n : syntax)
        CHECK(n.kind != model::OutlineKind::Step);
    std::vector<std::string> titles;
    for (const auto& n : logical)
        if (n.kind == model::OutlineKind::Step) {
            ++steps;
            titles.push_back(n.title);
        }
    CHECK_EQ(steps, 2);
    CHECK(titles.size() == 2 && titles[0].find("Validate") != std::string::npos && titles[1].find("Accumulate") != std::string::npos);
}

std::vector<model::SourceFile> sampleProject()
{
    return {
        {"src/core/Engine.h", "// Runs the simulation.\n#pragma once\nclass Engine {\npublic:\n    void step();\n};\n"},
        {"src/core/Engine.cpp", "#include \"Engine.h\"\n#include \"Clock.h\"\n\n// Advances one tick.\nvoid Engine::step()\n{\n    // Read the time.\n    int t = 0;\n\n    // Update.\n    t++;\n}\n\nvoid Engine::reset()\n{\n    step();\n    step();\n}\n"},
        {"src/core/Clock.h", "// Keeps time.\n#pragma once\nstruct Clock {};\n"},
        {"src/ui/Window.cpp", "#include \"core/Engine.h\"\n\n// Draws the world.\nvoid draw()\n{\n}\n"},
    };
}

TEST(logical_map_dissects_modules_components_and_relations)
{
    const model::LogicalMap map = model::dissect("Sim", sampleProject(), "");
    CHECK_EQ(map.source, std::string("local"));
    const model::LogicalNode* root = map.find(model::LogicalMap::kArchitectureRoot);
    CHECK(root && root->children.size() == 2);
    const model::LogicalNode* engine = map.find("component:core/Engine");
    CHECK(engine && engine->code.size() == 2);
    CHECK(engine && engine->summary.find("simulation") != std::string::npos);
    CHECK(map.find("component:core/Clock") != nullptr);
    // ui uses core: lifted to the modules.
    bool uiUsesCore = false;
    for (const auto& e : map.edgesAmongChildren(model::LogicalMap::kArchitectureRoot))
        uiUsesCore = uiUsesCore || (e.from == "module:ui" && e.to == "module:core");
    CHECK(uiUsesCore);
    bool engineUsesClock = false;
    for (const auto& e : map.edgesAmongChildren("module:core"))
        engineUsesClock = engineUsesClock || (e.from == "component:core/Engine" && e.to == "component:core/Clock");
    CHECK(engineUsesClock);
    // Calls between a component's functions (reset calls step twice); single-step functions have no steps.
    int resetCallsStep = 0;
    for (const auto& e : map.edgesAmongChildren("component:core/Engine"))
        if (map.find(e.from)->name.rfind("reset", 0) == 0 && map.find(e.to)->name.rfind("step", 0) == 0)
            resetCallsStep = e.weight;
    CHECK_EQ(resetCallsStep, 2);
    for (const auto& n : map.nodes)
        if (n.kind == model::LogicalKind::Function && n.name.rfind("reset", 0) == 0)
            CHECK(n.children.empty() && n.summary == "5 lines.");
    // The caret inside Engine::step resolves to that function, under the component.
    const std::string at = map.nodeAt("src/core/Engine.cpp", 7);
    CHECK(at.rfind("step:", 0) == 0 || at.rfind("function:", 0) == 0);
    const auto path = map.pathTo(at);
    CHECK(path.size() >= 4 && path[0] == "system" && path[1] == "module:core" && path[2] == "component:core/Engine");

    // JSON round trip keeps the structure.
    const auto back = model::LogicalMap::fromJson(map.toJson());
    CHECK(back && back->nodes.size() == map.nodes.size() && back->edges.size() == map.edges.size());
}

TEST(logical_map_merges_agent_features)
{
    model::LogicalMap map = model::dissect("Sim", sampleProject(), "");
    CHECK(model::agentPrompt(map).find("component:core/Engine") != std::string::npos);
    std::string error;
    CHECK(!model::mergeAgentAnalysis(map, "sorry, no JSON here", &error));
    CHECK(!error.empty());
    const std::string answer =
        "Here you go:\n```json\n"
        "{\"system\":{\"summary\":\"A tiny simulator.\"},"
        "\"modules\":[{\"id\":\"module:core\",\"summary\":\"The simulation.\",\"responsibilities\":[\"ticks\"]}],"
        "\"components\":[{\"id\":\"component:core/Clock\",\"summary\":\"The time source.\"}],"
        "\"features\":[{\"name\":\"Run the world\",\"summary\":\"The world moves.\",\"steps\":["
        "{\"component\":\"component:core/Engine\",\"action\":\"advance\"},"
        "{\"component\":\"component:ui/Window\",\"action\":\"draw\"}]}]}\n```\n";
    CHECK(model::mergeAgentAnalysis(map, answer, &error));
    CHECK_EQ(map.source, std::string("agent"));
    CHECK_EQ(map.find("system")->summary, std::string("A tiny simulator."));
    CHECK_EQ(map.find("component:core/Clock")->summary, std::string("The time source."));
    const model::LogicalNode* features = map.find(model::LogicalMap::kFeaturesRoot);
    CHECK(features && features->children.size() == 1);
    const model::LogicalNode* feature = features ? map.find(features->children[0]) : nullptr;
    CHECK(feature && feature->name == "Run the world" && feature->children.size() == 2);
    CHECK(feature && map.find(feature->children[0])->target == "component:core/Engine");
}

struct FakeAgentCli : interfaces::IAgentCliService {
    std::string answer;
    std::string prompt;
    std::function<void(bool, const std::string&)> pending;
    std::string command() const override { return "fake-agent"; }
    bool available() const override { return true; }
    void run(const std::string& p, const std::string&, std::function<void(bool, const std::string&)> done) override
    {
        prompt = p;
        pending = std::move(done);
    }
};

struct FakeProcesses : interfaces::IProcessService {
    std::map<std::string, std::string> settings;
    std::string lastCommand, lastDirectory, output;
    void run(const std::string& commandLine, const std::string& workingDirectory,
             std::function<void(const interfaces::ProcessResult&)> done) override
    {
        lastCommand = commandLine;
        lastDirectory = workingDirectory;
        done({true, 1, output, {}});
    }
    std::optional<std::string> setting(const std::string& name) const override
    {
        const auto it = settings.find(name);
        return it == settings.end() ? std::nullopt : std::optional<std::string>(it->second);
    }
};

TEST(tool_output_parses_into_findings)
{
    const std::string out = "C:/w/src/a.cpp:12:5: warning: unused variable 'x' [-Wunused-variable]\n"
                            "src/b.py:3:1: E302 expected 2 blank lines\n"
                            "./c.sh:7: error: missing quote\r\n"
                            "In file included from somewhere\n"
                            "make: *** [all] Error 1\n";
    const auto f = model::parseToolOutput(out, model::ReviewTool::Lint, "C:/w");
    CHECK_EQ(f.size(), size_t(3));
    CHECK(f[0].file == "src/a.cpp" && f[0].line == 11 && f[0].severity == model::Severity::Warning);
    CHECK(f[0].rule == "-Wunused-variable" && f[0].message == "unused variable 'x'");
    CHECK(f[1].file == "src/b.py" && f[1].line == 2 && f[1].message == "E302 expected 2 blank lines");
    CHECK(f[2].file == "c.sh" && f[2].severity == model::Severity::Error && f[2].message == "missing quote");
}

TEST(real_workspace_uses_real_agent_and_tools)
{
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-live";
    os::fs::createDirectories(root + "/src");
    files.write(root + "/src/a.cpp", "int main() { int x; }\n", nullptr);
    FakeAgentCli agent;
    FakeProcesses processes;

    // The sample workspace keeps its samples.
    viewmodel::NotepadViewModel sample(nullptr, &files, &agent, &processes);
    CHECK(!sample.agent().messages().empty() && !sample.review().findings().empty() && !sample.review().comments().empty());

    viewmodel::NotepadViewModel vm(nullptr, &files, &agent, &processes);
    CHECK(vm.openFolder(root));
    CHECK(vm.agent().live() && vm.agent().messages().empty() && vm.agent().changes().empty());
    CHECK(vm.review().findings().empty() && vm.review().comments().empty() && vm.review().checklist().empty());
    vm.openFile("src/a.cpp");
    vm.setCaret({0, 13}, false);

    // The chat goes to the agent CLI with context; the answer comes back as the reply.
    vm.agent().submitPrompt("What does main do?", "src/a.cpp:1");
    CHECK(vm.agent().busy.get() && agent.pending != nullptr);
    CHECK(agent.prompt.find("What does main do?") != std::string::npos && agent.prompt.find("src/a.cpp:1") != std::string::npos);
    agent.pending(true, "It declares an unused int.");
    CHECK(!vm.agent().busy.get());
    CHECK(vm.agent().messages().back().text == "It declares an unused int.");
    // Stop drops a late answer.
    vm.agent().submitPrompt("And then?");
    auto late = agent.pending;
    vm.agent().stop();
    late(true, "too late");
    CHECK(vm.agent().messages().back().text != "too late");

    // An unconfigured tool says so; a configured one runs in the workspace on the active file.
    vm.review().runTool(model::ReviewTool::Lint);
    CHECK(vm.review().findings().empty() && !vm.review().hasRun(model::ReviewTool::Lint));
    processes.settings["EASYNOTEPAD_LINT"] = "clang-tidy {file}";
    processes.output = vm.workspaceRoot() + "/src/a.cpp:1:18: warning: unused variable 'x'\n";
    vm.review().runTool(model::ReviewTool::Lint);
    CHECK(processes.lastCommand.find("clang-tidy \"") == 0 && processes.lastCommand.find("src/a.cpp\"") != std::string::npos);
    CHECK_EQ(processes.lastDirectory, vm.workspaceRoot());
    CHECK_EQ(vm.review().findings().size(), size_t(1));
    CHECK(vm.review().findings()[0].file == "src/a.cpp" && vm.review().findings()[0].line == 0);
    CHECK_EQ(vm.review().findingsForFile("src/a.cpp").size(), size_t(1));

    // Agent Review asks the agent CLI for findings in the same format.
    vm.review().runTool(model::ReviewTool::AgentReview);
    CHECK(agent.prompt.find("Review the file") == 0);
    agent.pending(true, "src/a.cpp:1: info: main returns nothing explicitly\n");
    CHECK_EQ(vm.review().count(model::ReviewTool::AgentReview), 1);
}

TEST(logical_map_view_model_zooms_opens_and_dissects)
{
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-logic";
    std::remove(os::fs::path::join(root, viewmodel::LogicalMapViewModel::kCachePath).c_str()); // from an earlier run
    for (const auto& f : sampleProject()) {
        os::fs::createDirectories(os::fs::path::join(root, f.path.substr(0, f.path.rfind('/'))));
        files.write(os::fs::path::join(root, f.path), f.text, nullptr);
    }
    FakeAgentCli agent;
    viewmodel::NotepadViewModel vm(nullptr, &files, &agent);
    CHECK(vm.openFolder(root));
    viewmodel::LogicalMapViewModel& logic = vm.logicalMap();
    CHECK_EQ(logic.focus(), std::string("system"));
    CHECK(!logic.hasFeatures());

    vm.execute(CommandId::ShowMapView);
    CHECK(vm.editorMode.get() == viewmodel::EditorMode::Map);
    logic.zoomIn("module:core");
    CHECK_EQ(logic.focus(), std::string("module:core"));
    CHECK(logic.breadcrumb().size() == 2 && logic.zoomDirection() > 0);
    logic.zoomIn("component:core/Engine");
    CHECK_EQ(logic.focus(), std::string("component:core/Engine"));
    logic.zoomOut();
    CHECK_EQ(logic.focus(), std::string("module:core"));

    // Opening a function shows its code.
    const model::LogicalNode* engine = logic.map().find("component:core/Engine");
    std::string fn;
    for (const auto& c : engine->children)
        if (fn.empty() && logic.map().find(c)->kind == model::LogicalKind::Function)
            fn = c;
    CHECK(!fn.empty());
    logic.open(fn);
    CHECK(vm.editorMode.get() == viewmodel::EditorMode::Code);
    CHECK(vm.activeDocument() && vm.activeDocument()->path.find("Engine.cpp") != std::string::npos);
    CHECK_EQ(vm.activeDocument()->caret.line, 4);

    // Back to the map: it reveals where the caret is.
    vm.execute(CommandId::ShowMapView);
    CHECK_EQ(logic.focus(), std::string("component:core/Engine"));
    CHECK(!logic.highlight().empty());

    // Agent dissection (asynchronous): analyzing until the CLI answers, then merged and cached.
    vm.execute(CommandId::DissectProject);
    CHECK(logic.analyzing.get());
    CHECK(agent.prompt.find("component:core/Engine") != std::string::npos);
    CHECK(agent.pending != nullptr);
    agent.pending(true, "{\"features\":[{\"name\":\"Tick\",\"summary\":\"Time passes.\",\"steps\":["
                        "{\"component\":\"component:core/Clock\",\"action\":\"read the time\"}]}]}");
    CHECK(!logic.analyzing.get());
    CHECK(logic.hasFeatures());
    CHECK(logic.lens.get() == viewmodel::MapLens::Features);
    CHECK(files.read(os::fs::path::join(root, viewmodel::LogicalMapViewModel::kCachePath)).has_value());

    // A feature's step jumps to its component in the architecture.
    const std::string feature = logic.map().find(model::LogicalMap::kFeaturesRoot)->children[0];
    logic.zoomIn(feature);
    const std::string step = logic.map().find(feature)->children[0];
    logic.zoomIn(step);
    CHECK(logic.lens.get() == viewmodel::MapLens::Architecture);
    CHECK_EQ(logic.focus(), std::string("module:core")); // Clock has no functions: shown in its module
    CHECK_EQ(logic.highlight(), std::string("component:core/Clock"));

    // A new session loads the agent's map from the cache.
    viewmodel::NotepadViewModel again(nullptr, &files, nullptr);
    CHECK(again.openFolder(root));
    CHECK_EQ(again.logicalMap().map().source, std::string("agent"));

    // Logic scope in the layers mirrors the map.
    vm.layers().scope.set(viewmodel::LayerScope::Logic);
    vm.logicalMap().lens.set(viewmodel::MapLens::Architecture);
    const auto& layers = vm.layers().nodes();
    CHECK(!layers.empty() && layers[0].id == "system");
}

// ---- Terminal emulator -------------------------------------------------------------------------

std::string rowText(const model::TerminalScreen& t, int row)
{
    return t.text(t.scrollbackSize() + row, 0, t.scrollbackSize() + row, t.columns());
}

TEST(terminal_prints_wraps_and_scrolls_into_scrollback)
{
    model::TerminalScreen t(5, 2);
    t.feed("abcdefg");
    CHECK_EQ(rowText(t, 0), std::string("abcde"));
    CHECK_EQ(rowText(t, 1), std::string("fg"));
    CHECK(t.row(0).wrapped);
    CHECK_EQ(t.text(0, 0, 1, 5), std::string("abcdefg")); // soft wrap joins without a newline
    t.feed("\r\nxy\r\nz");
    CHECK_EQ(t.scrollbackSize(), 2); // "abcde" and "fg"
    CHECK_EQ(rowText(t, 0), std::string("xy"));
    CHECK_EQ(rowText(t, 1), std::string("z"));
    CHECK_EQ(t.cursorColumn(), 1);
}

TEST(terminal_cursor_movement_and_erase)
{
    model::TerminalScreen t(10, 3);
    t.feed("hello\x1b[2;3HX");
    CHECK_EQ(t.cursorRow(), 1);
    CHECK_EQ(rowText(t, 1), std::string("  X"));
    t.feed("\x1b[1;3H\x1b[K");
    CHECK_EQ(rowText(t, 0), std::string("he"));
    t.feed("\x1b[2J");
    CHECK_EQ(rowText(t, 1), std::string(""));
    t.feed("\x1b[3;1Habc\x1b[2D\x1b[1P");
    CHECK_EQ(rowText(t, 2), std::string("ac"));
}

TEST(terminal_sgr_colours_and_attributes)
{
    model::TerminalScreen t(20, 2);
    t.feed("\x1b[1;31mR\x1b[38;5;200mI\x1b[38;2;1;2;3mT\x1b[0mn");
    const auto& cells = t.row(0).cells;
    CHECK(cells[0].attr.fg == model::termcolor::indexed(1));
    CHECK((cells[0].attr.flags & model::kBold) != 0);
    CHECK(cells[1].attr.fg == model::termcolor::indexed(200));
    CHECK(cells[2].attr.fg == model::termcolor::rgb(1, 2, 3));
    CHECK(cells[3].attr == model::TermAttr());
    const uint32_t base[16] = {};
    CHECK_EQ(model::termcolor::paletteRgb(196, base), uint32_t(0xFF0000));
    CHECK_EQ(model::termcolor::paletteRgb(232, base), uint32_t(0x080808));
}

TEST(terminal_utf8_split_across_feeds)
{
    model::TerminalScreen t(10, 1);
    t.feed("\xE2\x82");
    t.feed("\xAC!"); // euro sign
    CHECK(t.row(0).cells[0].ch == U'€');
    CHECK(t.row(0).cells[1].ch == U'!');
    CHECK_EQ(rowText(t, 0), std::string("\xE2\x82\xAC!"));
}

TEST(terminal_alternate_screen_restores_main)
{
    model::TerminalScreen t(10, 2);
    t.feed("shell$ ");
    t.feed("\x1b[?1049h\x1b[Hvim");
    CHECK(t.alternateScreen());
    CHECK_EQ(rowText(t, 0), std::string("vim"));
    t.feed("\x1b[?1049l");
    CHECK(!t.alternateScreen());
    CHECK_EQ(rowText(t, 0), std::string("shell$"));
    CHECK_EQ(t.cursorColumn(), 7);
}

TEST(terminal_scroll_region_and_modes)
{
    model::TerminalScreen t(5, 4);
    t.feed("1\r\n2\r\n3\r\n4");
    t.feed("\x1b[2;3r\x1b[3;1H\n"); // scroll only rows 2-3
    CHECK_EQ(rowText(t, 0), std::string("1"));
    CHECK_EQ(rowText(t, 1), std::string("3"));
    CHECK_EQ(rowText(t, 2), std::string(""));
    CHECK_EQ(rowText(t, 3), std::string("4"));
    CHECK_EQ(t.scrollbackSize(), 0); // partial regions never feed the scrollback
    t.feed("\x1b[?25l\x1b[?1h\x1b[?2004h");
    CHECK(!t.cursorVisible());
    CHECK(t.applicationCursorKeys());
    CHECK(t.bracketedPaste());
}

TEST(terminal_answers_status_queries_and_reads_titles)
{
    model::TerminalScreen t(10, 5);
    t.feed("\x1b[3;4H\x1b[6n\x1b[c");
    CHECK_EQ(t.takeResponses(), std::string("\x1b[3;4R\x1b[?1;2c"));
    CHECK_EQ(t.takeResponses(), std::string());
    t.feed("\x1b]0;my title\x07\x1b]2;other\x1b\\");
    CHECK_EQ(t.title(), std::string("other"));
}

TEST(terminal_resize_keeps_cursor_line)
{
    model::TerminalScreen t(10, 4);
    t.feed("a\r\nb\r\nc\r\nprompt");
    t.resize(6, 2);
    CHECK_EQ(t.rows(), 2);
    CHECK_EQ(rowText(t, t.cursorRow()), std::string("prompt"));
    CHECK_EQ(t.scrollbackSize(), 2);
    t.clearKeepingCursorLine();
    CHECK_EQ(t.scrollbackSize(), 0);
    CHECK_EQ(rowText(t, 0), std::string("prompt"));
}

TEST(terminal_key_encoding)
{
    using model::TermKey;
    const model::TermMods none;
    CHECK_EQ(model::encodeKey(TermKey::Up, none, false), std::string("\x1b[A"));
    CHECK_EQ(model::encodeKey(TermKey::Up, none, true), std::string("\x1bOA"));
    CHECK_EQ(model::encodeKey(TermKey::Right, model::TermMods{false, false, true}, false), std::string("\x1b[1;5C"));
    CHECK_EQ(model::encodeKey(TermKey::Delete, none, false), std::string("\x1b[3~"));
    CHECK_EQ(model::encodeKey(TermKey::F5, model::TermMods{true, false, false}, false), std::string("\x1b[15;2~"));
    CHECK_EQ(model::encodeKey(TermKey::Tab, model::TermMods{true, false, false}, false), std::string("\x1b[Z"));
    CHECK_EQ(model::encodeControl('c'), std::string("\x03"));
    CHECK_EQ(model::encodeControl('['), std::string("\x1b"));
    CHECK_EQ(model::encodePaste("a\r\nb", true), std::string("\x1b[200~a\rb\x1b[201~"));
}

// ---- Terminal view-model ------------------------------------------------------------------------

TEST(terminal_view_model_runs_sessions)
{
    tests::FakeTerminalService service;
    viewmodel::TerminalViewModel vm(&service);
    CHECK(vm.supported());
    vm.resize(40, 10);
    CHECK(vm.newTerminal(0));
    service.pump();
    viewmodel::TerminalInstance* t = vm.active();
    CHECK(t != nullptr);
    CHECK_EQ(service.sessions.back()->columns, 40);
    CHECK_EQ(rowText(t->screen, 0), std::string("Fake shell"));

    vm.sendInput("ls\r");
    service.pump();
    CHECK_EQ(rowText(t->screen, 2), std::string("ran: ls"));

    vm.resize(30, 8); // forwarded to the session
    CHECK_EQ(service.sessions.back()->columns, 30);
    CHECK_EQ(t->screen.rows(), 8);

    // A cursor-position query from the program is answered through the session.
    service.queue(service.sessions.back(), "\x1b[6n");
    service.pump();
    const std::string& written = service.sessions.back()->written;
    CHECK(written.rfind("\x1b[") != std::string::npos && written.back() == 'R'); // "ESC[row;colR"

    CHECK(vm.newTerminal(1));
    CHECK_EQ(vm.terminals().size(), size_t(2));
    CHECK_EQ(vm.activeIndex.get(), 1);
    vm.close(1);
    CHECK_EQ(vm.terminals().size(), size_t(1));
    CHECK_EQ(vm.activeIndex.get(), 0);
}

TEST(terminal_view_model_exit_and_errors)
{
    tests::FakeTerminalService service;
    viewmodel::TerminalViewModel vm(&service);
    vm.newTerminal(0);
    service.pump();
    vm.active()->session->terminate();
    service.pump();
    CHECK(vm.active()->exited);
    CHECK_EQ(vm.active()->exitCode, 1);
    vm.sendInput("x"); // any key closes an exited terminal
    CHECK(vm.terminals().empty());

    service.failStart = true;
    CHECK(!vm.newTerminal(0));
    CHECK_EQ(vm.lastError.get(), std::string("fake start failure"));

    viewmodel::TerminalViewModel none(nullptr);
    CHECK(!none.supported());
    CHECK(!none.newTerminal(0));
}

TEST(toggle_terminal_opens_panel_and_starts_shell)
{
    tests::FakeTerminalService service;
    viewmodel::NotepadViewModel vm(&service);
    vm.showPanel.set(false);
    vm.execute(CommandId::ToggleTerminal);
    CHECK(vm.showPanel.get());
    CHECK(vm.panelTab.get() == viewmodel::PanelTab::Terminal);
    CHECK_EQ(vm.terminal().terminals().size(), size_t(1));
    vm.execute(CommandId::ToggleTerminal); // second press hides the panel
    CHECK(!vm.showPanel.get());
    vm.execute(CommandId::NewTerminal);
    CHECK_EQ(vm.terminal().terminals().size(), size_t(2));
}

// ---- JSON and the MCP connector ------------------------------------------------------------------

TEST(json_round_trip_and_errors)
{
    const auto j = model::Json::parse(R"({"a":[1,2.5,"x\né"],"b":{"c":true,"d":null}})");
    CHECK(j.has_value());
    CHECK_EQ(j->find("a")->items().size(), size_t(3));
    CHECK_EQ(j->find("a")->items()[2].asString(), std::string("x\n\xC3\xA9"));
    CHECK(j->find("b")->getBool("c"));
    CHECK(j->find("b")->find("d")->isNull());
    CHECK_EQ(model::Json::parse(j->dump())->dump(), j->dump());
    CHECK_EQ(model::Json::object().set("n", 3).set("s", "q\"").dump(), std::string(R"({"n":3,"s":"q\""})"));
    std::string error;
    CHECK(!model::Json::parse("{\"a\":}", &error));
    CHECK(!error.empty());
    CHECK(!model::Json::parse("[1,2] x"));
}

TEST(mcp_session_speaks_json_rpc)
{
    std::vector<std::string> calls;
    app::McpSession session(
        [&](const std::string& method, const model::Json& params, std::string* error) -> std::optional<model::Json> {
            calls.push_back(method + " " + params.dump());
            if (method == "click") {
                *error = "no element";
                return std::nullopt;
            }
            return model::Json::object().set("ok", true);
        },
        "9.9");
    auto init = session.handle(*model::Json::parse(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})"));
    CHECK(init && init->find("result")->getString("protocolVersion") == "2025-06-18");
    CHECK(init->find("result")->find("serverInfo")->getString("version") == "9.9");
    CHECK(!session.handle(*model::Json::parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})")));

    auto list = session.handle(*model::Json::parse(R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})"));
    CHECK(list && list->find("result")->find("tools")->items().size() == app::automationTools().size());

    auto call = session.handle(*model::Json::parse(
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"get_state","arguments":{"x":1}}})"));
    CHECK(call && !call->find("result")->getBool("isError", true));
    CHECK_EQ(calls.back(), std::string("get_state {\"x\":1}"));

    auto failed = session.handle(*model::Json::parse(
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"click","arguments":{"id":"nope"}}})"));
    CHECK(failed && failed->find("result")->getBool("isError"));

    auto unknown = session.handle(*model::Json::parse(R"({"jsonrpc":"2.0","id":5,"method":"resources/list"})"));
    CHECK(unknown && unknown->find("error")->getNumber("code") == -32601);
}

TEST(automation_controller_drives_the_view_model)
{
    tests::FakeTerminalService service;
    viewmodel::NotepadViewModel vm(&service);
    vm.openStartupDocuments({});
    auto state = app::handleAutomation(vm, "get_state", model::Json::object());
    CHECK_EQ(state.find("activeDocument")->getString("path"), std::string("src/main.cpp"));

    auto set = app::handleAutomation(vm, "set_detail_level", model::Json::object().set("level", "Overview"));
    CHECK(!set.find("error"));
    CHECK_EQ(vm.detailLevel.get(), 2);
    auto outline = app::handleAutomation(vm, "read_outline", model::Json::object());
    CHECK(!outline.find("regions")->items().empty());

    // Commands work even when their button is not on screen (no UI in this test).
    auto clicked = app::handleAutomation(vm, "click", model::Json::object().set("id", "command:ToggleTerminal"));
    CHECK(!clicked.find("error"));
    CHECK(vm.panelTab.get() == viewmodel::PanelTab::Terminal);
    CHECK(vm.agentPointer.get().visible && vm.agentPointer.get().click);

    auto bad = app::handleAutomation(vm, "click", model::Json::object().set("id", "nowhere"));
    CHECK(bad.find("error") != nullptr);

    app::handleAutomation(vm, "run_in_terminal", model::Json::object().set("command", "echo hi"));
    service.pump();
    CHECK(service.sessions.back()->written.find("echo hi\r") != std::string::npos);

    auto pointed = app::handleAutomation(vm, "point", model::Json::object().set("id", "layers-map").set("message", "Look"));
    CHECK(!pointed.find("error"));
    CHECK_EQ(vm.agentPointer.get().message, std::string("Look"));
}

TEST(automation_endpoint_end_to_end)
{
    // A private endpoint file, so a running EasyNotepad is not disturbed.
#if defined(_WIN32)
    _putenv_s("EASYNOTEPAD_ENDPOINT", (os::fs::appDataDirectory() + "/test-endpoint.json").c_str());
#else
    setenv("EASYNOTEPAD_ENDPOINT", (os::fs::appDataDirectory() + "/test-endpoint.json").c_str(), 1);
#endif
    services::AutomationServer server([](const std::string& method, const model::Json& params) {
        return model::Json::object().set("method", method).set("echo", params);
    });
    std::string error;
    CHECK(server.start(&error));
    if (!error.empty())
        std::printf("  start error: %s\n", error.c_str());

    std::optional<model::Json> reply;
    std::string callError;
    std::atomic<bool> done{false};
    std::thread client([&]() {
        reply = app::forwardToRunningApp("get_state", model::Json::object().set("k", "v"), &callError);
        done = true;
    });
    for (int i = 0; i < 400 && !done; ++i) { // the handler runs here, on the "UI thread"
        os::dispatch::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    client.join();
    CHECK(reply.has_value());
    if (reply) {
        CHECK_EQ(reply->getString("method"), std::string("get_state"));
        CHECK_EQ(reply->find("echo")->getString("k"), std::string("v"));
    } else {
        std::printf("  call error: %s\n", callError.c_str());
    }
    server.stop();
    os::fs::writeFile(services::AutomationServer::endpointPath(), ""); // leave nothing behind
    std::string stoppedError;
    CHECK(!app::forwardToRunningApp("get_state", model::Json::object(), &stoppedError));
    CHECK(!stoppedError.empty());
}

// ---- Pseudo-terminal backend (real process) -----------------------------------------------------

TEST(pty_runs_a_real_process)
{
    if (!os::pty::supported()) {
        std::printf("  (skipped: no pseudo-terminal support)\n");
        return;
    }
    os::pty::Options options;
#if defined(_WIN32)
    options.command = "cmd.exe /c echo enp-pty-ok";
#else
    options.command = "/bin/sh";
    options.arguments = {"-c", "echo enp-pty-ok"};
#endif
    std::mutex mutex;
    std::string output;
    std::atomic<bool> exited{false};
    std::atomic<int> code{-1};
    std::string error;
    auto process = os::pty::spawn(
        options,
        [&](std::string data) {
            std::lock_guard<std::mutex> lock(mutex);
            output += data;
        },
        [&](int c) {
            code = c;
            exited = true;
        },
        &error);
    CHECK(process != nullptr);
    if (!process) {
        std::printf("  spawn error: %s\n", error.c_str());
        return;
    }
    for (int i = 0; i < 200 && !exited; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(exited);
    CHECK_EQ(code.load(), 0);
    std::lock_guard<std::mutex> lock(mutex);
    model::TerminalScreen screen(80, 24);
    screen.feed(output);
    CHECK(screen.text(0, 0, screen.totalLines() - 1, 80).find("enp-pty-ok") != std::string::npos);
}

#if !defined(_WIN32) // top and htop are POSIX programs
// Runs a full-screen program the way the terminal panel does (output into the emulator, its
// status replies back to the program), lets it draw, presses `quitKeys`, and returns the screen as
// it was while the program ran. Empty when the program is not installed.
struct FullScreenRun {
    bool ran = false;
    bool exited = false;
    bool usedAlternateScreen = false;
    bool backOnMainScreen = false;
    std::string screenText; // while running
};

FullScreenRun runFullScreen(const std::string& program, const std::vector<std::string>& arguments, const std::string& quitKeys)
{
    FullScreenRun run;
    const std::string path = program.find('/') != std::string::npos ? program : os::process::findExecutable(program);
    if (path.empty() || !os::pty::supported())
        return run;
    os::pty::Options options;
    options.command = path;
    options.arguments = arguments;
    options.columns = 120;
    options.rows = 32;
    std::mutex mutex;
    std::string pending;
    std::atomic<bool> exited{false};
    auto process = os::pty::spawn(
        options,
        [&](std::string data) {
            std::lock_guard<std::mutex> lock(mutex);
            pending += data;
        },
        [&](int) { exited = true; });
    if (!process)
        return run;
    run.ran = true;
    model::TerminalScreen screen(120, 32);
    auto pump = [&](int milliseconds) {
        for (int t = 0; t < milliseconds; t += 20) {
            std::string data;
            {
                std::lock_guard<std::mutex> lock(mutex);
                data.swap(pending);
            }
            if (!data.empty()) {
                screen.feed(data);
                run.usedAlternateScreen = run.usedAlternateScreen || screen.alternateScreen();
                const std::string replies = screen.takeResponses();
                if (!replies.empty())
                    process->write(replies);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    };
    pump(2500); // a refresh or two
    run.screenText = screen.text(screen.scrollbackSize(), 0, screen.totalLines(), screen.columns());
    process->write(quitKeys);
    for (int i = 0; i < 100 && !exited; ++i)
        pump(50);
    pump(100);
    run.exited = exited;
    run.backOnMainScreen = !screen.alternateScreen();
    if (!exited)
        process->terminate();
    return run;
}
#endif

TEST(pty_runs_full_screen_programs)
{
#if defined(_WIN32)
    std::printf("  (skipped: top / htop are POSIX programs)\n");
#else
    // top: header, task summary and the process table on the alternate screen; 'q' quits.
    const FullScreenRun top = runFullScreen("top", {"-d", "1"}, "q");
    if (!top.ran) {
        std::printf("  (skipped: top not found)\n");
    } else {
        CHECK(top.screenText.find("load average") != std::string::npos);
        CHECK(top.screenText.find("Tasks:") != std::string::npos);
        CHECK(top.screenText.find("PID") != std::string::npos && top.screenText.find("COMMAND") != std::string::npos);
        // procps top redraws the main screen (no alternate screen); htop below uses it.
        CHECK(top.exited && top.backOnMainScreen);
        if (top.screenText.find("load average") == std::string::npos || std::getenv("ENP_SHOW_SCREENS"))
            std::printf("  top screen:\n%s\n", top.screenText.c_str());
    }
    // htop: meters, the process list and the function-key bar; F10 (or q) quits. ENP_HTOP points
    // at a binary that is not on PATH.
    const char* htopPath = std::getenv("ENP_HTOP");
    const FullScreenRun htop = runFullScreen(htopPath ? htopPath : "htop", {"-d", "10"}, "q");
    if (!htop.ran) {
        std::printf("  (skipped: htop not found)\n");
    } else {
        CHECK(htop.screenText.find("PID") != std::string::npos);
        CHECK(htop.screenText.find("Load average") != std::string::npos);
        CHECK(htop.screenText.find("F10") != std::string::npos || htop.screenText.find("Quit") != std::string::npos);
        CHECK(htop.usedAlternateScreen);
        CHECK(htop.exited && htop.backOnMainScreen);
        if (htop.screenText.find("Load average") == std::string::npos || std::getenv("ENP_SHOW_SCREENS"))
            std::printf("  htop screen:\n%s\n", htop.screenText.c_str());
    }
#endif
}

} // namespace

int main(int argc, char** argv)
{
    os::InitParams params;
    params.argc = argc;
    params.argv = argv;
    os::init(params);
    for (auto& [name, fn] : registry()) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
