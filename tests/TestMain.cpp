// Unit tests for the model, view-models and the pseudo-terminal backend (no framework dependency).
#include "FakeTerminalService.h"

#include "enp/model/Diff.h"
#include "enp/model/Git.h"
#include "enp/model/Document.h"
#include "enp/model/AgentRoute.h"
#include "enp/model/Hash.h"
#include "enp/model/Highlighter.h"
#include "enp/model/Indent.h"
#include "enp/model/Language.h"
#include "enp/model/Outline.h"
#include "enp/model/Symbols.h"
#include "enp/model/TerminalScreen.h"
#include "enp/model/TextBuffer.h"
#include "enp/model/WorkspaceFile.h"
#include "enp/os/Process.h"
#include "enp/os/Pty.h"
#include "enp/viewmodel/DummyData.h"
#include "enp/viewmodel/NotepadViewModel.h"
#include "enp/model/Json.h"
#include "enp/model/LogicalMap.h"
#include "enp/model/BinaryInfo.h"
#include "enp/model/Csv.h"
#include "enp/model/DataTree.h"
#include "enp/model/Emoji.h"
#include "enp/model/FileKind.h"
#include "enp/model/Markdown.h"
#include "enp/model/Math.h"
#include "enp/interfaces/IAgentCliService.h"
#include "enp/interfaces/ILocalEngineService.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Os.h"
#include "enp/os/Memory.h"
#include "enp/services/AutomationServer.h"
#include "enp/services/EmojiPack.h"
#include "enp/services/FileService.h"
#include "enp/services/ImageDecoder.h"
#include "enp/services/ImageEncoder.h"
#include "enp/services/ProcessService.h"

#include "AutomationController.h"
#include "McpBridge.h"

#include <atomic>
#include <cstring>
#include <map>
#include <regex>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#   include <csignal>
#   include <unistd.h>
#   if defined(__APPLE__) || defined(__GLIBC__)
#       include <execinfo.h>
#       define ENP_TEST_BACKTRACE 1
#   endif
#endif

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

// ---- Multiple carets (VS Code) ------------------------------------------------------------------

// Carets at the given places (the first is the primary one).
void carets(viewmodel::NotepadViewModel& vm, std::vector<TextPos> at)
{
    std::vector<model::Selection> sels;
    for (TextPos p : at)
        sels.push_back({p, p});
    vm.setSelections(sels);
}

TEST(multi_cursor_typing_is_one_undo_step)
{
    Fixture f("foo\nfoo\nfoo");
    carets(f.vm, {{0, 0}, {1, 0}, {2, 0}});
    CHECK_EQ(f.vm.cursorCount(), 3);
    f.vm.insertText("x");
    f.vm.insertText("y");
    CHECK_EQ(f.text(), std::string("xyfoo\nxyfoo\nxyfoo"));
    CHECK(f.vm.activeDocument()->caret == (TextPos{0, 2}));
    f.vm.execute(CommandId::Undo);
    CHECK_EQ(f.text(), std::string("foo\nfoo\nfoo"));
    CHECK_EQ(f.vm.cursorCount(), 3); // the carets come back with the text
    f.vm.execute(CommandId::Redo);
    CHECK_EQ(f.text(), std::string("xyfoo\nxyfoo\nxyfoo"));
    // Several carets on one line.
    Fixture g("a b c");
    carets(g.vm, {{0, 1}, {0, 3}, {0, 5}});
    g.vm.insertText("!");
    CHECK_EQ(g.text(), std::string("a! b! c!"));
    g.vm.clearExtraCursors();
    CHECK_EQ(g.vm.cursorCount(), 1);
}

TEST(multi_cursor_backspace_delete_and_newline)
{
    Fixture f("ab\ncd\nef");
    carets(f.vm, {{1, 0}, {2, 0}});
    f.vm.backspace(); // both line breaks: bottom first, the upper caret follows
    CHECK_EQ(f.text(), std::string("abcdef"));
    CHECK_EQ(f.vm.cursorCount(), 2);
    f.vm.newline();
    CHECK_EQ(f.text(), std::string("ab\ncd\nef"));
    f.vm.deleteForward();
    CHECK_EQ(f.text(), std::string("ab\nd\nf"));
    // Selections are replaced at every caret; carets meeting in one place merge.
    Fixture g("one two one");
    g.vm.setSelections({{{0, 0}, {0, 3}}, {{0, 8}, {0, 11}}});
    CHECK_EQ(g.vm.selectedText(), std::string("one\none"));
    g.vm.insertText("1");
    CHECK_EQ(g.text(), std::string("1 two 1"));
    carets(g.vm, {{0, 1}, {0, 1}});
    CHECK_EQ(g.vm.cursorCount(), 1);
}

TEST(multi_cursor_moves_and_adds_carets)
{
    Fixture f("abc\na\nabcdef");
    f.vm.setCaret({0, 3}, false);
    f.vm.addCursorVertical(+1);
    f.vm.addCursorVertical(+1);
    CHECK_EQ(f.vm.cursorCount(), 3);
    const std::vector<model::Selection> all = f.vm.activeDocument()->selections();
    CHECK(all[0].caret == (TextPos{2, 1})); // the newest caret is the primary one; column of the line above
    f.vm.moveCarets([](const model::Selection& s) { return TextPos{s.caret.line, 0}; }, false);
    f.vm.insertText("-");
    CHECK_EQ(f.text(), std::string("-abc\n-a\n-abcdef"));
    // Alt+click adds a caret, on a caret removes it.
    f.vm.setCaret({0, 0}, false);
    f.vm.addCursor({2, 2});
    CHECK_EQ(f.vm.cursorCount(), 2);
    f.vm.addCursor({0, 0});
    CHECK_EQ(f.vm.cursorCount(), 1);
    CHECK(f.vm.activeDocument()->caret == (TextPos{2, 2}));
    // A caret at the end of every selected line.
    f.vm.setSelection({0, 1}, {2, 2});
    f.vm.execute(CommandId::CursorsAtLineEnds);
    CHECK_EQ(f.vm.cursorCount(), 3);
    f.vm.insertText(";");
    CHECK_EQ(f.text(), std::string("-abc;\n-a;\n-abcdef;"));
}

TEST(add_next_occurrence_and_select_all)
{
    Fixture f("int a = a + ab;");
    f.vm.setCaret({0, 4}, false);
    f.vm.execute(CommandId::AddNextOccurrence); // the word at the caret
    CHECK_EQ(f.vm.selectedText(), std::string("a"));
    CHECK_EQ(f.vm.cursorCount(), 1);
    f.vm.execute(CommandId::AddNextOccurrence); // the next whole word "a" (not the one in "ab")
    CHECK_EQ(f.vm.cursorCount(), 2);
    f.vm.execute(CommandId::AddNextOccurrence); // none left
    CHECK_EQ(f.vm.cursorCount(), 2);
    f.vm.insertText("b");
    CHECK_EQ(f.text(), std::string("int b = b + ab;"));
    // A selection made by hand matches inside words too.
    f.vm.setSelection({0, 4}, {0, 5});
    f.vm.execute(CommandId::AddNextOccurrence);
    f.vm.execute(CommandId::AddNextOccurrence);
    CHECK_EQ(f.vm.cursorCount(), 3);
    // Every occurrence at once.
    Fixture g("x1 x x2 x");
    g.vm.setCaret({0, 3}, false);
    g.vm.execute(CommandId::SelectAllOccurrences);
    CHECK_EQ(g.vm.cursorCount(), 2);
    CHECK(g.vm.activeDocument()->caret == (TextPos{0, 4})); // still on the one it was on
    g.vm.insertText("y");
    CHECK_EQ(g.text(), std::string("x1 y x2 y"));
}

TEST(multi_cursor_line_operations)
{
    Fixture f("1\n2\n3\n4\n5");
    carets(f.vm, {{1, 0}, {3, 0}});
    f.vm.execute(CommandId::MoveLineUp);
    CHECK_EQ(f.text(), std::string("2\n1\n4\n3\n5"));
    CHECK(f.vm.activeDocument()->caret == (TextPos{0, 0}));
    f.vm.execute(CommandId::MoveLineUp); // one block is at the top: nothing moves
    CHECK_EQ(f.text(), std::string("2\n1\n4\n3\n5"));
    f.vm.execute(CommandId::MoveLineDown);
    CHECK_EQ(f.text(), std::string("1\n2\n3\n4\n5"));
    // Copy down puts the carets on the copies, copy up leaves them on the originals.
    carets(f.vm, {{0, 0}, {3, 0}});
    f.vm.execute(CommandId::DuplicateLine);
    CHECK_EQ(f.text(), std::string("1\n1\n2\n3\n4\n4\n5"));
    CHECK(f.vm.activeDocument()->caret == (TextPos{1, 0}));
    f.vm.execute(CommandId::Undo);
    CHECK_EQ(f.text(), std::string("1\n2\n3\n4\n5"));
    carets(f.vm, {{0, 0}, {3, 0}});
    f.vm.execute(CommandId::CopyLineUp);
    CHECK_EQ(f.text(), std::string("1\n1\n2\n3\n4\n4\n5"));
    CHECK(f.vm.activeDocument()->caret == (TextPos{0, 0}));
    // Adjacent carets form one block: no line is deleted twice.
    Fixture g("0\n1\n2\n3\n4\n5");
    carets(g.vm, {{1, 0}, {2, 0}, {4, 0}});
    g.vm.execute(CommandId::DeleteLine);
    CHECK_EQ(g.text(), std::string("0\n3\n5"));
    CHECK_EQ(g.vm.cursorCount(), 2);
    // Comments and indentation per block; the carets keep their place in the text.
    Fixture h("int a;\nint b;\nint c;");
    h.vm.activeDocument()->language = model::Language::Cpp;
    carets(h.vm, {{0, 4}, {2, 4}});
    h.vm.execute(CommandId::ToggleComment);
    CHECK_EQ(h.text(), std::string("// int a;\nint b;\n// int c;"));
    CHECK(h.vm.activeDocument()->caret == (TextPos{0, 7}));
    h.vm.execute(CommandId::ToggleComment);
    CHECK_EQ(h.text(), std::string("int a;\nint b;\nint c;"));
    h.vm.execute(CommandId::UpperCase);
    CHECK_EQ(h.text(), std::string("INT A;\nint b;\nINT C;"));
}

TEST(multi_cursor_paste_spreads_lines)
{
    Fixture f("a\nb\nc");
    carets(f.vm, {{0, 1}, {1, 1}, {2, 1}});
    f.vm.paste("1\n2\n3");
    CHECK_EQ(f.text(), std::string("a1\nb2\nc3"));
    f.vm.paste("xy"); // one line for three carets: all of it at each
    CHECK_EQ(f.text(), std::string("a1xy\nb2xy\nc3xy"));
    f.vm.setSelections({{{0, 0}, {0, 1}}, {{1, 0}, {1, 1}}});
    const std::string copied = f.vm.selectedText();
    CHECK_EQ(copied, std::string("a\nb"));
    f.vm.deleteSelection(); // Cut
    carets(f.vm, {{0, 2}, {1, 2}});
    f.vm.paste(copied + "\n"); // a trailing line break does not count as a line
    CHECK_EQ(f.text(), std::string("1xay\n2xby\nc3xy"));
}

// ---- Workspace files ----------------------------------------------------------------------------

TEST(workspace_files_are_parsed)
{
    CHECK(model::isWorkspaceFile("C:\\src\\App.SLN"));
    CHECK(model::isWorkspaceFile("/a/b.code-workspace"));
    CHECK(!model::isWorkspaceFile("/a/b.json"));
    CHECK_EQ(model::normalizePath("C:\\a\\b\\..\\c\\.\\d"), std::string("C:/a/c/d"));
    CHECK_EQ(model::normalizePath("/a/../../b/"), std::string("/b"));
    CHECK_EQ(model::commonFolder({"/a/b/c", "/a/b/d/e", "/a/b"}), std::string("/a/b"));
    CHECK_EQ(model::commonFolder({"/a/bc", "/a/b"}), std::string("/a"));
    CHECK_EQ(model::stripJsonComments("{\"a\": \"//no\", // yes\n \"b\": [1, 2,], /* c */}"),
             std::string("{\"a\": \"//no\", \n \"b\": [1, 2]  }")); // both trailing commas go

    const std::string workspace = R"({
        // VS Code writes comments and trailing commas
        "folders": [
            { "path": "." },
            { "name": "lib", "path": "../shared/lib" },
            { "uri": "vscode-remote://ssh-remote+box/home/me" },
        ],
        "settings": { "editor.tabSize": 2, "editor.insertSpaces": true, },
    })";
    std::string error;
    const auto w = model::parseWorkspaceFile("/home/me/proj/proj.code-workspace", workspace, &error);
    CHECK(w.has_value());
    if (w) {
        CHECK(w->kind == model::WorkspaceImport::Kind::CodeWorkspace);
        CHECK_EQ(w->name, std::string("proj"));
        CHECK_EQ(w->root, std::string("/home/me/proj"));
        CHECK_EQ(w->folders.size(), size_t(2)); // the remote folder is left out
        CHECK_EQ(w->folders[1], std::string("/home/me/shared/lib"));
        CHECK(w->tabSize && *w->tabSize == 2);
        CHECK(w->insertSpaces && *w->insertSpaces);
    }
    CHECK(!model::parseWorkspaceFile("/x/empty.code-workspace", "{\"folders\": []}", &error));
    CHECK(!error.empty());

    const std::string sln = "\xEF\xBB\xBF\r\nMicrosoft Visual Studio Solution File, Format Version 12.00\r\n"
                            "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"App\", \"App\\App.vcxproj\", \"{1}\"\r\nEndProject\r\n"
                            "Project(\"{2150E333-8FDC-42A3-9474-1A3956D46DE8}\") = \"Libs\", \"Libs\", \"{2}\"\r\nEndProject\r\n"
                            "Project(\"{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}\") = \"Core\", \"..\\core\\Core.vcxproj\", \"{3}\"\r\nEndProject\r\n";
    const auto s = model::parseWorkspaceFile("C:\\work\\app\\App.sln", sln, &error);
    CHECK(s.has_value());
    if (s) {
        CHECK(s->kind == model::WorkspaceImport::Kind::Solution);
        CHECK_EQ(s->projects.size(), size_t(2)); // the solution folder "Libs" is not a project
        CHECK_EQ(s->projects[1], std::string("C:/work/core/Core.vcxproj"));
        CHECK_EQ(s->root, std::string("C:/work")); // holds the solution and both projects
    }
    const auto x = model::parseWorkspaceFile("/w/App.slnx", "<Solution>\n  <Folder Name=\"/src/\">\n"
                                             "    <Project Path=\"src/App/App.csproj\" />\n  </Folder>\n</Solution>", &error);
    CHECK(x.has_value() && x->projects.size() == 1 && x->root == "/w");
    CHECK(!model::parseWorkspaceFile("/w/notes.sln", "hello", &error));
}

TEST(open_workspace_file_opens_its_folder)
{
    services::FileService files;
    const std::string base = scratchDirectory() + "/enp-test-workspace";
    os::fs::createDirectories(base + "/proj/src");
    os::fs::createDirectories(base + "/other");
    files.write(base + "/proj/src/a.txt", "x\n", nullptr);
    files.write(base + "/proj/proj.code-workspace",
                "{ \"folders\": [ {\"path\": \".\"}, {\"path\": \"../other\"} ], \"settings\": {\"editor.tabSize\": 3} }", nullptr);
    files.write(base + "/proj/App.sln", "Microsoft Visual Studio Solution File, Format Version 12.00\n"
                "Project(\"{X}\") = \"A\", \"src\\A.vcxproj\", \"{1}\"\nEndProject\n", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openWorkspaceFile(base + "/proj/proj.code-workspace"));
    CHECK(vm.workspaceRoot().size() > 5 && vm.workspaceRoot().substr(vm.workspaceRoot().size() - 5) == "/proj");
    CHECK(vm.recentFolders().size() >= 2);
    CHECK(vm.recentFolders().size() >= 2 && vm.recentFolders()[1].find("/other") != std::string::npos); // Ctrl+R's first entry
    CHECK_EQ(vm.indentFor(model::Language::Cpp).width, 3);
    CHECK(vm.command(CommandId::OpenRecent).canExecute());
    // Another folder forgets the workspace's indentation.
    CHECK(vm.openFolder(base + "/other"));
    CHECK(vm.indentFor(model::Language::Cpp).width != 3);
    CHECK(vm.openWorkspaceFile(base + "/proj/App.sln"));
    CHECK(vm.workspaceRoot().size() > 5 && vm.workspaceRoot().substr(vm.workspaceRoot().size() - 5) == "/proj");
    CHECK(!vm.openWorkspaceFile(base + "/proj/src/a.txt"));
    // The file dialog's workspace mode lists only workspace files.
    vm.fileDialog().show(viewmodel::FileDialogMode::OpenWorkspace, base + "/proj");
    bool onlyWorkspaces = true;
    for (const auto& e : vm.fileDialog().visibleEntries())
        onlyWorkspaces = onlyWorkspaces && (e.isDirectory || viewmodel::NotepadViewModel::isWorkspaceFile(e.name));
    CHECK(onlyWorkspaces);
    CHECK_EQ(vm.fileDialog().visibleEntries().size(), size_t(3)); // src, App.sln, proj.code-workspace
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

TEST(sha256_and_macros)
{
    // FIPS 180-4 test vectors.
    CHECK_EQ(model::sha256Hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(model::sha256Hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(model::sha256Hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
    CHECK_EQ(model::sha256Hex(std::string(1000000, 'a')), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));

    services::FileService files;
    const std::string path = scratchDirectory() + "/enp-test-settings/macro.json";
    std::remove(path.c_str());
    {
        viewmodel::NotepadViewModel vm(nullptr, &files);
        vm.setSettingsPath(path);
        vm.newDocument();
        model::Document& doc = *vm.activeDocument();
        vm.insertText("alpha\nbeta\ngamma\ndelta");
        vm.setCaret({0, 0}, false);
        // Record: prefix the line with "- ", duplicate it (the caret goes to the copy), then go to
        // the start of the next line.
        vm.execute(CommandId::StartRecording);
        vm.insertText("-");
        vm.insertText(" ");
        vm.execute(CommandId::DuplicateLine);
        vm.setCaret({doc.caret.line + 1, 0}, false);
        vm.execute(CommandId::StopRecording);
        CHECK_EQ(vm.macro().size(), size_t(3)); // text (coalesced), command, move
        CHECK_EQ(doc.buffer.text(), std::string("- alpha\n- alpha\nbeta\ngamma\ndelta"));
        // Play at line 2 ("beta"), twice: one undo step for both.
        vm.setCaret({2, 0}, false);
        vm.playMacro(2);
        CHECK_EQ(doc.buffer.text(), std::string("- alpha\n- alpha\n- beta\n- beta\n- gamma\n- gamma\ndelta"));
        vm.execute(CommandId::Undo);
        CHECK_EQ(doc.buffer.text(), std::string("- alpha\n- alpha\nbeta\ngamma\ndelta"));
        // SHA-256 of the selection.
        vm.setSelection({2, 0}, {2, 4});
        vm.execute(CommandId::HashSha256);
        CHECK_EQ(vm.hashResult.get(), model::sha256Hex("beta"));
    }
    // The macro is remembered.
    viewmodel::NotepadViewModel vm(nullptr, &files);
    vm.setSettingsPath(path);
    CHECK_EQ(vm.macro().size(), size_t(3));
    CHECK(vm.macro()[1].kind == viewmodel::NotepadViewModel::MacroStep::Kind::Command &&
          vm.macro()[1].command == CommandId::DuplicateLine);
    std::remove(path.c_str());
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

TEST(markdown_inline_styles_and_links)
{
    namespace md = model::markdown;
    const md::Spans s = md::parseInline("Use **bold**, *it*, `a*b`, ~~old~~ and [docs](https://x.io \"t\") or my_var_name");
    CHECK_EQ(md::plainText(s), std::string("Use bold, it, a*b, old and docs or my_var_name"));
    auto styleOf = [&s](const std::string& text) {
        for (const md::Span& span : s)
            if (span.text == text)
                return span.style;
        return 999u;
    };
    CHECK_EQ(styleOf("bold"), unsigned(md::Bold));
    CHECK_EQ(styleOf("it"), unsigned(md::Italic));
    CHECK_EQ(styleOf("a*b"), unsigned(md::Code));
    CHECK_EQ(styleOf("old"), unsigned(md::Strike));
    CHECK_EQ(styleOf("docs"), unsigned(md::Link));
    for (const md::Span& span : s)
        if (span.text == "docs")
            CHECK_EQ(span.url, std::string("https://x.io"));
    // Unclosed markers stay text; bare URLs are links without trailing punctuation.
    CHECK_EQ(md::plainText(md::parseInline("2 * 3 = 6 and **open")), std::string("2 * 3 = 6 and **open"));
    const md::Spans url = md::parseInline("See https://example.com/a.");
    CHECK(url.size() == 3 && url[1].style == md::Link && url[1].url == "https://example.com/a" && url[2].text == ".");
}

TEST(markdown_entities_images_and_math)
{
    namespace md = model::markdown;
    // HTML character references become their characters; a lone '&' stays.
    CHECK_EQ(md::decodeEntities("Fish &amp; Chips &copy; 2026 &rarr; &#169; &#xA9; A & B &bogus; &lt;b&gt;"),
             std::string("Fish & Chips © 2026 → © © A & B &bogus; <b>"));
    const md::Spans e = md::parseInline("caf&eacute; &mdash; 5&nbsp;km");
    CHECK_EQ(md::plainText(e), std::string("café — 5\xC2\xA0km"));
    // Images: ![alt](src) and <img>, with the alt text and the source.
    const md::Spans img = md::parseInline("See ![a cat](pics/cat.png) and <img src=\"logo.svg\" alt=\"Logo\"> here");
    CHECK_EQ(img.size(), size_t(5));
    if (img.size() == 5) {
        CHECK(img[1].style == md::Image && img[1].text == "a cat" && img[1].url == "pics/cat.png");
        CHECK(img[3].style == md::Image && img[3].text == "Logo" && img[3].url == "logo.svg");
        CHECK_EQ(img[4].text, std::string(" here"));
    }
    // Inline math: $...$ and \(...\); prices are not formulas.
    const md::Spans m = md::parseInline("Euler: $e^{i\\pi} + 1 = 0$ costs $5 and $6, also \\(\\frac{a}{b}\\).");
    int mathSpans = 0;
    for (const md::Span& span : m)
        if (span.style & md::Math) {
            ++mathSpans;
            CHECK(span.text == "e^{i\\pi} + 1 = 0" || span.text == "\\frac{a}{b}");
        }
    CHECK_EQ(mathSpans, 2);
    CHECK(md::plainText(m).find("$5 and $6") != std::string::npos);
    // <br> breaks the line; other tags stay text.
    CHECK_EQ(md::plainText(md::parseInline("a<br>b <span>c</span>")), std::string("a\nb <span>c</span>"));
    // Display math blocks: $$ on one line or several, and \[ \].
    const std::vector<md::Block> b = md::parse("Text\n\n$$\n\\sum_{i=0}^{n} i = \\frac{n(n+1)}{2}\n$$\n\n$$ x^2 $$\n\\[ y \\]\nafter");
    CHECK_EQ(b.size(), size_t(5));
    if (b.size() == 5) {
        CHECK(b[1].kind == md::BlockKind::Math && b[1].code == "\\sum_{i=0}^{n} i = \\frac{n(n+1)}{2}");
        CHECK(b[2].kind == md::BlockKind::Math && b[2].code == "x^2");
        CHECK(b[3].kind == md::BlockKind::Math && b[3].code == "y");
        CHECK(b[4].kind == md::BlockKind::Paragraph && md::plainText(b[4].spans) == "after");
    }
}

TEST(latex_formulas_parse_into_a_layout_tree)
{
    namespace mt = model::math;
    using K = mt::Node::Kind;
    const mt::Node f = mt::parse("\\frac{a+b}{2}");
    CHECK(f.kind == K::Frac && f.children.size() == 2);
    CHECK_EQ(mt::plainText(f), std::string("(a+b)/(2)"));
    const mt::Node s = mt::parse("x^{2}_{i}");
    CHECK(s.kind == K::Scripts && s.children.size() == 3 && s.children[0].text == "x");
    CHECK_EQ(mt::plainText(s), std::string("x_{i}^{2}"));
    const mt::Node sum = mt::parse("\\sum_{i=0}^{n} i");
    CHECK(sum.kind == K::Row && sum.children.size() == 2 && sum.children[0].kind == K::Scripts);
    CHECK(sum.children[0].children[0].kind == K::Op && sum.children[0].children[0].text == "∑" && sum.children[0].children[0].limitsAbove);
    const mt::Node r = mt::parse("\\sqrt[3]{x} \\alpha \\le \\infty \\left( \\frac{1}{2} \\right) \\text{if } \\hat{y}");
    CHECK_EQ(mt::plainText(r), std::string("√(x)α≤∞((1)/(2))if y^"));
    CHECK(r.children.size() >= 6 && r.children[0].kind == K::Sqrt && r.children[0].children[1].text == "3");
    const mt::Node fenced = mt::parse("\\left[ a \\right]");
    CHECK(fenced.kind == K::Fenced && fenced.text == "[" && fenced.text2 == "]");
    // Unknown commands and unbalanced braces do not break it.
    CHECK_EQ(mt::plainText(mt::parse("\\unknown{x} + {y")), std::string("unknownx+y"));
    CHECK_EQ(mt::plainText(mt::parse("\\int_0^1 f(x)\\,dx")), std::string("∫_{0}^{1}f(x) dx"));
}

TEST(file_types_are_detected_before_loading_and_images_decode)
{
    using model::FileKind;
    const std::string png("\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16);
    CHECK(model::detectFileType("a.png", png).kind == FileKind::Image && model::detectFileType("a.png", png).format == "PNG");
    CHECK(model::detectFileType("noext", png).kind == FileKind::Image); // the content decides, not the name
    CHECK(model::detectFileType("x.jpg", std::string("\xFF\xD8\xFF\xE0", 4)).format == "JPEG");
    CHECK(model::detectFileType("x.webp", std::string("RIFF\0\0\0\0WEBPVP8 ", 16)).format == "WebP");
    CHECK(model::detectFileType("x.wav", std::string("RIFF\0\0\0\0WAVEfmt ", 16)).kind == FileKind::Binary);
    CHECK(model::detectFileType("prog", std::string("\x7F" "ELF\x02\x01\x01\0", 8)).format == "ELF executable");
    CHECK(model::detectFileType("a.zip", std::string("PK\x03\x04", 4)).kind == FileKind::Binary);
    CHECK(model::detectFileType("data.bin", std::string("abc\0def", 7)).kind == FileKind::Binary);  // a NUL byte
    CHECK(model::detectFileType("notes.txt", "plain text\nwith lines\n").kind == FileKind::Text);
    CHECK(model::detectFileType("utf8.md", "caf\xC3\xA9 \xE2\x86\x92 \xCE\xB1\n").kind == FileKind::Text);    // UTF-8 is text
    CHECK(model::detectFileType("pic.svg", "<?xml version=\"1.0\"?><svg></svg>").kind == FileKind::Text); // SVG: XML to edit
    CHECK(model::detectFileType("x.ppm", "P6\n2 2\n255\n").kind == FileKind::Image);
    CHECK(model::detectFileType("empty.txt", "").kind == FileKind::Text);

    // Decoders: PNM (stb), QOI and ICO (our own), and a refusal with a reason.
    services::DecodedImage img;
    std::string note;
    const std::string ppm = std::string("P6\n2 1\n255\n") + std::string("\xFF\x00\x00\x00\x00\xFF", 6);
    CHECK(services::decodeImage(ppm, img, note));
    CHECK(img.width == 2 && img.height == 1 && img.rgba.size() == 8 && img.rgba[0] == 255 && img.rgba[6] == 255 && img.rgba[3] == 255);
    // QOI, 2x1: an RGBA pixel (red), then a run of 1, then the end marker.
    std::string qoi("qoif\0\0\0\x02\0\0\0\x01\x04\x00", 14);
    qoi += std::string("\xFF\xFF\x00\x00\x80", 5); // QOI_OP_RGBA red, alpha 128
    qoi += std::string("\xC0", 1);                 // QOI_OP_RUN, 1 more
    qoi += std::string("\0\0\0\0\0\0\0\x01", 8);
    CHECK(services::decodeImage(qoi, img, note));
    CHECK(img.width == 2 && img.height == 1 && img.format == "QOI" && img.rgba[0] == 255 && img.rgba[3] == 128 && img.rgba[4] == 255 && img.rgba[7] == 128);
    // ICO with one PNG-less 1x1 32-bit DIB entry.
    std::string dib(40, '\0');
    dib[0] = 40;                  // header size
    dib[4] = 1;                   // width
    dib[8] = 2;                   // height (XOR + AND)
    dib[12] = 1;                  // planes
    dib[14] = 32;                 // bits per pixel
    dib += std::string("\x00\xFF\x00\xFF", 4); // BGRA green
    dib += std::string(4, '\0');               // AND mask row
    std::string ico("\0\0\x01\0\x01\0", 6);
    std::string entry(16, '\0');
    entry[0] = 1;
    entry[1] = 1;
    const unsigned length = unsigned(dib.size()), offset = 22;
    std::memcpy(&entry[8], &length, 4);
    std::memcpy(&entry[12], &offset, 4);
    ico += entry + dib;
    CHECK(services::decodeImage(ico, img, note));
    CHECK(img.width == 1 && img.height == 1 && img.rgba[1] == 255 && img.rgba[0] == 0);
    CHECK(!services::decodeImage(std::string("RIFF\0\0\0\0WEBPVP8 ", 16), img, note) && note.find("WebP") != std::string::npos);
    CHECK(!services::decodeImage("hello", img, note) && !note.empty());

    // Opening: an image and a binary file get a document without text; saving it refuses
    // (it would write the empty buffer over the file); "open as text" loads the bytes.
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-file-kinds";
    os::fs::createDirectories(root);
    files.write(root + "/pic.ppm", ppm, nullptr);
    files.write(root + "/blob.bin", std::string("head\0\x01\x02tail", 11), nullptr);
    files.write(root + "/notes.txt", "hello\n", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    vm.openFile("pic.ppm");
    CHECK(vm.activeDocument() && vm.activeDocument()->kind == FileKind::Image && vm.activeDocument()->buffer.text().empty());
    vm.openFile("blob.bin");
    model::Document* blob = vm.activeDocument();
    CHECK(blob && blob->kind == FileKind::Binary && blob->buffer.text().empty() && blob->format == "bin file");
    vm.execute(viewmodel::CommandId::Save); // the active document: refused
    CHECK_EQ(files.read(root + "/blob.bin")->size(), size_t(11)); // untouched
    CHECK(vm.openAsText(vm.activeIndex.get()));
    CHECK(blob->kind == FileKind::Text && blob->buffer.text().size() == 11);
    vm.openFile("notes.txt");
    CHECK(vm.activeDocument() && vm.activeDocument()->kind == FileKind::Text && vm.activeDocument()->buffer.text() == "hello\n");
    // The workspace reader never hands out an image document's empty buffer for its file.
    CHECK_EQ(vm.readWorkspaceFile("pic.ppm").size(), ppm.size());
}

TEST(emoji_sequences_follow_uts51)
{
    namespace e = model::emoji;
    auto key = [](const std::string& text) {
        std::string k;
        const size_t n = e::match(text, 0, &k);
        return n == text.size() ? k : (n == 0 ? std::string("-") : "partial:" + k);
    };
    // The screenshot's forms: a text-default arrow, its emoji form with FE0F.
    CHECK_EQ(key("⬇"), std::string("-"));            // ⬇ alone stays text
    CHECK_EQ(key("⬇️"), std::string("2b07"));   // ⬇️
    CHECK_EQ(key("⬇︎"), std::string("-"));      // asked to be text
    CHECK_EQ(e::invisible("︎", 0), size_t(3));
    CHECK_EQ(key("\U0001F600"), std::string("1f600"));    // 😀 (above U+FFFF)
    CHECK_EQ(key("©"), std::string("-"));            // © text
    CHECK_EQ(key("©️"), std::string("00a9"));   // ©️
    CHECK_EQ(key("#️⃣"), std::string("0023_20e3")); // keycaps, with or without FE0F
    CHECK_EQ(key("7⃣"), std::string("0037_20e3"));
    CHECK_EQ(key("7"), std::string("-"));
    CHECK_EQ(key("\U0001F1F2\U0001F1FE"), std::string("1f1f2_1f1fe")); // flag MY
    CHECK_EQ(key("\U0001F44D\U0001F3FD"), std::string("1f44d_1f3fd")); // skin tone
    CHECK_EQ(key("\U0001F469‍\U0001F52C"), std::string("1f469_200d_1f52c")); // ZWJ
    CHECK_EQ(key("❤️‍\U0001F525"), std::string("2764_200d_1f525")); // ❤️‍🔥
    CHECK_EQ(key("\U0001F3F4\U000E0067\U000E0062\U000E0065\U000E006E\U000E0067\U000E007F"),
             std::string("1f3f4_e0067_e0062_e0065_e006e_e0067_e007f")); // England
    CHECK_EQ(key("\U0001F469\U0001F3FD‍\U0001F52C"), std::string("1f469_1f3fd_200d_1f52c"));
    std::string k;
    CHECK_EQ(e::match("\U0001F600‍", 0, &k), size_t(4)); // a joiner with nothing after it is not part of it
    CHECK(e::components("1f469_200d_1f52c") == std::vector<std::string>({"1f469", "1f52c"}));
    CHECK_EQ(e::withoutModifiers("1f44d_1f3fd"), std::string("1f44d"));
    CHECK_EQ(std::string(e::unicodeVersion()), std::string("18.0"));

    // In Markdown: raw, hex and decimal entities all become the same emoji; the plain arrow stays text.
    namespace md = model::markdown;
    const md::Spans s = md::parseInline("Raw ⬇️, hex &#x2B07;&#xFE0F;, dec &#11015;&#65039;, plain ⬇ and \U0001F44D\U0001F3FD.");
    int emojis = 0;
    for (const md::Span& span : s)
        if (span.style & md::Emoji) {
            ++emojis;
            CHECK(span.emoji == "2b07" || span.emoji == "1f44d_1f3fd");
        }
    CHECK_EQ(emojis, 4);
    for (const md::Span& span : s) // no selector left in text drawn as text (it would show as a box)
        if (!(span.style & md::Emoji))
            CHECK(span.text.find("\xEF\xB8\x8F") == std::string::npos);
    CHECK(md::plainText(s).find("plain ⬇") != std::string::npos);
    // Emoji keep the style around them (a link's target, bold).
    const md::Spans link = md::parseInline("[go \U0001F680](https://x.io) **\U0001F600**");
    bool linked = false, bold = false;
    for (const md::Span& span : link) {
        linked |= (span.style & md::Emoji) && (span.style & md::Link) && span.url == "https://x.io" && span.emoji == "1f680";
        bold |= (span.style & md::Emoji) && (span.style & md::Bold);
    }
    CHECK(linked && bold);
}

TEST(emoji_pack_reads_only_what_it_needs)
{
    // A small stored zip, built here: two pictures and a file that is not one.
    auto le16 = [](std::string& b, unsigned v) { b += char(v & 0xFF); b += char((v >> 8) & 0xFF); };
    auto le32 = [&](std::string& b, unsigned v) { le16(b, v & 0xFFFF); le16(b, v >> 16); };
    struct File { std::string name, data; };
    const std::vector<File> files = {{"2b07.png", "PNGDATA-ARROW"}, {"1f600.png", "PNGDATA-GRIN"}, {"notes.txt", "x"}};
    std::string zip, cd;
    for (const File& f : files) {
        const unsigned offset = unsigned(zip.size());
        le32(zip, 0x04034b50); le16(zip, 10); le16(zip, 0); le16(zip, 0); le16(zip, 0); le16(zip, 0);
        le32(zip, 0); le32(zip, unsigned(f.data.size())); le32(zip, unsigned(f.data.size()));
        le16(zip, unsigned(f.name.size())); le16(zip, 3); zip += f.name; zip += "abc"; zip += f.data; // with an extra field
        le32(cd, 0x02014b50); le16(cd, 20); le16(cd, 10); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0);
        le32(cd, 0); le32(cd, unsigned(f.data.size())); le32(cd, unsigned(f.data.size()));
        le16(cd, unsigned(f.name.size())); le16(cd, 0); le16(cd, 0); le16(cd, 0); le16(cd, 0); le32(cd, 0); le32(cd, offset);
        cd += f.name;
    }
    const unsigned cdOffset = unsigned(zip.size());
    zip += cd;
    le32(zip, 0x06054b50); le16(zip, 0); le16(zip, 0); le16(zip, 3); le16(zip, 3); le32(zip, unsigned(cd.size())); le32(zip, cdOffset); le16(zip, 0);
    services::FileService files_;
    const std::string path = scratchDirectory() + "/enp-test-emoji-pack.zip";
    files_.write(path, zip, nullptr);
    services::EmojiPack pack(path, true);
    CHECK(!pack.loaded());
    CHECK(pack.contains("2b07") && pack.contains("1f600") && !pack.contains("notes") && !pack.contains("1f601"));
    CHECK(pack.loaded() && pack.size() == 2);
    CHECK(pack.read("2b07").value_or("") == "PNGDATA-ARROW");
    pack.unload();
    CHECK(!pack.loaded() && pack.size() == 0);
    CHECK(pack.read("1f600").value_or("") == "PNGDATA-GRIN"); // the index comes back on demand
    std::remove(path.c_str());
    services::EmojiPack missing(scratchDirectory() + "/no-such-pack.zip", true);
    CHECK(!missing.contains("2b07") && !missing.read("2b07"));

    // The shipped pack: every kind of sequence has a picture, and pictures decode at 72 px.
    services::EmojiPack real(std::string(ENP_SOURCE_DIR) + "/resources/emoji/noto-emoji-72.zip", true);
    CHECK(real.contains("2b07") && real.contains("1f469_200d_1f52c") && real.contains("0023_20e3") && real.contains("1f1f2_1f1fe") &&
          real.contains("1f44d_1f3fd") && real.contains("1f3f4_e0067_e0062_e0065_e006e_e0067_e007f"));
    CHECK(real.size() >= 4000);
    services::DecodedImage img;
    std::string note;
    CHECK(services::decodeImage(real.read("1f600").value_or(""), img, note) && img.width == 72 && img.height == 72);
}

TEST(csv_tables_follow_rfc4180)
{
    namespace csv = model::csv;
    const std::string text = "name,age,note\r\n\"Smith, J\",42,\"said \"\"hi\"\"\"\nLee,7,\"two\nlines\"\nshort\n";
    CHECK_EQ(csv::detectDelimiter(text), ',');
    const csv::Table t = csv::parse(text, ',');
    CHECK_EQ(t.rowCount(), size_t(4));
    CHECK_EQ(t.columns, 3);
    CHECK(t.cell(0, 0) == "name" && t.cell(1, 0) == "Smith, J" && t.cell(1, 2) == "said \"hi\"");
    CHECK(t.cell(2, 2) == "two\nlines");             // a line break inside quotes
    CHECK(t.cellCount(3) == 1 && t.cell(3, 1).empty()); // a short row
    CHECK_EQ(csv::detectDelimiter("a;b;c\n1;2;3\n4;5;6\n"), ';');
    CHECK_EQ(csv::detectDelimiter("a\tb\n1\t2\n"), '\t');
    CHECK_EQ(csv::detectDelimiter("a,b\n", "data.tsv"), '\t');
    CHECK_EQ(csv::parse("a,b,\n", ',').cellCount(0), 3); // a trailing delimiter: an empty last cell
    CHECK_EQ(csv::parse("", ',').rowCount(), size_t(0));
}

TEST(json_and_yaml_read_into_one_tree)
{
    namespace d = model::data;
    const d::Tree j = d::parseJson("{\n  \"name\": \"Ed\\u00e9n \\ud83d\\ude00\",\n  \"n\": -1.5e3,\n  \"ok\": true,\n  \"none\": null,\n"
                                   "  \"list\": [1, {\"a\": [ ]}, \"x\"]\n}\n");
    CHECK(j.ok());
    const d::Node& root = j.nodes[0];
    CHECK(root.kind == d::Kind::Object && root.childCount == 5);
    const d::Node& name = j.nodes[root.firstChild];
    CHECK(j.key(name) == "name" && j.value(name) == "Ed\xC3\xA9n \xF0\x9F\x98\x80" && name.line == 1);
    std::uint32_t list = root.firstChild;
    while (list != d::kNone && j.key(j.nodes[list]) != "list")
        list = j.nodes[list].nextSibling;
    CHECK(list != d::kNone && j.nodes[list].kind == d::Kind::Array && j.nodes[list].childCount == 3 && j.nodes[list].line == 5);
    const std::uint32_t second = j.nodes[j.nodes[list].firstChild].nextSibling;
    CHECK_EQ(j.path(j.nodes[second].firstChild), std::string("$.list[1].a"));
    const d::Tree bad = d::parseJson("{\n  \"a\": 1\n  \"b\": 2\n}");
    CHECK(!bad.ok() && bad.errorLine == 2 && bad.error.find("','") != std::string::npos);

    const d::Tree y = d::parseYaml("# config\nname: demo   # trailing comment\nversion: 2\nempty:\nquoted: \"a: b # not a comment\"\n"
                                   "servers:\n  - host: a.example\n    port: 80\n  - host: b.example\n    port: 8080\n"
                                   "tags: [one, two, \"three, four\"]\nmap: {x: 1, y: [2, 3]}\nscript: |\n  echo hi\n  # kept\n"
                                   "folded: >\n  one\n  two\nplain: a long\n  line\nlist:\n- p\n- q\n");
    CHECK(y.ok());
    const d::Node& yr = y.nodes[0];
    CHECK(yr.kind == d::Kind::Object);
    auto child = [&](std::uint32_t parent, std::string_view key) {
        for (std::uint32_t c = y.nodes[parent].firstChild; c != d::kNone; c = y.nodes[c].nextSibling)
            if (y.key(y.nodes[c]) == key)
                return c;
        return d::kNone;
    };
    CHECK(y.value(y.nodes[child(0, "name")]) == "demo");
    CHECK(y.nodes[child(0, "version")].kind == d::Kind::Number);
    CHECK(y.nodes[child(0, "empty")].kind == d::Kind::Null);
    CHECK(y.value(y.nodes[child(0, "quoted")]) == "a: b # not a comment");
    const std::uint32_t servers = child(0, "servers");
    CHECK(servers != d::kNone && y.nodes[servers].kind == d::Kind::Array && y.nodes[servers].childCount == 2);
    const std::uint32_t s2 = y.nodes[y.nodes[servers].firstChild].nextSibling;
    CHECK(y.nodes[s2].kind == d::Kind::Object && y.value(y.nodes[child(s2, "port")]) == "8080" && y.nodes[child(s2, "port")].line == 9);
    CHECK_EQ(y.path(child(s2, "host")), std::string("$.servers[1].host"));
    const std::uint32_t tags = child(0, "tags");
    CHECK(y.nodes[tags].childCount == 3 && y.value(y.nodes[y.nodes[y.nodes[y.nodes[tags].firstChild].nextSibling].nextSibling]) == "three, four");
    const std::uint32_t map = child(0, "map");
    CHECK(y.nodes[map].kind == d::Kind::Object && y.nodes[child(map, "y")].childCount == 2);
    CHECK(y.value(y.nodes[child(0, "script")]) == "echo hi\n# kept");
    CHECK(y.value(y.nodes[child(0, "folded")]) == "one two");
    CHECK(y.value(y.nodes[child(0, "plain")]) == "a long line");
    CHECK(y.nodes[child(0, "list")].childCount == 2); // "key:\n- a" at the same indent
    const d::Tree docs = d::parseYaml("a: 1\n---\nb: 2\n");
    CHECK(docs.ok() && docs.nodes[0].kind == d::Kind::Array && docs.nodes[0].childCount == 2);
    CHECK(!d::parseYaml("a: 1\n  b: 2\n").ok());
}

TEST(data_files_switch_between_text_and_their_views)
{
    using model::DataView;
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-data-views";
    os::fs::createDirectories(root);
    files.write(root + "/t.csv", "a,b\n1,2\n", nullptr);
    files.write(root + "/c.json", "{\"a\": 1}", nullptr);
    files.write(root + "/c.yaml", "a: 1\n", nullptr);
    files.write(root + "/n.txt", "x", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    vm.openFile("t.csv");
    CHECK(vm.activeDocument()->rendered); // a table first, like Markdown
    vm.execute(CommandId::ToggleMarkdownView);
    CHECK(!vm.activeDocument()->rendered);
    vm.openFile("c.json");
    CHECK(vm.activeDocument()->dataView == DataView::Text); // configs open as text
    vm.execute(CommandId::ToggleMarkdownView);
    CHECK(vm.activeDocument()->dataView == DataView::Tree);
    vm.execute(CommandId::ToggleMarkdownView);
    CHECK(vm.activeDocument()->dataView == DataView::Accordion);
    CHECK(vm.isChecked(CommandId::ToggleMarkdownView));
    vm.execute(CommandId::ToggleMarkdownView);
    CHECK(vm.activeDocument()->dataView == DataView::Text);
    vm.openFile("c.yaml");
    vm.setDataView(DataView::Accordion);
    CHECK(vm.activeDocument()->dataView == DataView::Accordion);
    vm.openFile("n.txt");
    CHECK(!vm.command(CommandId::ToggleMarkdownView).canExecute());
    vm.setDataView(DataView::Tree); // not a data file: nothing happens
    CHECK(vm.activeDocument()->dataView == DataView::Text);
}

TEST(markdown_blocks)
{
    namespace md = model::markdown;
    const std::string text = "# Title\n"
                             "Some *text*\ncontinued.\n"
                             "\n"
                             "- one\n"
                             "  - nested\n"
                             "- [x] done\n"
                             "3. third\n"
                             "\n"
                             "> quoted\n"
                             "\n"
                             "```cpp\nint main() {}\n```\n"
                             "---\n"
                             "| A | B |\n|:--|--:|\n| 1 | `x|y` |\n"
                             "Sub\n===\n"
                             "```\nstill streaming";
    const std::vector<md::Block> b = md::parse(text);
    CHECK_EQ(int(b.size()), 12);
    if (b.size() != 12)
        return;
    CHECK(b[0].kind == md::BlockKind::Heading && b[0].level == 1 && md::plainText(b[0].spans) == "Title");
    CHECK(b[1].kind == md::BlockKind::Paragraph && md::plainText(b[1].spans) == "Some text continued." && b[1].line == 1);
    CHECK(b[2].kind == md::BlockKind::Bullet && b[2].level == 0);
    CHECK(b[3].kind == md::BlockKind::Bullet && b[3].level == 1 && md::plainText(b[3].spans) == "nested");
    CHECK(b[4].kind == md::BlockKind::Task && b[4].checked && md::plainText(b[4].spans) == "done");
    CHECK(b[5].kind == md::BlockKind::Numbered && b[5].marker == "3.");
    CHECK(b[6].kind == md::BlockKind::Paragraph && b[6].quote == 1 && md::plainText(b[6].spans) == "quoted");
    CHECK(b[7].kind == md::BlockKind::Code && b[7].language == "cpp" && b[7].code == "int main() {}");
    CHECK(b[8].kind == md::BlockKind::Rule);
    CHECK(b[9].kind == md::BlockKind::Table && b[9].rows.size() == 2 && b[9].align == std::vector<int>({-1, 1}));
    CHECK(b[9].rows.size() == 2 && md::plainText(b[9].rows[1][1]) == "x|y");
    CHECK(b[10].kind == md::BlockKind::Heading && b[10].level == 1 && md::plainText(b[10].spans) == "Sub");
    CHECK(b[11].kind == md::BlockKind::Code && b[11].code == "still streaming"); // unclosed fence
}

TEST(markdown_files_open_rendered_and_toggle_to_text)
{
    viewmodel::NotepadViewModel vm;
    vm.openFile("docs/README.md");
    model::Document* doc = vm.activeDocument();
    CHECK(doc && doc->language == model::Language::Markdown);
    if (!doc)
        return;
    CHECK(doc->rendered);
    CHECK(vm.command(CommandId::ToggleMarkdownView).canExecute() && vm.isChecked(CommandId::ToggleMarkdownView));
    vm.execute(CommandId::ToggleMarkdownView);
    CHECK(!doc->rendered && !vm.isChecked(CommandId::ToggleMarkdownView));
    vm.execute(CommandId::NewFile);
    CHECK(!vm.command(CommandId::ToggleMarkdownView).canExecute()); // plain text
}

// Claude Code that streams: the test plays its output lines.
struct FakeStreamingAgent : FakeAgentCli {
    interfaces::AgentStreamRequest request;
    std::function<void(const std::string&)> onLine;
    std::function<void(bool, const std::string&)> finished;
    bool cancelled = false;
    bool canStream() const override { return true; }
    void cancel() override { cancelled = true; }
    void stream(const interfaces::AgentStreamRequest& r, std::function<void(const std::string&)> line,
                std::function<void(bool, const std::string&)> done) override
    {
        request = r;
        onLine = std::move(line);
        finished = std::move(done);
    }
};

TEST(agent_streams_a_claude_code_conversation)
{
    FakeStreamingAgent cli;
    viewmodel::AgentViewModel agent;
    agent.goLive(&cli, "/work");
    agent.submitPrompt("Show me the terminal button", "src/a.cpp:3");
    CHECK(agent.busy.get());
    CHECK(cli.request.sessionId.empty());
    CHECK(cli.request.appControl);
    CHECK(cli.request.prompt.find("src/a.cpp:3") != std::string::npos);
    CHECK(cli.request.prompt.find("Show me the terminal button") != std::string::npos);

    cli.onLine(R"({"type":"system","subtype":"init","session_id":"s-1","mcp_servers":[{"name":"easynotepad","status":"connected"}]})");
    cli.onLine(R"({"type":"stream_event","event":{"type":"message_start"},"session_id":"s-1"})");
    cli.onLine(R"({"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"Here "}}})");
    cli.onLine(R"({"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"it is."}}})");
    CHECK(agent.messages().back().role == model::ChatRole::Agent);
    CHECK_EQ(agent.messages().back().text, std::string("Here it is."));
    // The whole message repeats what was streamed: not shown twice.
    cli.onLine(R"({"type":"assistant","message":{"content":[{"type":"text","text":"Here it is."}]}})");
    cli.onLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":"mcp__easynotepad__point","input":{"id":"command:ToggleTerminal","message":"This one"}}]}})");
    CHECK_EQ(agent.messages().back().text, std::string("point command:ToggleTerminal"));
    CHECK_EQ(agent.activity(), std::string("point command:ToggleTerminal"));
    cli.onLine(R"({"type":"user","message":{"content":[{"type":"tool_result","is_error":true,"content":[{"type":"text","text":"no such element"}]}]}})");
    CHECK(agent.messages().back().text.find("no such element") != std::string::npos);
    cli.onLine(R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":"Edit","input":{"file_path":"\\work\\src\\a.cpp"}}]}})");
    CHECK_EQ(agent.messages().back().text, std::string("Edit src/a.cpp"));
    cli.onLine("not json at all");
    cli.onLine(R"({"type":"stream_event","event":{"type":"message_start"}})");
    cli.onLine(R"({"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"text_delta","text":"Done."}}})");
    cli.onLine(R"({"type":"result","subtype":"success","is_error":false,"result":"Done.","session_id":"s-1","num_turns":2,"total_cost_usd":0.01})");
    cli.finished(true, "");
    CHECK(!agent.busy.get());
    CHECK_EQ(agent.messages().back().text, std::string("Done."));
    int agentMessages = 0;
    for (const model::ChatMessage& m : agent.messages())
        agentMessages += m.role == model::ChatRole::Agent;
    CHECK_EQ(agentMessages, 2);
    CHECK_EQ(agent.sessionId(), std::string("s-1"));

    // The next prompt continues the conversation; Stop ends the program; New Chat starts over.
    agent.submitPrompt("And now?");
    CHECK_EQ(cli.request.sessionId, std::string("s-1"));
    agent.stop();
    CHECK(cli.cancelled && !agent.busy.get());
    cli.finished(false, "stopped"); // arrives after Stop: ignored
    CHECK_EQ(agent.messages().back().text, std::string("Stopped by user."));
    agent.newChat();
    agent.submitPrompt("Fresh start");
    CHECK(cli.request.sessionId.empty());
    // A run that ends without a result says why.
    cli.finished(false, "the agent exited with code 1: not logged in");
    CHECK(!agent.busy.get());
    CHECK(agent.messages().back().text.find("not logged in") != std::string::npos);
}

struct FakeProcesses : interfaces::IProcessService {
    std::map<std::string, std::string> settings;
    std::string lastCommand, lastDirectory, output;
    void run(const std::string& commandLine, const std::string& workingDirectory,
             std::function<void(const interfaces::ProcessResult&)> done) override
    {
        lastCommand = commandLine;
        lastDirectory = workingDirectory;
        done({true, 1, output, {}, {}});
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

TEST(mcp_screenshot_comes_back_as_an_image)
{
    app::McpSession session(
        [](const std::string& method, const model::Json&, std::string*) -> std::optional<model::Json> {
            return model::Json::object().set("width", 2).set("height", 1).set("bytes", 4.0).set("png_base64", "iVBORw==");
        },
        "9.9");
    auto shot = session.handle(*model::Json::parse(
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"screenshot","arguments":{}}})"));
    const model::Json* content = shot ? shot->find("result")->find("content") : nullptr;
    CHECK(content && content->items().size() == 2);
    CHECK(content && content->items()[0].getString("type") == "image" && content->items()[0].getString("mimeType") == "image/png" &&
          content->items()[0].getString("data") == "iVBORw==");
    CHECK(content && content->items()[1].getString("text").find("\"width\":2") != std::string::npos);
}

TEST(png_encoding_writes_a_valid_file)
{
    const unsigned char pixels[] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255}; // 2 x 2
    const std::string png = services::encodePng(pixels, 2, 2);
    CHECK(png.size() > 8 && png.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0);
    services::DecodedImage back;
    std::string note;
    CHECK(services::decodeImage(png, back, note) && back.width == 2 && back.height == 2 && back.rgba.size() == 16 &&
          std::equal(back.rgba.begin(), back.rgba.end(), pixels));
    CHECK(services::encodePng(nullptr, 2, 2).empty());
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
        if (method == "deferred") // a program run by the connection's thread, not this one
            return model::Json::object().set(
                "__process", model::Json::object().set("arguments", model::Json::array().push("git").push("--version")).set("key", "text"));
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
    done = false;
    std::thread deferredClient([&]() {
        reply = app::forwardToRunningApp("deferred", model::Json::object(), &callError);
        done = true;
    });
    for (int i = 0; i < 1000 && !done; ++i) {
        os::dispatch::pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    deferredClient.join();
    CHECK(reply && reply->getString("text").find("git version") != std::string::npos);
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
    // top: header, task summary and the process table; 'q' quits. Linux (procps) top only: the
    // BSD top of macOS takes other options and prints another layout.
#if defined(__APPLE__)
    const FullScreenRun top;
#else
    const FullScreenRun top = runFullScreen("top", {"-d", "1"}, "q");
#endif
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

TEST(explorer_creates_renames_and_deletes)
{
    services::FileService files;
    const std::string dir = scratchDirectory() + "/enp-explorer-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    os::fs::createDirectories(dir + "/src");
    files.write(dir + "/src/a.txt", "alpha needle\n", nullptr);
    files.write(dir + "/b.txt", "beta needle\n", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(!vm.canEditFiles()); // the sample workspace: nothing on disk to change
    CHECK(vm.openFolder(dir));
    CHECK(vm.canEditFiles());

    // New file (opened at once), new folder; invalid names and existing files are refused.
    CHECK(vm.createFile("src", "new.md"));
    CHECK(os::fs::exists(dir + "/src/new.md") && vm.activeDocument() && vm.activeDocument()->path == "src/new.md");
    CHECK(!vm.createFile("src", "new.md"));
    CHECK(!vm.createFile("", "bad/name"));
    CHECK(vm.createFolder("", "docs") && os::fs::isDirectory(dir + "/docs"));

    // Rename: the open document follows, also when its folder is renamed.
    CHECK(vm.renamePath("src/new.md", "renamed.md"));
    CHECK(os::fs::exists(dir + "/src/renamed.md") && !os::fs::exists(dir + "/src/new.md"));
    CHECK(vm.activeDocument()->path == "src/renamed.md" && vm.activeDocument()->title == "renamed.md");
    CHECK(vm.renamePath("src", "source"));
    CHECK(vm.activeDocument()->path == "source/renamed.md" && os::fs::exists(dir + "/source/a.txt"));

    // Find in Folder searches only under that folder.
    vm.findInFiles().query = "needle";
    vm.findInFolder("source");
    vm.findInFiles().searchNow();
    CHECK(vm.findInFiles().results().size() == 1 && vm.findInFiles().results()[0].path == "source/a.txt");
    vm.findInFiles().folder.clear();
    vm.findInFiles().searchNow();
    CHECK_EQ(vm.findInFiles().results().size(), size_t(2));

    // Delete a folder with an open document in it: the document closes, the folder goes.
    const size_t before = vm.documents().size();
    CHECK(vm.deletePath("source"));
    CHECK(!os::fs::exists(dir + "/source") && vm.documents().size() == before - 1);
    CHECK(vm.deletePath("b.txt") && !os::fs::exists(dir + "/b.txt"));
}

TEST(close_folder_asks_about_unsaved_documents)
{
    using Choice = viewmodel::NotepadViewModel::CloseChoice;
    services::FileService files;
    const std::string dir = scratchDirectory() + "/enp-closefolder-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    os::fs::createDirectories(dir);
    files.write(dir + "/a.txt", "a\n", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(!vm.noFolder()); // first start: the sample workspace
    CHECK(vm.openFolder(dir));
    vm.openFile("a.txt");
    vm.insertText("edit ");
    vm.execute(CommandId::CloseFolder);
    CHECK(vm.closeFolderPending.get() && vm.folderOpen()); // unsaved: asks first
    vm.resolveCloseFolder(Choice::Cancel);
    CHECK(vm.folderOpen() && !vm.documents().empty());
    vm.execute(CommandId::CloseFolder);
    vm.resolveCloseFolder(Choice::Save);
    CHECK(!vm.folderOpen() && vm.documents().empty() && vm.noFolder());
    CHECK_EQ(*files.read(dir + "/a.txt"), std::string("edit a\n"));
    CHECK(!vm.command(CommandId::CloseFolder).canExecute());
    CHECK(vm.openFolder(dir) && !vm.noFolder());
}

// ---- Git ----------------------------------------------------------------------------------------

TEST(git_status_parses_porcelain_v2)
{
    using namespace model::git;
    std::string s;
    auto rec = [&](const std::string& r) { s += r + '\0'; };
    rec("# branch.oid 1234567890abcdef1234567890abcdef12345678");
    rec("# branch.head main");
    rec("# branch.upstream origin/main");
    rec("# branch.ab +2 -1");
    rec("1 M. N... 100644 100644 100644 aaa bbb src/staged file.cpp");
    rec("1 .M N... 100644 100644 100644 aaa bbb src/changed.cpp");
    rec("1 MM N... 100644 100644 100644 aaa bbb both.cpp");
    rec("2 R. N... 100644 100644 100644 aaa bbb R100 new name.txt");
    rec("old name.txt");
    rec("u UU N... 100644 100644 100644 100644 a b c conflict.txt");
    rec("? untracked dir/é.txt");
    rec("1 .M SCMU 160000 160000 160000 aaa aaa 3rdparty/lib");
    rec("1 M. SC.. 160000 160000 160000 aaa bbb 3rdparty/other");
    const Status st = parseStatus(s);
    CHECK_EQ(st.branch, std::string("main"));
    CHECK_EQ(st.upstream, std::string("origin/main"));
    CHECK(st.ahead == 2 && st.behind == 1 && !st.detached && !st.initial);
    CHECK_EQ(st.changes.size(), size_t(9));
    CHECK(st.changes[0].path == "src/staged file.cpp" && st.changes[0].group == Area::Staged && st.changes[0].status == 'M');
    CHECK(st.changes[1].path == "src/changed.cpp" && st.changes[1].group == Area::Changes);
    CHECK(st.changes[2].group == Area::Staged && st.changes[3].group == Area::Changes && st.changes[3].path == "both.cpp");
    CHECK(st.changes[4].status == 'R' && st.changes[4].path == "new name.txt" && st.changes[4].origPath == "old name.txt");
    CHECK(st.changes[5].group == Area::Conflicts && st.changes[5].path == "conflict.txt");
    CHECK(st.changes[6].group == Area::Untracked && st.changes[6].path == "untracked dir/\xc3\xa9.txt");
    CHECK(st.changes[7].path == "3rdparty/lib" && st.changes[7].submodule == "new commits, modified files, untracked files");
    CHECK(st.changes[8].group == Area::Staged && st.changes[8].submodule == "new commit staged");
    CHECK(st.changes[0].submodule.empty());

    std::string d;
    for (const char* r : {"# branch.oid (initial)", "# branch.head (detached)"})
        d += std::string(r) + '\0';
    const Status detached = parseStatus(d);
    CHECK(detached.initial && detached.detached);
}

TEST(git_branches_log_and_stashes_parse)
{
    using namespace model::git;
    // Fields joined with NUL, as for-each-ref prints them with %00.
    auto join = [](std::initializer_list<const char*> fields, char end) {
        std::string out;
        bool first = true;
        for (const char* f : fields) {
            if (!first)
                out += '\0';
            first = false;
            out += f;
        }
        return out + end;
    };
    const std::string branches = join({"refs/heads/main", "main", "origin/main", "ahead 1, behind 2", "*", "100"}, '\n') +
                                 join({"refs/heads/old", "old", "origin/old", "gone", " ", "50"}, '\n') +
                                 join({"refs/remotes/origin/HEAD", "origin/HEAD", "", "", " ", "1"}, '\n') +
                                 join({"refs/remotes/origin/main", "origin/main", "", "", " ", "1"}, '\n') +
                                 join({"refs/tags/v1.0", "v1.0", "", "", " ", "1"}, '\n');
    const std::vector<Branch> b = parseBranches(branches);
    CHECK_EQ(b.size(), size_t(4)); // origin/HEAD is skipped
    CHECK(b[0].name == "main" && b[0].current && b[0].ahead == 1 && b[0].behind == 2 && b[0].lastUsed == 100);
    CHECK(b[1].gone && !b[1].current);
    CHECK(b[2].kind == RefKind::Remote && b[3].kind == RefKind::Tag);

    // Three commits: a merge (m) of a side commit (s) into a main line (a -> r).
    const std::string log = "m\x1fm\x1f" "a s\x1f" "Ann\x1f" "300\x1fHEAD -> main\x1fMerge side\x1e\n"
                            "s\x1fs\x1fr\x1f" "Bob\x1f" "200\x1f\x1fSide work\x1e\n"
                            "a\x1f" "a\x1fr\x1f" "Ann\x1f" "250\x1f\x1fMain work\x1e\n"
                            "r\x1fr\x1f\x1f" "Ann\x1f" "100\x1ftag: v1\x1fRoot\x1e\n";
    std::vector<Commit> commits = parseLog(log);
    CHECK_EQ(commits.size(), size_t(4));
    CHECK(commits[0].parents.size() == 2 && commits[0].message == "Merge side" && commits[0].time == 300);
    CHECK(commits[3].parents.empty() && commits[3].refs == "tag: v1");
    layoutGraph(commits);
    CHECK(commits[0].lane == 0 && commits[1].lane == 1 && commits[2].lane == 0 && commits[3].lane == 0);
    // The merge sends a line to each parent's lane; both lines meet again at the root.
    CHECK(std::find(commits[0].edges.begin(), commits[0].edges.end(), std::make_pair(0, 1)) != commits[0].edges.end());
    CHECK(std::find(commits[2].edges.begin(), commits[2].edges.end(), std::make_pair(1, 0)) != commits[2].edges.end());

    const std::vector<Stash> stashes = parseStashList("stash@{0}\x1fOn main: half-done ribbon\x1f" "50\n"
                                                      "stash@{1}\x1fWIP on feature/x: 1a2b3c4 subject\x1f" "40\n");
    CHECK_EQ(stashes.size(), size_t(2));
    CHECK(stashes[0].message == "half-done ribbon" && stashes[0].branch == "main" && stashes[0].time == 50);
    CHECK(stashes[1].message == "WIP on feature/x" && stashes[1].branch == "feature/x");

    const std::string names = join({"M", "a.cpp", "R087", "old.h", "new.h", "A", "b.txt"}, '\0');
    const std::string nums = join({"3\t1\ta.cpp", "2\t2\t", "old.h", "new.h", "-\t-\tb.txt"}, '\0');
    const std::vector<FileChange> files = parseFileChanges(names, nums);
    CHECK_EQ(files.size(), size_t(3));
    CHECK(files[0].path == "a.cpp" && files[0].added == 3 && files[0].removed == 1);
    CHECK(files[1].path == "new.h" && files[1].status == 'R' && files[1].added == 2);
    CHECK(files[2].status == 'A' && files[2].added == 0);
}

TEST(git_blame_names_and_times)
{
    using namespace model::git;
    const std::string h1(40, 'a'), h2(40, 'b');
    const std::string blame = h1 + " 1 1 2\nauthor Ann\nauthor-time 100\nsummary First\n\tline one\n" + h1 +
                              " 2 2\n\tline two\n" + h2 + " 3 3 1\nauthor Bob\nauthor-time 200\nsummary Second\n\tline three\n";
    const std::vector<BlameLine> lines = parseBlame(blame);
    CHECK_EQ(lines.size(), size_t(3));
    CHECK(lines[0].author == "Ann" && lines[1].hash == h1 && lines[1].summary == "First" && lines[2].author == "Bob");

    CHECK(validBranchName("feature/login-button") && validBranchName("v1.2"));
    for (const char* bad : {"", "has space", "a..b", "-x", "x/", "x.lock", "a~1", "a^", "a:b", "@", "x@{1}", "a\\b"})
        CHECK(!validBranchName(bad));
    CHECK_EQ(relativeTime(1000, 1030), std::string("just now"));
    CHECK_EQ(relativeTime(0, 7200), std::string("2 hours ago"));
    CHECK_EQ(relativeTime(0, 90000), std::string("yesterday"));
    CHECK_EQ(relativeTime(0, 60), std::string("1 minute ago"));
}

TEST(git_conflict_blocks_resolve)
{
    using namespace model::git;
    const std::vector<std::string> text = {"keep", "<<<<<<< HEAD", "ours 1", "ours 2", "=======", "theirs", ">>>>>>> feature",
                                           "middle", "<<<<<<< HEAD", "a", "||||||| base", "b", "=======", "c", ">>>>>>> other",
                                           "======= not a marker without a block", "end"};
    const std::vector<ConflictBlock> blocks = findConflicts(text);
    CHECK_EQ(blocks.size(), size_t(2));
    CHECK(blocks[0].start == 1 && blocks[0].middle == 4 && blocks[0].end == 6 && blocks[0].incoming == "feature");
    CHECK(blocks[1].base == 10 && blocks[1].current == "HEAD");
    CHECK(resolveConflict(text, blocks[0], Resolution::Current) == (std::vector<std::string>{"ours 1", "ours 2"}));
    CHECK(resolveConflict(text, blocks[0], Resolution::Incoming) == (std::vector<std::string>{"theirs"}));
    CHECK(resolveConflict(text, blocks[0], Resolution::Both) == (std::vector<std::string>{"ours 1", "ours 2", "theirs"}));
    CHECK(resolveConflict(text, blocks[1], Resolution::Current) == (std::vector<std::string>{"a"})); // the base is dropped
}

TEST(git_partial_patch_keeps_unchosen_rows)
{
    using namespace model::git;
    const std::string oldText = "one\ntwo\nthree\nfour\nfive\n";
    const std::string newText = "ONE\ntwo\nthree\nfour\nFIVE\nsix\n";
    const std::vector<model::DiffRow> rows = model::diffLines(model::splitLines(oldText), model::splitLines(newText));
    std::vector<bool> chosen(rows.size(), false);
    const std::pair<int, int> first = hunkAt(rows, 0);
    for (int i = first.first; i <= first.second; ++i)
        chosen[size_t(i)] = true;
    const std::string patch = buildPatch("f.txt", oldText, rows, chosen);
    CHECK(patch.find("-one\n+ONE\n") != std::string::npos);
    CHECK(patch.find(" five\n") != std::string::npos); // the second change is left out: context
    CHECK(patch.find("six") == std::string::npos);
    CHECK(patch.find("@@ -1,5 +1,5 @@") != std::string::npos);
    // Without a final newline the last old line is marked.
    const std::string noEol = "a\nb";
    const std::vector<model::DiffRow> r2 = model::diffLines(model::splitLines(noEol), model::splitLines("a\nB"));
    const std::string p2 = buildPatch("g.txt", noEol, r2, std::vector<bool>(r2.size(), true));
    CHECK(p2.find("-b\n\\ No newline at end of file\n") != std::string::npos);
}

namespace {

std::string fileName(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Runs git directly (test set-up), in `dir`.
std::string gitIn(const std::string& dir, std::vector<std::string> args)
{
    os::process::Command c;
    c.arguments = {"git", "-c", "core.autocrlf=false"};
    c.arguments.insert(c.arguments.end(), args.begin(), args.end());
    c.workingDirectory = dir;
    return os::process::run(c).output;
}

// Pumps the main-thread queue (where git's results arrive) until `done` or a timeout.
bool pumpUntil(viewmodel::NotepadViewModel& vm, const std::function<bool()>& done, int milliseconds = 20000)
{
    for (int waited = 0; waited < milliseconds; waited += 5) {
        os::dispatch::pump();
        if (done() && !vm.git().busy())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

// Stop ends the program and what it started at once (a child left running would hold the output
// pipe open, and the run would wait for it).
TEST(process_cancel_stops_the_program_and_its_children)
{
#if defined(_WIN32)
    const std::string command = "ping -n 30 127.0.0.1 > nul";
#else
    const std::string command = "sleep 30; true";
#endif
    os::process::CancelFlag cancel = std::make_shared<std::atomic<bool>>(false);
    std::thread stopper([cancel]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        *cancel = true;
    });
    const auto start = std::chrono::steady_clock::now();
    const os::process::Result result = os::process::run(command, "", "", 60, cancel);
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
    stopper.join();
    CHECK(result.started);
    CHECK(result.cancelled);
    CHECK(!result.timedOut);
    CHECK(seconds < 5);
}

TEST(git_end_to_end_in_a_real_repository)
{
    if (os::process::findExecutable("git").empty()) {
        std::printf("  (git not installed: skipped)\n");
        return;
    }
    using model::git::Area;
    services::FileService files;
    services::ProcessService processes;
    const std::string dir = scratchDirectory() + "/enp-git-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    os::fs::createDirectories(dir);
    viewmodel::NotepadViewModel vm(nullptr, &files, nullptr, &processes);
    viewmodel::GitViewModel& git = vm.git();
    CHECK(git.demo()); // no folder yet: the demo repository
    CHECK(vm.openFolder(dir));
    CHECK(pumpUntil(vm, [&]() { return git.available(); }));
    CHECK(!git.demo() && !git.repo().repository);

    // A new repository; its first file is untracked, then staged, then committed.
    git.init();
    CHECK(pumpUntil(vm, [&]() { return git.repo().repository; }));
    gitIn(dir, {"config", "user.name", "Test"});
    gitIn(dir, {"config", "user.email", "test@example.com"});
    gitIn(dir, {"config", "core.autocrlf", "false"});
    files.write(dir + "/a.txt", "one\ntwo\nthree\nfour\nfive\nsix\nseven\n", nullptr);
    git.refresh();
    CHECK(pumpUntil(vm, [&]() { return !git.repo().status.changes.empty(); }));
    CHECK(git.repo().status.changes.size() == 1 && git.repo().status.changes[0].group == Area::Untracked);
    git.stage({"a.txt"});
    CHECK(pumpUntil(vm, [&]() { return !git.repo().status.changes.empty() && git.repo().status.changes[0].group == Area::Staged; }));
    git.commitMessage = "First \"quoted\" $commit\n\nWith a body.";
    git.commitWithMessage();
    CHECK(pumpUntil(vm, [&]() { return git.repo().status.changes.empty() && !git.repo().status.initial; }));
    CHECK_EQ(gitIn(dir, {"log", "-1", "--format=%B"}), std::string("First \"quoted\" $commit\n\nWith a body.\n\n"));
    CHECK(git.commitMessage.empty());
    // The Status tab's last commit; never fetched (no remote yet).
    CHECK(pumpUntil(vm, [&]() { return git.repo().headSubject == "First \"quoted\" $commit"; }));
    CHECK(git.repo().headShort.size() >= 7 && git.repo().headTime > 0);
    CHECK_EQ(git.repo().fetchedAt, int64_t(0));

    // Two separate changes; stage only the first through the Diff tab's rows.
    files.write(dir + "/a.txt", "ONE\ntwo\nthree\nfour\nfive\nsix\nSEVEN\n", nullptr);
    git.refresh();
    CHECK(pumpUntil(vm, [&]() { return git.changeCount() == 1; }));
    git.showChange("a.txt", Area::Changes);
    CHECK(pumpUntil(vm, [&]() { return git.diffContext().active; }));
    std::vector<bool> chosen(git.diffContext().rows.size(), false);
    const std::pair<int, int> hunk = model::git::hunkAt(git.diffContext().rows, 0);
    for (int i = hunk.first; i <= hunk.second; ++i)
        chosen[size_t(i)] = true;
    git.applyRows(chosen, viewmodel::GitViewModel::RowsAction::Stage);
    CHECK(pumpUntil(vm, [&]() { return git.repo().status.changes.size() == 2; })); // staged and still changed
    const std::string cached = gitIn(dir, {"diff", "--cached"});
    CHECK(cached.find("+ONE") != std::string::npos && cached.find("SEVEN") == std::string::npos);
    // Unstage it again by rows, then discard the working change of the last line.
    git.showChange("a.txt", Area::Staged);
    CHECK(pumpUntil(vm, [&]() { return git.diffContext().active && git.diffContext().area == Area::Staged; }));
    git.applyRows(std::vector<bool>(git.diffContext().rows.size(), true), viewmodel::GitViewModel::RowsAction::Unstage);
    CHECK(pumpUntil(vm, [&]() { return gitIn(dir, {"diff", "--cached"}).empty(); }));
    git.discard({"a.txt"});
    CHECK(pumpUntil(vm, [&]() { return git.changeCount() == 0; }));
    CHECK_EQ(*files.read(dir + "/a.txt"), std::string("one\ntwo\nthree\nfour\nfive\nsix\nseven\n"));

    // Branches: create and switch, commit there, back to the first branch.
    const std::string mainBranch = git.repo().status.branch;
    git.createBranch("feature/x", {}, true);
    CHECK(pumpUntil(vm, [&]() { return git.repo().status.branch == "feature/x"; }));
    files.write(dir + "/a.txt", "one\ntwo\nTHREE from feature\nfour\nfive\nsix\nseven\n", nullptr);
    files.write(dir + "/b.txt", "new on feature\n", nullptr);
    git.stage({"a.txt", "b.txt"});
    git.commit("Feature work", false);
    CHECK(pumpUntil(vm, [&]() { return git.changeCount() == 0 && git.repo().status.branch == "feature/x"; }));
    git.switchTo(mainBranch);
    CHECK(pumpUntil(vm, [&]() { return git.repo().status.branch == mainBranch; }));

    // Compare and take one file from the branch, staged.
    git.compare(mainBranch, "feature/x");
    CHECK(pumpUntil(vm, [&]() { return git.repo().compareFiles.size() == 2; }));
    CHECK_EQ(git.repo().compareCommits.size(), size_t(1));
    git.takeFiles("feature/x", {"b.txt"}, true);
    CHECK(pumpUntil(vm, [&]() { return git.changeCount() == 1; }));
    CHECK(git.repo().status.changes[0].path == "b.txt" && git.repo().status.changes[0].group == Area::Staged);
    git.commit("Take b.txt from feature", false);
    CHECK(pumpUntil(vm, [&]() { return git.changeCount() == 0; }));

    // A conflicting change on the main branch, then merging the feature stops with a conflict.
    files.write(dir + "/a.txt", "one\ntwo\nTHREE from main\nfour\nfive\nsix\nseven\n", nullptr);
    git.stage({"a.txt"});
    git.commit("Main work", false);
    CHECK(pumpUntil(vm, [&]() { return git.changeCount() == 0; }));
    git.merge("feature/x");
    CHECK(pumpUntil(vm, [&]() { return git.repo().operation == viewmodel::GitOperation::Merge; }));
    CHECK(git.repo().status.changes.size() == 1 && git.repo().status.changes[0].group == Area::Conflicts);
    const std::vector<std::string> conflicted = model::git::lines(*files.read(dir + "/a.txt"));
    const std::vector<model::git::ConflictBlock> blocks = model::git::findConflicts(conflicted);
    CHECK_EQ(blocks.size(), size_t(1));
    // Resolve by keeping both sides, mark resolved, continue.
    std::vector<std::string> resolved(conflicted.begin(), conflicted.begin() + blocks[0].start);
    for (const std::string& l : model::git::resolveConflict(conflicted, blocks[0], model::git::Resolution::Both))
        resolved.push_back(l);
    resolved.insert(resolved.end(), conflicted.begin() + blocks[0].end + 1, conflicted.end());
    std::string text;
    for (const std::string& l : resolved)
        text += l + "\n";
    files.write(dir + "/a.txt", text, nullptr);
    git.stage({"a.txt"});
    CHECK(pumpUntil(vm, [&]() { return !git.repo().status.changes.empty() && git.repo().status.changes[0].group == Area::Staged; }));
    git.continueOperation();
    CHECK(pumpUntil(vm, [&]() { return git.repo().operation == viewmodel::GitOperation::None && git.changeCount() == 0; }));
    CHECK(files.read(dir + "/a.txt")->find("THREE from main\nTHREE from feature") != std::string::npos);
    CHECK(gitIn(dir, {"log", "-1", "--format=%P"}).find(' ') != std::string::npos); // a merge commit

    // Stash only one of two changed files, then pop it back.
    files.write(dir + "/a.txt", "changed a\n", nullptr);
    files.write(dir + "/b.txt", "changed b\n", nullptr);
    git.refresh();
    CHECK(pumpUntil(vm, [&]() { return git.changeCount() == 2; }));
    git.stashPush("only a", {"a.txt"}, true, false);
    CHECK(pumpUntil(vm, [&]() { return git.repo().stashes.size() == 1 && git.changeCount() == 1; }));
    CHECK(git.repo().stashes[0].message == "only a" && git.repo().status.changes[0].path == "b.txt");
    git.stashApply(0, true);
    CHECK(pumpUntil(vm, [&]() { return git.repo().stashes.empty() && git.changeCount() == 2; }));

    // History: the log, the graph, and a commit's files.
    git.loadLog(false, {});
    CHECK(pumpUntil(vm, [&]() { return git.repo().logLoaded && git.repo().log.size() >= 5; }));
    CHECK_EQ(git.repo().log.front().parents.size(), size_t(2));
    git.loadCommitFiles(0);
    CHECK(pumpUntil(vm, [&]() { return !git.repo().log.front().files.empty(); }));

    // The merged branch can be deleted without force.
    git.deleteBranch("feature/x", false);
    CHECK(pumpUntil(vm, [&]() { return git.localBranchNames().size() == 1; }));

    // The connector sees the same state.
    const model::Json status = app::handleAutomation(vm, "git_status", model::Json::object());
    CHECK_EQ(status.getString("branch"), mainBranch);
    CHECK(status.find("changes") && status.find("changes")->isArray());

    // A remote (a bare repository next to it): publish the branch, then clone it elsewhere.
    const std::string bare = dir + "-origin.git";
    gitIn(scratchDirectory(), {"init", "--bare", "-q", bare});
    git.addRemote("origin", bare);
    git.loadRemotes();
    CHECK(pumpUntil(vm, [&]() { return git.remotes().size() == 1; }));
    git.stage({"a.txt", "b.txt"});
    git.commit("Before publishing", false);
    CHECK(pumpUntil(vm, [&]() { return git.changeCount() == 0; }));
    CHECK(git.repo().status.upstream.empty());
    git.push();
    CHECK(pumpUntil(vm, [&]() { return !git.repo().status.upstream.empty(); }));
    CHECK_EQ(git.repo().status.upstream, "origin/" + mainBranch);
    CHECK_EQ(viewmodel::GitViewModel::cloneName("https://example.com/team/project.git/"), std::string("project"));
    git.clone(bare, scratchDirectory(), fileName(dir) + "-clone");
    CHECK(pumpUntil(vm, [&]() { return vm.workspaceRoot().find("-clone") != std::string::npos && git.repo().repository; }));
    CHECK(pumpUntil(vm, [&]() { return !git.repo().log.empty() || (git.loadLog(false, {}), false); }));

    // The guide switches to demo data and back.
    git.setLearning(true);
    CHECK(git.demo() && git.repo().status.branch == "feature/git-ui");
    git.setLearning(false);
    CHECK(!git.demo() && git.repo().status.branch == mainBranch);
    CHECK(git.repo().status.upstream == "origin/" + mainBranch); // the clone tracks origin
}

TEST(sha256_in_pieces_matches_one_shot)
{
    const std::string data(1000003, 'a');
    model::Sha256 hash;
    size_t at = 0;
    for (size_t step = 1; at < data.size(); step = step * 3 + 1) { // uneven pieces across block edges
        const size_t n = std::min(step % 9000 + 1, data.size() - at);
        hash.update(data.data() + at, n);
        at += n;
    }
    CHECK_EQ(hash.hexDigest(), model::sha256Hex(data));
}

// An engine that answers at once: files are sizes in a map, downloads finish when told to.
struct FakeEngine final : interfaces::ILocalEngineService {
    std::map<std::string, uint64_t> files;
    uint64_t free = 100ull << 30;
    std::string hashAnswer;
    interfaces::DownloadStatus download;
    std::string downloadPath;
    bool downloadMetered = false;
    bool metered = false;
    std::string refuse; // startDownload fails with this
    std::vector<interfaces::EngineRunSettings> runs;

    interfaces::EngineInfo info() override
    {
        interfaces::EngineInfo i;
        i.available = true;
        i.cpuName = "Test CPU";
        i.cpuThreads = 12;
        i.memoryTotal = 16ull << 30;
        return i;
    }
    void run(const interfaces::EngineRunSettings& s, std::function<void(const std::string&)> progress,
             std::function<void(const interfaces::EngineRunResult&)> done) override
    {
        runs.push_back(s);
        progress("Generating");
        interfaces::EngineRunResult r;
        r.ok = true;
        r.backend = "CPU, 6 threads";
        r.promptTokensPerSecond = 120;
        r.generateTokensPerSecond = s.drafterPath.empty() ? 20 : 30;
        if (!s.drafterPath.empty()) {
            r.drafted = 10;
            r.accepted = 6;
        }
        done(r);
    }
    void cancel() override {}
    bool running() const override { return false; }
    std::string modelsDirectory() override { return "/models"; }
    uint64_t freeSpace(const std::string&) override { return free; }
    uint64_t fileSize(const std::string& path) override { return files.count(path) ? files[path] : 0; }
    bool startDownload(const std::string&, const std::string& path, bool allowMetered, std::string* error) override
    {
        if (!refuse.empty()) {
            if (error)
                *error = refuse;
            return false;
        }
        downloadPath = path;
        downloadMetered = allowMetered;
        download.state = interfaces::DownloadStatus::State::Running;
        return true;
    }
    interfaces::DownloadStatus downloadStatus() override { return download; }
    void cancelDownload() override { download.state = interfaces::DownloadStatus::State::Idle; }
    bool meteredNetwork() override { return metered; }
    std::string importPath;
    bool canImport() override { return true; }
    bool importFile(const std::string& path, std::string*) override
    {
        importPath = path;
        download.state = interfaces::DownloadStatus::State::Running;
        return true;
    }
    // The local agent: answers at once, in two pieces (or fails when told to).
    std::vector<interfaces::LocalChatRequest> chats;
    bool chatFails = false;
    void chat(const interfaces::LocalChatRequest& request, std::function<void(const std::string&)> stage,
              std::function<void(const std::string&)> text, std::function<void(const interfaces::LocalChatResult&)> done) override
    {
        chats.push_back(request);
        interfaces::LocalChatResult r;
        if (chatFails) {
            r.error = "out of memory";
            done(r);
            return;
        }
        stage("Writing");
        text("Local ");
        text("answer.");
        r.ok = true;
        r.generatedTokens = 3;
        done(r);
    }
    void hashFile(const std::string&, std::function<void(double)> progress, std::function<void(const std::string&)> done) override
    {
        progress(0.5);
        done(hashAnswer);
    }
};

TEST(engine_lab_downloads_checks_and_compares)
{
    using Lab = viewmodel::EngineLabViewModel;
    using State = Lab::FileState;
    FakeEngine engine;
    Lab lab(&engine);
    std::string status;
    auto sub = lab.statusMessage.subscribe([&](const std::string& s) { status = s; });
    lab.info();
    CHECK_EQ(lab.threads.get(), 6); // half the threads on a desktop
    const std::string modelPath = "/models/" + std::string(Lab::modelFile().fileName);
    const std::string drafterPath = "/models/" + std::string(Lab::drafterFile().fileName);
    CHECK(lab.fileStatus(Lab::Which::Model).state == State::Missing);
    CHECK(!lab.fileUsable(Lab::Which::Model));

    // Not enough room: refused before anything starts.
    engine.free = 1ull << 30;
    lab.download(Lab::Which::Model);
    CHECK(!lab.downloading());
    CHECK(status.find("Not enough space") != std::string::npos);

    // On mobile data it asks first; confirmed, it downloads over mobile data.
    engine.free = 100ull << 30;
    engine.metered = true;
    lab.download(Lab::Which::Model);
    CHECK(lab.confirmationPending() && !lab.downloading() && lab.pendingDownload() == Lab::Which::Model);
    lab.cancelConfirmation();
    CHECK(!lab.confirmationPending() && !lab.downloading());
    lab.download(Lab::Which::Model);
    lab.confirmMobileDownload();
    CHECK(lab.downloading() && engine.downloadMetered);
    lab.cancelDownload();
    engine.metered = false;

    // Failures are told in the lab itself and stay on the file until it is tried again.
    engine.refuse = "SecurityException: not allowed";
    lab.download(Lab::Which::Model);
    CHECK(!lab.downloading() && lab.noticeIsError() && lab.notice().find("not allowed") != std::string::npos);
    CHECK(lab.downloadError(Lab::Which::Model).find("not allowed") != std::string::npos);
    engine.refuse.clear();
    lab.download(Lab::Which::Model);
    CHECK(lab.downloading() && lab.downloadError(Lab::Which::Model).empty() && !lab.noticeIsError());
    engine.download.state = interfaces::DownloadStatus::State::Idle; // dropped by the system
    lab.refresh();
    CHECK(!lab.downloading() && lab.noticeIsError() && !lab.downloadError(Lab::Which::Model).empty());

    // Download, then the checksum is checked by itself; a match makes the file ready.
    lab.download(Lab::Which::Model);
    CHECK(lab.downloading() && engine.downloadPath == modelPath);
    CHECK(lab.fileStatus(Lab::Which::Model).state == State::Downloading);
    lab.refresh(); // still running
    CHECK(lab.downloading());
    engine.download.state = interfaces::DownloadStatus::State::Done;
    engine.files[modelPath] = Lab::modelFile().size;
    engine.hashAnswer = Lab::modelFile().sha256;
    lab.refresh();
    CHECK(!lab.downloading());
    CHECK(lab.fileStatus(Lab::Which::Model).state == State::Ready);

    // A wrong checksum is reported, and the file is not run.
    engine.files[drafterPath] = Lab::drafterFile().size;
    CHECK(lab.fileStatus(Lab::Which::Drafter).state == State::Unverified);
    engine.hashAnswer = "0000";
    lab.verify(Lab::Which::Drafter);
    CHECK(lab.fileStatus(Lab::Which::Drafter).state == State::Mismatch);
    CHECK(!lab.fileUsable(Lab::Which::Drafter));
    engine.hashAnswer = Lab::drafterFile().sha256;
    lab.verify(Lab::Which::Drafter);
    CHECK(lab.fileStatus(Lab::Which::Drafter).state == State::Ready);

    // A file picked on the phone is copied in like a download, then checked.
    lab.importFile(Lab::Which::Drafter);
    CHECK(lab.importing() && engine.importPath == drafterPath);
    CHECK(lab.fileStatus(Lab::Which::Drafter).state == State::Downloading);
    engine.download.state = interfaces::DownloadStatus::State::Done;
    engine.files[drafterPath] = Lab::drafterFile().size;
    engine.hashAnswer = Lab::drafterFile().sha256;
    lab.refresh();
    CHECK(!lab.importing() && lab.fileStatus(Lab::Which::Drafter).state == State::Ready);

    // The comparison runs without and then with the drafter; newest first.
    lab.runComparison();
    CHECK_EQ(engine.runs.size(), size_t(2));
    CHECK(engine.runs.size() == 2 && engine.runs[0].drafterPath.empty() && engine.runs[1].drafterPath == drafterPath);
    CHECK_EQ(lab.history().size(), size_t(2));
    CHECK(!lab.history().empty() && !lab.history().front().settings.drafterPath.empty());
    const std::string report = lab.report();
    CHECK(report.find("+ drafter") != std::string::npos && report.find("accepted 6 (60%)") != std::string::npos);

    // Saved and loaded: results and checked files survive a restart.
    Lab restored(&engine);
    restored.load(model::Json::parse(lab.save().dump()).value_or(model::Json()));
    CHECK_EQ(restored.history().size(), size_t(2));
    CHECK(restored.fileStatus(Lab::Which::Model).state == State::Ready);
    CHECK(!restored.history().empty() && restored.history().front().result.accepted == 6);

    // A file of one's own is used as it is.
    engine.files["/sdcard/mine.gguf"] = 123;
    lab.customModelPath.set("/sdcard/mine.gguf");
    CHECK(lab.fileStatus(Lab::Which::Model).state == State::Custom && lab.fileUsable(Lab::Which::Model));
    lab.run(false);
    CHECK(!engine.runs.empty() && engine.runs.back().modelPath == "/sdcard/mine.gguf");

    // Hidden until developer mode is on.
    viewmodel::NotepadViewModel vm(nullptr, nullptr, nullptr, nullptr, &engine);
    int opened = 0;
    auto openSub = vm.engineLabRequested.subscribe([&]() { ++opened; });
    CHECK(!vm.command(CommandId::EngineLab).canExecute());
    // The download itself is for everyone: the Local Model command opens the agent set-up card.
    CHECK(vm.command(CommandId::LocalModel).canExecute());
    vm.execute(CommandId::LocalModel);
    CHECK(vm.agent().showSetup.get() && vm.showAgentPanel.get());
    vm.developerMode.set(true);
    CHECK(vm.command(CommandId::EngineLab).canExecute());
    vm.execute(CommandId::EngineLab);
    CHECK_EQ(opened, 1);
}

TEST(agent_auto_route_rules)
{
    using model::AgentRoute;
    auto route = [](const std::string& prompt) { return model::routeTask(prompt).route; };
    CHECK(route("Explain this function") == AgentRoute::Local);
    CHECK(route("what does parseArgs return?") == AgentRoute::Local);
    CHECK(route("Write a commit message for these changes") == AgentRoute::Local);
    CHECK(route("is this thread safe?") == AgentRoute::Local);
    CHECK(route("What's the address it binds to?") == AgentRoute::Local); // "address" is not "add"
    CHECK(route("Refactor the parser into two classes") == AgentRoute::Claude);
    CHECK(route("Add tests for TextBuffer") == AgentRoute::Claude);
    CHECK(route("fix the crash in main.cpp") == AgentRoute::Claude);
    CHECK(route("rename it across all files") == AgentRoute::Claude);
    CHECK(route("Please look at this") == AgentRoute::Claude); // unclear: the stronger agent
    CHECK(route("why? " + std::string(700, 'x')) == AgentRoute::Claude); // long
}

TEST(agent_chooses_local_or_claude_and_falls_back)
{
    using Choice = viewmodel::AgentViewModel::Choice;
    FakeEngine engine;
    FakeStreamingAgent cli;
    viewmodel::AgentViewModel agent;
    bool ready = true;
    agent.setLocal(
        &engine,
        [&]() {
            viewmodel::AgentViewModel::LocalSetup s;
            s.ready = ready;
            s.why = ready ? "" : "the model is not downloaded yet";
            s.modelPath = "/models/m.gguf";
            return s;
        },
        []() { return std::string("```\nint parse(const char* s);\n```"); });
    agent.goLive(&cli, "/work");
    CHECK(agent.claudeReady() && agent.localReady());
    const auto last = [&]() { return agent.messages().back(); };

    // Auto: a question about the code goes to the local model, with the code itself.
    agent.submitPrompt("Explain this function", "src/a.cpp:3");
    CHECK_EQ(engine.chats.size(), size_t(1));
    CHECK(!engine.chats.empty() && engine.chats.back().messages.front().role == "system");
    CHECK(!engine.chats.empty() && engine.chats.back().messages.back().text.find("int parse(const char* s);") != std::string::npos);
    CHECK(!engine.chats.empty() && engine.chats.back().messages.back().text.find("src/a.cpp:3") != std::string::npos);
    CHECK(last().by == "Local" && last().text == "Local answer." && !agent.busy.get());

    // Auto: a change goes to Claude Code, and its answer is tagged.
    agent.submitPrompt("Refactor the parser");
    CHECK(agent.busy.get() && cli.request.prompt.find("Refactor the parser") != std::string::npos);
    cli.onLine(R"({"type":"result","subtype":"success","is_error":false,"result":"Done.","session_id":"s-1","num_turns":1,"total_cost_usd":0.01})");
    CHECK(last().by == "Claude" && !agent.busy.get());

    // A follow-up in Claude's conversation stays with Claude, even a quick question.
    agent.submitPrompt("why is that?");
    CHECK_EQ(engine.chats.size(), size_t(1));
    cli.onLine(R"({"type":"result","subtype":"success","is_error":false,"result":"Because.","session_id":"s-1","num_turns":1,"total_cost_usd":0.01})");

    // Claude Code fails: the local model answers, and says why.
    agent.submitPrompt("Add tests for it");
    cli.finished(false, "network is unreachable");
    CHECK_EQ(engine.chats.size(), size_t(2));
    CHECK(last().by == "Local" && !agent.busy.get());
    bool explained = false;
    for (const model::ChatMessage& m : agent.messages())
        explained = explained || m.text.find("so the local model answers") != std::string::npos;
    CHECK(explained);
    // The earlier turns went along, alternating and starting with the user.
    const auto& sent = engine.chats.back().messages;
    CHECK(sent.size() >= 3 && sent[1].role == "user");
    for (size_t i = 2; i < sent.size(); ++i)
        CHECK(sent[i].role != sent[i - 1].role);

    // Ask Claude instead: the same question goes to Claude Code.
    agent.askClaudeInstead(int(agent.messages().size()) - 1);
    CHECK(agent.busy.get() && cli.request.prompt.find("Add tests for it") != std::string::npos);
    cli.onLine(R"({"type":"result","subtype":"success","is_error":false,"result":"Added.","session_id":"s-1","num_turns":1,"total_cost_usd":0.01})");
    CHECK(last().by == "Claude");

    // Always local, even for a change.
    agent.agentChoice.set(int(Choice::Local));
    agent.submitPrompt("Refactor everything");
    CHECK(last().by == "Local");

    // A failing local model says so.
    engine.chatFails = true;
    agent.submitPrompt("Explain it again");
    CHECK(last().role == model::ChatRole::Tool && last().text.find("out of memory") != std::string::npos && !agent.busy.get());
    engine.chatFails = false;

    // Not downloaded: it says what to do instead of failing silently.
    ready = false;
    agent.submitPrompt("explain");
    CHECK(last().role == model::ChatRole::Tool && last().text.find("not downloaded") != std::string::npos);

    // Always Claude, with the fallback off: the local model is not used.
    ready = true;
    agent.agentChoice.set(int(Choice::Claude));
    agent.localFallback.set(false);
    const size_t chats = engine.chats.size();
    agent.submitPrompt("Explain this function");
    CHECK(agent.busy.get() && engine.chats.size() == chats);
    cli.finished(false, "network is unreachable");
    CHECK(engine.chats.size() == chats && !agent.busy.get());
}


TEST(opening_another_folder_forgets_the_previous_one)
{
    // The layers' file facts are keyed by workspace-relative path and Find in Files keeps its hits:
    // both must go with the folder, or they describe the wrong files and grow with every folder.
    services::FileService files;
    const std::string a = scratchDirectory() + "/enp-test-folder-a";
    const std::string b = scratchDirectory() + "/enp-test-folder-b";
    os::fs::createDirectories(a + "/src");
    os::fs::createDirectories(b + "/src");
    files.write(a + "/src/x.cpp", "int a;\nint b;\nint c;", nullptr);
    files.write(b + "/src/x.cpp", "int a;\nint b;\nint c;\nint d;\nint e;\nint f;", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    // Project scope, zoomed into src: the line count of src/x.cpp as the layers describe it.
    auto linesOfX = [&]() {
        viewmodel::LayersViewModel& layers = vm.layers();
        layers.scope.set(viewmodel::LayerScope::Project);
        const std::vector<viewmodel::LayerNode>& nodes = layers.nodes();
        for (int i = 0; i < int(nodes.size()); ++i)
            if (nodes[size_t(i)].id == "dir:src")
                layers.zoomIn(i);
        for (const viewmodel::LayerNode& n : layers.nodes())
            if (n.id == "file:src/x.cpp")
                return n.size;
        return -1;
    };
    CHECK(vm.openFolder(a));
    CHECK_EQ(linesOfX(), 3);
    vm.findInFiles().query = "int";
    vm.findInFiles().searchNow();
    CHECK_EQ(vm.findInFiles().hitCount(), 3);

    CHECK(vm.openFolder(b));
    CHECK(vm.findInFiles().results().empty());
    CHECK_EQ(vm.findInFiles().hitCount(), 0);
    CHECK_EQ(linesOfX(), 6); // not the first folder's 3

    vm.findInFiles().searchNow();
    CHECK_EQ(vm.findInFiles().hitCount(), 6);
    vm.closeFolder();
    CHECK(vm.findInFiles().results().empty());
    CHECK(vm.layers().nodes().size() <= 1); // the project node at most: nothing of the closed folder
    // Closing asks the app to give memory back, once.
    CHECK(vm.takeMemoryTrimRequest());
    CHECK(!vm.takeMemoryTrimRequest());
    vm.newDocument();
    vm.closeDocument(0);
    CHECK(vm.takeMemoryTrimRequest()); // the last document closed
    os::trimMemory();                  // and this must be callable any time
}

TEST(process_stats_describe_this_process)
{
    const os::ProcessStats s = os::processStats();
#if defined(_WIN32)
    // Not the working set: os::trimMemory() (the test before) empties it on Windows, so it can be
    // far below a megabyte. The commit charge stays.
    CHECK(s.residentBytes > 0);
    CHECK(s.privateBytes > 1024 * 1024);
    CHECK(s.threads >= 1);
    CHECK(s.handles >= 3); // stdin, stdout, stderr at least
#elif defined(__linux__)
    CHECK(s.residentBytes > 1024 * 1024);
    CHECK(s.threads >= 1);
    CHECK(s.handles >= 3); // stdin, stdout, stderr at least
#else
    (void)s;
#endif
}

TEST(explorer_scans_breadth_first_within_limits_and_reads_deep_folders_on_demand)
{
    // A repository whose first folder alone exceeds the budget used to show as that folder and
    // nothing else; a folder of a million files was read whole.
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-explorer";
    for (const char* dir : {"/a", "/b", "/c/d/e"})
        os::fs::createDirectories(root + dir);
    for (int i = 0; i < 10; ++i)
        files.write(root + "/a/f" + std::to_string(i) + ".txt", "x", nullptr);
    files.write(root + "/b/one.txt", "x", nullptr);
    files.write(root + "/b/two.txt", "x", nullptr);
    files.write(root + "/c/d/e/deep.txt", "x", nullptr);
    // Hidden entries: a dot folder, a dot file, and the .git folder (which never shows).
    os::fs::createDirectories(root + "/.github/workflows");
    os::fs::createDirectories(root + "/.git/objects");
    files.write(root + "/.gitignore", "build/\n", nullptr);
    files.write(root + "/.github/workflows/build.yml", "on: push\n", nullptr);

    bool more = false;
    CHECK_EQ(files.list(root + "/a", 3, &more).size(), size_t(3));
    CHECK(more);
    CHECK_EQ(files.list(root + "/b", 5, &more).size(), size_t(2));
    CHECK(!more);
    {
        // Listings include hidden entries, flagged, so that callers decide.
        const std::vector<interfaces::DirectoryEntry> top = files.list(root);
        int hidden = 0, shown = 0;
        for (const interfaces::DirectoryEntry& e : top)
            (e.hidden ? hidden : shown)++;
        CHECK_EQ(hidden, 3); // .git, .github, .gitignore
        CHECK_EQ(shown, 3);  // a, b, c
    }

    viewmodel::NotepadViewModel vm(nullptr, &files);
    vm.setExplorerLimits({8, 40, 2}); // the top level (6 entries with the hidden ones) fits
    CHECK(vm.openFolder(root));
    auto find = [&](const std::string& path) -> const viewmodel::ExplorerItem* {
        auto walk = [&](auto&& self, const viewmodel::ExplorerItem& item) -> const viewmodel::ExplorerItem* {
            if (item.path == path)
                return &item;
            for (const viewmodel::ExplorerItem& child : item.children)
                if (const viewmodel::ExplorerItem* found = self(self, child))
                    return found;
            return nullptr;
        };
        return walk(walk, vm.explorer());
    };
    // .github and .gitignore show by default (folders first), .git never; off, no dot entries.
    CHECK_EQ(vm.explorer().children.size(), size_t(5)); // .github, a, b, c, .gitignore
    CHECK(find(".github") && find(".github")->isDirectory && find(".gitignore") && !find(".git"));
    CHECK(find(".github/workflows") != nullptr);
    vm.showHiddenFiles.set(false);
    CHECK_EQ(vm.explorer().children.size(), size_t(3)); // a, b and c: all of the top level
    CHECK(!find(".github") && !find(".gitignore"));
    vm.showHiddenFiles.set(true);
    CHECK(find(".github") != nullptr);
    vm.showHiddenFiles.set(false);
    const viewmodel::ExplorerItem* a = find("a");
    CHECK(a && a->children.size() == 8 && a->truncated);
    const viewmodel::ExplorerItem* b = find("b");
    CHECK(b && b->children.size() == 2 && !b->truncated);
    const viewmodel::ExplorerItem* e = find("c/d/e");
    CHECK(e && !e->scanned && e->children.empty()); // beyond the depth: not read yet
    vm.expandExplorer("c/d/e");
    e = find("c/d/e");
    CHECK(e && e->scanned && e->children.size() == 1 && e->children[0].name == "deep.txt");
}

TEST(memory_accounting_counts_scopes_and_folder_cycles_release_everything)
{
    // docs/MEMORY.md: every subsystem's state is attributed to its scope, and opening and closing
    // a workspace in a loop keeps nothing: the total is flat and each workspace scope is at zero.
    if (!os::mem::enabled()) {
        std::printf("    (built without ENP_MEMORY_ACCOUNTING: skipped)\n");
        CHECK(true);
        return;
    }
    {
        os::mem::Scope scope("test-scope");
        std::vector<char> block(100000);
        // Used, so the allocation stays: libc++ allocates through __builtin_operator_new, and
        // clang -O3 removes a vector whose memory is never looked at.
        char* volatile keep = block.data();
        (void)keep;
        CHECK(os::mem::liveBytes("test-scope") >= 100000);
    }
    CHECK_EQ(os::mem::liveBytes("test-scope"), std::uint64_t(0)); // freed outside the scope: still attributed to it

    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-memory-cycles";
    os::fs::createDirectories(root + "/src");
    os::fs::createDirectories(root + "/docs");
    std::string readme = "# Memory\n\nA project to open and close.\n\n";
    for (int i = 0; i < 100; ++i)
        readme += "Paragraph " + std::to_string(i) + " with **bold**, `code` and a [link](https://example.com).\n\n";
    files.write(root + "/README.md", readme, nullptr);
    files.write(root + "/docs/guide.md", "# Guide\n\n- one\n- two\n", nullptr);
    for (int i = 0; i < 20; ++i) {
        std::string text = "// file " + std::to_string(i) + "\n#include \"other.h\"\n";
        for (int l = 0; l < 100; ++l)
            text += "int value" + std::to_string(l) + " = " + std::to_string(l) + "; // comment\n";
        files.write(root + "/src/f" + std::to_string(i) + ".cpp", text, nullptr);
    }
    viewmodel::NotepadViewModel vm(nullptr, &files);
    auto cycle = [&]() {
        CHECK(vm.openFolder(root));
        vm.openFile("docs/guide.md");
        vm.openFile("src/f3.cpp");
        vm.openFile("src/f7.cpp");
        // Several carets: every occurrence selected and edited (their undo steps hold carets too).
        vm.setCaret({0, 0}, false);
        vm.execute(CommandId::AddNextOccurrence);
        vm.execute(CommandId::SelectAllOccurrences);
        vm.insertText("z");
        vm.execute(CommandId::AddCursorBelow);
        vm.paste("a\nb");
        vm.layers().scope.set(viewmodel::LayerScope::Project);
        (void)vm.layers().nodes();
        (void)vm.logicalMap().map();
        vm.findInFiles().query = "value1";
        vm.findInFiles().searchNow();
        CHECK(vm.findInFiles().hitCount() > 0);
        // IntelliSense: the index, a list of suggestions, a context for a provider, a definition.
        vm.intellisense().buildIndexNow();
        CHECK(vm.intellisense().index().size() >= 2000);
        vm.setCaret({2, 0}, false);
        vm.insertText("val");
        vm.intellisense().trigger();
        CHECK(vm.intellisense().active() && !vm.intellisense().items().empty());
        CHECK(!vm.intellisense().contextFor("value", "src/f7.cpp", "int x = value3;").empty());
        vm.setCaret({3, 6}, false);
        vm.intellisense().goToDefinition();
        vm.intellisense().goBack();
        os::dispatch::pump();
        vm.closeFolder();
        os::dispatch::pump();
        CHECK_EQ(os::mem::liveBytes("intellisense"), std::uint64_t(0)); // every byte of it, every cycle
    };
    // Measuring must not move the needle: snapshots come from the accounting's static pool and
    // are copied into plain records here; nothing on this path allocates.
    using os::mem::Snapshot;
    // A close gives everything back: after the second close and after the eighth, the total and
    // every scope are byte-for-byte identical (the first cycle may fill process-lifetime tables).
    cycle();
    cycle();
    const Snapshot after2 = os::mem::snapshot();
    for (int i = 0; i < 6; ++i)
        cycle();
    const Snapshot after8 = os::mem::snapshot();
    for (int i = 0; i < after8.count; ++i) {
        const std::uint64_t before = i < after2.count ? after2.scopes[i].liveBytes : 0;
        if (after8.scopes[i].liveBytes != before)
            std::printf("    scope %s: %llu bytes after cycle 2, %llu after cycle 8\n", after8.scopes[i].name, (unsigned long long)before,
                        (unsigned long long)after8.scopes[i].liveBytes);
        CHECK(after8.scopes[i].liveBytes == before);
    }
    CHECK_EQ(after8.total.liveBytes, after2.total.liveBytes);
    CHECK_EQ(after8.total.liveBlocks, after2.total.liveBlocks);
}

// ---- IntelliSense -------------------------------------------------------------------------------

namespace {

const model::Symbol* findSymbol(const std::vector<model::Symbol>& symbols, const std::string& name, model::SymbolKind kind)
{
    for (const model::Symbol& s : symbols)
        if (s.name == name && s.kind == kind)
            return &s;
    return nullptr;
}

bool hasSymbol(const std::vector<model::Symbol>& symbols, const std::string& name)
{
    for (const model::Symbol& s : symbols)
        if (s.name == name)
            return true;
    return false;
}

// Types `text` into the active document one character at a time, as the editor does.
void typeText(viewmodel::NotepadViewModel& vm, const std::string& text)
{
    for (char c : text) {
        const std::string one(1, c);
        vm.insertText(one);
        vm.intellisense().typed(one);
        vm.intellisense().sync();
    }
}

int itemIndex(const viewmodel::IntellisenseViewModel& is, const std::string& label)
{
    for (size_t i = 0; i < is.items().size(); ++i)
        if (is.items()[i].label == label)
            return int(i);
    return -1;
}

// A small C++ project: a class declared in a header and defined in a source file.
std::string writeIntellisenseProject(services::FileService& files, const std::string& name)
{
    const std::string root = scratchDirectory() + "/" + name;
    os::fs::deletePath(root);
    os::fs::createDirectories(root + "/src");
    files.write(root + "/src/widget.h",
                "#pragma once\n"
                "namespace ui {\n"
                "class Widget {\n"
                "public:\n"
                "    void resize(int width);\n"
                "    int widthOf() const;\n"
                "    static constexpr int kMaxWidth = 100;\n"
                "};\n"
                "} // namespace ui\n", nullptr);
    files.write(root + "/src/widget.cpp",
                "#include \"widget.h\"\n"
                "void ui::Widget::resize(int width) { (void)width; }\n"
                "int ui::Widget::widthOf() const { return 0; }\n", nullptr);
    files.write(root + "/src/main.cpp",
                "#include \"widget.h\"\n"
                "int main()\n"
                "{\n"
                "    ui::Widget w;\n"
                "    return 0;\n"
                "}\n", nullptr);
    return root;
}

} // namespace

TEST(symbols_are_found_in_each_language)
{
    using K = model::SymbolKind;
    std::vector<model::Symbol> cpp;
    model::extractSymbols("#define LIMIT 4\n"
                          "namespace app {\n"
                          "// class Commented {};\n"
                          "const char* text = \"struct InString {\";\n"
                          "class ENP_API Widget final : public Base<int, char> {\n"
                          "public:\n"
                          "    explicit Widget(int size);\n"
                          "    int size() const { return size_; }\n"
                          "    static constexpr int kLimit = 4;\n"
                          "private:\n"
                          "    std::vector<int> items_, more_;\n"
                          "};\n"
                          "enum class Color : uint8_t { Red, Green = 2, Blue };\n"
                          "template <typename T>\n"
                          "T maxOf(T a, T b)\n"
                          "{\n"
                          "    int local = 3;\n"
                          "    return a < b ? b : a;\n"
                          "}\n"
                          "void Widget::resize(int n) { items_.resize(n); }\n"
                          "TEST(macro_call_is_not_a_function) {}\n"
                          "typedef struct { int a; } Pair;\n"
                          "using Name = std::string;\n"
                          "class Forward;\n"
                          "}\n",
                          model::Language::Cpp, 7, cpp);
    CHECK(findSymbol(cpp, "LIMIT", K::Macro));
    CHECK(findSymbol(cpp, "app", K::Namespace));
    const model::Symbol* widget = findSymbol(cpp, "Widget", K::Type);
    CHECK(widget && widget->container == "app" && widget->line == 4 && widget->column == 14 && widget->file == 7);
    CHECK(widget && widget->detail.rfind("class ENP_API Widget", 0) == 0);
    const model::Symbol* size = findSymbol(cpp, "size", K::Function);
    CHECK(size && size->container == "Widget" && size->definition);
    CHECK(findSymbol(cpp, "kLimit", K::Constant));
    CHECK(findSymbol(cpp, "items_", K::Variable) && findSymbol(cpp, "more_", K::Variable));
    const model::Symbol* green = findSymbol(cpp, "Green", K::Constant);
    CHECK(green && green->container == "Color");
    CHECK(findSymbol(cpp, "maxOf", K::Function));
    const model::Symbol* resize = findSymbol(cpp, "resize", K::Function);
    CHECK(resize && resize->container == "Widget" && resize->definition);
    CHECK(findSymbol(cpp, "Pair", K::Type) && findSymbol(cpp, "Name", K::Type));
    // Not declarations: comments, strings, locals, macro calls, forward declarations.
    CHECK(!hasSymbol(cpp, "Commented") && !hasSymbol(cpp, "InString") && !hasSymbol(cpp, "local"));
    CHECK(!hasSymbol(cpp, "TEST") && !hasSymbol(cpp, "Forward"));

    std::vector<model::Symbol> py;
    model::extractSymbols("MAX = 3\n"
                          "class Store(Base):\n"
                          "    \"\"\"class Fake in a docstring\"\"\"\n"
                          "    count: int = 0\n"
                          "    def load(self, url,\n"
                          "             retries):\n"
                          "        inner = 1\n"
                          "        def helper(): pass\n"
                          "async def main():\n"
                          "    pass\n",
                          model::Language::Python, 0, py);
    CHECK(findSymbol(py, "MAX", K::Constant) && findSymbol(py, "Store", K::Type));
    const model::Symbol* load = findSymbol(py, "load", K::Function);
    CHECK(load && load->container == "Store");
    CHECK(findSymbol(py, "count", K::Variable) && findSymbol(py, "main", K::Function));
    CHECK(!hasSymbol(py, "Fake") && !hasSymbol(py, "inner") && !hasSymbol(py, "helper") && !hasSymbol(py, "retries"));

    std::vector<model::Symbol> ts;
    model::extractSymbols("export const API_URL = \"x\";\n"
                          "const add = (a, b) => a + b;\n"
                          "export default class Store extends Base {\n"
                          "  items = [];\n"
                          "  async load(url) { const r = 1; }\n"
                          "}\n"
                          "function helper(a) { let z = 1; }\n"
                          "interface Shape { area(): number; }\n"
                          "type Id = string;\n",
                          model::Language::TypeScript, 0, ts);
    CHECK(findSymbol(ts, "API_URL", K::Constant) && findSymbol(ts, "add", K::Function));
    CHECK(findSymbol(ts, "Store", K::Type) && findSymbol(ts, "items", K::Variable) && findSymbol(ts, "load", K::Function));
    CHECK(findSymbol(ts, "helper", K::Function) && findSymbol(ts, "area", K::Function) && findSymbol(ts, "Id", K::Type));
    CHECK(!hasSymbol(ts, "Base") && !hasSymbol(ts, "r") && !hasSymbol(ts, "z"));

    std::vector<model::Symbol> rust;
    model::extractSymbols("pub struct Point { x: f64 }\n"
                          "impl Point {\n"
                          "    pub fn new() -> Self { Point { x: 0.0 } }\n"
                          "}\n"
                          "pub(crate) enum Shape { Circle(f64), Square }\n"
                          "const LIMIT: usize = 3;\n",
                          model::Language::Rust, 0, rust);
    const model::Symbol* make = findSymbol(rust, "new", K::Function);
    CHECK(make && make->container == "Point");
    CHECK(findSymbol(rust, "Shape", K::Type) && findSymbol(rust, "Square", K::Constant) && findSymbol(rust, "LIMIT", K::Constant));

    std::vector<model::Symbol> go;
    model::extractSymbols("package main\ntype Server struct {\n\taddr string\n}\nfunc (s *Server) Start() error {\n\treturn nil\n}\n",
                          model::Language::Go, 0, go);
    const model::Symbol* start = findSymbol(go, "Start", K::Function);
    CHECK(findSymbol(go, "Server", K::Type) && start && start->container == "Server");

    std::vector<model::Symbol> java;
    model::extractSymbols("public class App {\n    public static void main(String[] args) {\n        run();\n    }\n}\n",
                          model::Language::Java, 0, java);
    CHECK(findSymbol(java, "App", K::Type) && findSymbol(java, "main", K::Function) && !hasSymbol(java, "run"));

    std::vector<model::Symbol> lua;
    model::extractSymbols("local M = {}\nfunction M.greet(name) return name end\n", model::Language::Lua, 0, lua);
    const model::Symbol* greet = findSymbol(lua, "greet", K::Function);
    CHECK(greet && greet->container == "M");

    std::vector<model::Symbol> sh;
    model::extractSymbols("NAME=x\nbuild() {\n  echo $#\n}\n", model::Language::Shell, 0, sh);
    CHECK(findSymbol(sh, "build", K::Function) && findSymbol(sh, "NAME", K::Constant));

    CHECK(!model::canExtractSymbols(model::Language::Markdown));
}

TEST(symbol_matching_prefers_prefixes_and_word_starts)
{
    CHECK(model::matchScore("get", "getWidth") > model::matchScore("Get", "getWidth")); // exact case first
    CHECK(model::matchScore("Get", "getWidth") > model::matchScore("gw", "getWidth"));  // prefixes before humps
    CHECK(model::matchScore("gw", "getWidth") > 0);
    CHECK(model::matchScore("gw", "getWidth") > model::matchScore("gt", "getWidth")); // word starts before letters inside
    CHECK_EQ(model::matchScore("xyz", "getWidth"), -1);
    CHECK(model::matchScore("w", "getWidth") > 0);    // "W" starts a part of the name
    CHECK_EQ(model::matchScore("i", "getWidth"), -1); // the first letter must start a part
    CHECK(model::matchScore("rs", "resize_all") > 0);
    CHECK(model::matchScore("get", "get") > model::matchScore("get", "getWidth")); // shorter first
    CHECK_EQ(model::matchScore("", "anything"), 0);

    const std::pair<int, int> around = model::identifierAround("  w.resize(3);", 8);
    CHECK(around.first == 4 && around.second == 10);
    const std::pair<int, int> atEnd = model::identifierAround("value", 5);
    CHECK(atEnd.first == 0 && atEnd.second == 5);
    const std::pair<int, int> number = model::identifierAround("x = 42", 6);
    CHECK(number.first == number.second); // a number is not a name
}

TEST(symbol_index_finds_names_and_replaces_saved_files)
{
    model::SymbolIndex index;
    std::vector<model::Symbol> symbols;
    model::extractSymbols("void beta();\nvoid alpha();\n", model::Language::Cpp, int(index.fileCount()), symbols);
    index.addFile("a.h");
    index.add(symbols);
    model::extractSymbols("void alpha() {}\n", model::Language::Cpp, int(index.fileCount()), symbols);
    index.addFile("a.cpp");
    index.add(symbols);
    index.finish();
    CHECK_EQ(index.size(), size_t(3));
    CHECK(index.at(0).name == "alpha" && index.at(2).name == "beta");
    CHECK(index.at(2).detail == "void beta();" && index.at(2).kind == model::SymbolKind::Function && !index.at(2).definition);
    auto [first, last] = index.named("alpha");
    CHECK_EQ(last - first, size_t(2));
    CHECK(index.path(index.at(first).file) == "a.h");
    CHECK(index.path(1) == "a.cpp" && index.path(2).empty() && index.fileIndex("a.cpp") == 1);
    CHECK_EQ(index.named("gamma").first, index.named("gamma").second);

    // Saving a file replaces its names; saving it many times keeps the index its size.
    model::extractSymbols("void gamma() {}\n", model::Language::Cpp, -1, symbols);
    index.replaceFile("a.cpp", symbols);
    const size_t bytes = index.bytes();
    for (int i = 0; i < 50; ++i) {
        model::extractSymbols("void gamma() {}\n", model::Language::Cpp, -1, symbols);
        index.replaceFile("a.cpp", symbols);
    }
    CHECK(index.bytes() <= bytes);
    CHECK_EQ(index.named("alpha").second - index.named("alpha").first, size_t(1));
    CHECK_EQ(index.named("gamma").second - index.named("gamma").first, size_t(1));
    CHECK(index.path(index.at(index.named("gamma").first).file) == "a.cpp");
    index.release();
    CHECK(index.empty() && index.fileCount() == 0 && index.bytes() == 0);
}

TEST(intellisense_completes_names_of_the_workspace_document_and_language)
{
    services::FileService files;
    const std::string root = writeIntellisenseProject(files, "enp-test-intellisense-complete");
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    viewmodel::IntellisenseViewModel& is = vm.intellisense();
    is.buildIndexNow();
    CHECK(is.indexReady() && is.index().size() >= 6);
    vm.openFile("src/main.cpp");
    vm.setCaret({3, 17}, false); // end of "    ui::Widget w;"
    vm.newline();

    // Members after ".": the declarations with a container, the typed part narrows them.
    typeText(vm, "w.");
    CHECK(is.active() && itemIndex(is, "resize") >= 0 && itemIndex(is, "widthOf") >= 0 && itemIndex(is, "Widget") < 0);
    typeText(vm, "re");
    CHECK(is.active() && !is.items().empty() && is.items()[0].label == "resize");
    CHECK(is.items()[0].source == viewmodel::IntellisenseViewModel::Source::Workspace);
    CHECK(is.items()[0].location.find("src/widget") == 0);
    CHECK(is.accept());
    CHECK(!is.active());
    CHECK(vm.activeDocument()->buffer.line(4) == "    w.resize");
    vm.execute(viewmodel::CommandId::Undo);
    CHECK(vm.activeDocument()->buffer.line(4) == "    w.re"); // one undoable insertion

    // Names by themselves after two letters: the workspace's, the language's and the document's.
    vm.setCaret({4, int(vm.activeDocument()->buffer.line(4).size())}, false);
    vm.newline();
    typeText(vm, "W");
    CHECK(!is.active()); // one letter is not enough
    typeText(vm, "i");
    CHECK(is.active() && !is.items().empty() && is.items()[0].label == "Widget");
    CHECK(is.items()[0].kind == model::SymbolKind::Type);
    is.dismiss();
    CHECK(is.items().empty() && is.prefix().empty());
    typeText(vm, ";\n    ret");
    CHECK(is.active() && itemIndex(is, "return") >= 0);
    CHECK(is.items()[size_t(itemIndex(is, "return"))].source == viewmodel::IntellisenseViewModel::Source::Language);
    // The caret leaving the word closes the list.
    vm.setCaret({0, 0}, false);
    is.sync();
    CHECK(!is.active());

    // "ui::" lists what the namespace declares.
    vm.setCaret({5, int(vm.activeDocument()->buffer.line(5).size())}, false);
    vm.newline();
    typeText(vm, "ui::");
    CHECK(is.active() && itemIndex(is, "Widget") >= 0 && itemIndex(is, "return") < 0);
    is.dismiss();

    // Ctrl+Space shows suggestions without anything typed; a comment never opens them by itself.
    vm.newline();
    is.trigger();
    CHECK(is.active() && is.items().size() > 5);
    is.move(+2);
    CHECK_EQ(is.selected(), 2);
    is.move(-10);
    CHECK_EQ(is.selected(), 0);
    is.dismiss();
    typeText(vm, "// wid");
    CHECK(!is.active());
    // Turned off: nothing appears while typing, Ctrl+Space still works.
    vm.newline();
    vm.execute(viewmodel::CommandId::ToggleAutoComplete);
    CHECK(!is.autoComplete.get() && !vm.isChecked(viewmodel::CommandId::ToggleAutoComplete));
    typeText(vm, "wid");
    CHECK(!is.active());
    vm.execute(viewmodel::CommandId::TriggerCompletion);
    CHECK(is.active() && itemIndex(is, "widthOf") >= 0);
    vm.execute(viewmodel::CommandId::ToggleAutoComplete);
    CHECK(is.autoComplete.get());

    // Closing the document closes its list; closing the folder drops the index.
    vm.closeDocument(vm.activeIndex.get());
    CHECK(!is.active());
    vm.closeFolder();
    CHECK(!is.indexReady() && is.index().empty());
}

TEST(intellisense_goes_to_definitions_in_the_file_and_the_workspace)
{
    services::FileService files;
    const std::string root = writeIntellisenseProject(files, "enp-test-intellisense-definition");
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    vm.intellisense().buildIndexNow();
    vm.openFile("src/main.cpp");
    vm.setCaret({3, 9}, false); // on "Widget" in "    ui::Widget w;"
    vm.execute(viewmodel::CommandId::GoToDefinition);
    CHECK(vm.activeDocument()->path == "src/widget.h");
    CHECK(vm.activeDocument()->caret.line == 2 && vm.activeDocument()->caret.column == 6);
    // From the declaration in the header to the definition in the source file.
    vm.setCaret({4, 11}, false); // on "resize"
    CHECK(vm.intellisense().goToDefinition());
    CHECK(vm.activeDocument()->path == "src/widget.cpp");
    CHECK_EQ(vm.activeDocument()->caret.line, 1);
    // A name declared further down the same document: found in its current (unsaved) text.
    vm.setCaret({2, 0}, false);
    vm.insertText("int later = helper();\nint helper() { return 1; }\n");
    vm.setCaret({2, 14}, false); // on "helper"
    CHECK(vm.intellisense().goToDefinition());
    CHECK(vm.activeDocument()->path == "src/widget.cpp" && vm.activeDocument()->caret.line == 3);
    vm.setCaret({2, 3}, false); // on "int": nothing to go to
    CHECK(!vm.intellisense().goToDefinition());
    // The hover tooltip's lookup: the definition before the declaration.
    const std::optional<model::SymbolView> hovered = vm.intellisense().lookup("resize", "src/widget.h");
    CHECK(hovered && hovered->definition && vm.intellisense().index().path(hovered->file) == "src/widget.cpp");
    CHECK(!vm.intellisense().lookup("nothing", ""));
    // A save updates the index.
    model::Document* doc = vm.activeDocument();
    doc->buffer.setText("int renamedFunction() { return 2; }\n");
    doc->modified = true;
    vm.execute(viewmodel::CommandId::Save);
    CHECK(vm.intellisense().lookup("renamedFunction", "").has_value());
    CHECK(vm.intellisense().lookup("widthOf", "").has_value()); // still declared in the header
    vm.closeFolder();
}

TEST(intellisense_suggests_with_one_caret_only)
{
    // Each caret has its own word: one list cannot complete them all, so with several carets
    // (Alt+click, Ctrl+D, Ctrl+Alt+Down) it does not open, and an open one closes.
    viewmodel::NotepadViewModel vm;
    vm.newDocument();
    vm.activeDocument()->language = model::Language::Cpp;
    vm.insertText("int widthOne = 1;\nint widthTwo = 2;\nint a = wi;\nint b = wi;\n");
    viewmodel::IntellisenseViewModel& is = vm.intellisense();
    vm.setCaret({2, 10}, false);
    is.trigger();
    CHECK(is.active() && itemIndex(is, "widthOne") >= 0);
    vm.execute(viewmodel::CommandId::AddCursorBelow);
    CHECK_EQ(vm.cursorCount(), 2);
    is.sync();
    CHECK(!is.active());
    is.trigger();
    CHECK(!is.active());
    typeText(vm, "d");
    CHECK(!is.active());
    CHECK(vm.activeDocument()->buffer.line(2) == "int a = wid;" && vm.activeDocument()->buffer.line(3) == "int b = wid;");
    vm.clearExtraCursors();
    is.trigger();
    CHECK(is.active());
}

TEST(intellisense_goes_back_and_forward_through_its_jumps)
{
    using C = viewmodel::CommandId;
    services::FileService files;
    const std::string root = writeIntellisenseProject(files, "enp-test-intellisense-history");
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    viewmodel::IntellisenseViewModel& is = vm.intellisense();
    is.buildIndexNow();
    auto at = [&](const char* path, int line, int column) {
        const model::Document* doc = vm.activeDocument();
        return doc && doc->path == path && doc->caret.line == line && doc->caret.column == column;
    };
    CHECK(!vm.command(C::NavigateBack).canExecute() && !vm.command(C::NavigateForward).canExecute());
    vm.openFile("src/main.cpp");
    vm.setCaret({3, 9}, false); // "Widget"
    CHECK(is.goToDefinition());
    CHECK(at("src/widget.h", 2, 6));
    vm.setCaret({4, 11}, false); // "resize"
    CHECK(is.goToDefinition());
    CHECK(at("src/widget.cpp", 1, 17));
    CHECK_EQ(is.historySize(), size_t(2));
    CHECK(vm.command(C::NavigateBack).canExecute());

    vm.execute(C::NavigateBack);
    CHECK(at("src/widget.h", 4, 11));
    CHECK(is.goBack());
    CHECK(at("src/main.cpp", 3, 9));
    CHECK(!is.canGoBack() && !is.goBack());
    CHECK(at("src/main.cpp", 3, 9)); // nothing to go back to: it stays
    vm.execute(C::NavigateForward);
    CHECK(at("src/widget.h", 4, 11));
    // A document closed since is opened again from its path.
    vm.closeDocument(vm.documentIndex("src/widget.cpp"));
    CHECK(is.goForward());
    CHECK(at("src/widget.cpp", 1, 17));
    CHECK(!is.canGoForward());
    // A new jump drops the way forward.
    CHECK(is.goBack());
    CHECK(is.canGoForward());
    vm.setCaret({5, 9}, false); // "widthOf"
    CHECK(is.goToDefinition());
    CHECK(at("src/widget.cpp", 2, 16));
    CHECK(!is.canGoForward());
    // An untitled document that closed is skipped on the way back.
    vm.newDocument();
    vm.activeDocument()->language = model::Language::Cpp;
    vm.insertText("int later = helper();\nint helper() { return 1; }\n");
    vm.setCaret({0, 14}, false);
    CHECK(is.goToDefinition()); // in the untitled document itself
    CHECK_EQ(vm.activeDocument()->caret.line, 1);
    vm.closeDocument(vm.activeIndex.get());
    vm.openFile("src/main.cpp");
    vm.setCaret({3, 9}, false);
    CHECK(is.goToDefinition());
    CHECK(is.goBack());
    CHECK(at("src/main.cpp", 3, 9));
    CHECK(is.goBack()); // skips both places in the closed untitled document
    CHECK(at("src/widget.h", 5, 9));
    // At most kMaxHistory places are kept each way.
    for (int i = 0; i < 70; ++i) {
        vm.openFile("src/main.cpp");
        vm.setCaret({3, 9}, false);
        is.goToDefinition();
    }
    CHECK_EQ(is.historySize(), viewmodel::IntellisenseViewModel::kMaxHistory);
    // The folder's places go with it.
    vm.closeFolder();
    CHECK_EQ(is.historySize(), size_t(0));
}

TEST(intellisense_chooses_context_for_an_ai_provider_and_drops_late_answers)
{
    // AI completion plugs in as a provider: it is asked with the code around the caret and the
    // declarations IntelliSense chose, answers later, and is shown first. A late answer to an
    // older request, or after the list closed, is dropped.
    using IS = viewmodel::IntellisenseViewModel;
    services::FileService files;
    const std::string root = writeIntellisenseProject(files, "enp-test-intellisense-provider");
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    IS& is = vm.intellisense();
    is.buildIndexNow();

    // The context alone: what is typed, what the code nearby uses, what the file declares.
    const std::vector<IS::ContextSnippet> context = is.contextFor("res", "src/widget.cpp", "ui::Widget w;\nw.");
    auto reasonOf = [&](const std::string& name) {
        for (const IS::ContextSnippet& c : context)
            if (c.name == name)
                return int(c.reason);
        return -1;
    };
    CHECK(!context.empty() && context[0].name == "resize" && context[0].reason == IS::ContextReason::Typed);
    CHECK_EQ(reasonOf("Widget"), int(IS::ContextReason::UsedNearby));
    CHECK_EQ(reasonOf("widthOf"), int(IS::ContextReason::SameFile));
    const std::string text = IS::formatContext(context);
    CHECK(text.find("function resize in Widget (src/widget") != std::string::npos);
    CHECK(text.find("used nearby") != std::string::npos);
    IS::ContextBudget small;
    small.maxSnippets = 2;
    CHECK_EQ(is.contextFor("res", "src/widget.cpp", "ui::Widget w;", small).size(), size_t(2));

    std::vector<IS::Request> requests;
    is.setProvider([&](const IS::Request& r) { requests.push_back(r); });
    vm.openFile("src/main.cpp");
    vm.setCaret({3, 17}, false);
    vm.newline();
    typeText(vm, "w.re");
    CHECK(is.active() && !requests.empty());
    const IS::Request& last = requests.back();
    CHECK(last.serial == is.requestSerial() && last.prefix == "re" && last.path == "src/main.cpp");
    CHECK(last.before.size() >= 4 && last.before.compare(last.before.size() - 4, 4, "w.re") == 0);
    CHECK(last.after.find("return 0;") != std::string::npos);
    CHECK(!last.context.empty() && last.context[0].name == "resize");

    // An answer to an older request is dropped; the current one is shown first and inserted whole.
    const int older = requests.front().serial;
    is.provide(older, {IS::Item{"stale", "stale()", "", "", model::SymbolKind::Function, IS::Source::Provider, 0}});
    CHECK(itemIndex(is, "stale") < 0);
    IS::Item answer;
    answer.insertText = "resize(800);\nw.widthOf();";
    is.provide(last.serial, {answer});
    CHECK(!is.items().empty() && is.items()[0].source == IS::Source::Provider && is.items()[0].label == "resize(800);");
    CHECK(itemIndex(is, "resize") > 0); // the names stay below
    CHECK(is.accept());
    CHECK(vm.activeDocument()->buffer.line(4) == "    w.resize(800);" && vm.activeDocument()->buffer.line(5) == "w.widthOf();");
    // After the list closed, an answer is dropped.
    const int closed = is.requestSerial();
    is.provide(closed, {answer});
    CHECK(!is.active() && is.items().empty());
    is.setProvider({});
    CHECK(!is.hasProvider());
    vm.closeFolder();
}

TEST(closing_a_folder_stops_the_symbol_index_build)
{
    // The index is built on a background thread: closing the folder cancels it, and its late
    // result never lands (in the next folder or anywhere): every byte of it is freed.
    if (!os::mem::enabled()) {
        CHECK(true);
        return;
    }
    services::FileService files;
    const std::string a = scratchDirectory() + "/enp-test-index-a";
    const std::string b = scratchDirectory() + "/enp-test-index-b";
    os::fs::deletePath(a);
    os::fs::deletePath(b);
    os::fs::createDirectories(a + "/src");
    os::fs::createDirectories(b + "/src");
    for (int i = 0; i < 200; ++i) {
        std::string text;
        for (int l = 0; l < 50; ++l)
            text += "int folderA_" + std::to_string(i) + "_" + std::to_string(l) + "(int x) { return x; }\n";
        files.write(a + "/src/a" + std::to_string(i) + ".cpp", text, nullptr);
    }
    files.write(b + "/src/b.cpp", "int onlyInB() { return 1; }\n", nullptr);
    auto waitForBuilds = []() {
        for (int i = 0; i < 1000 && viewmodel::IntellisenseViewModel::runningBuilds() > 0; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(50)); // the finished thread lets go of its task
        os::dispatch::pump();
    };
    waitForBuilds();
    const std::uint64_t before = os::mem::liveBytes("intellisense");
    {
        viewmodel::NotepadViewModel vm(nullptr, &files);
        CHECK(vm.openFolder(a));
        vm.intellisense().ensureIndex();
        CHECK(vm.intellisense().indexing.get());
        CHECK(vm.openFolder(b)); // while it reads folder a
        CHECK(!vm.intellisense().indexing.get());
        waitForBuilds();
        CHECK(!vm.intellisense().indexReady()); // folder a's late result was dropped
        vm.intellisense().ensureIndex();
        waitForBuilds();
        CHECK(vm.intellisense().indexReady());
        CHECK(vm.intellisense().lookup("onlyInB", "").has_value());
        CHECK(!vm.intellisense().lookup("folderA_0_0", ""));
        vm.closeFolder();
        CHECK(!vm.intellisense().indexReady());
        CHECK_EQ(os::mem::liveBytes("intellisense"), before);
        // Destroyed while a build runs: the build stops and its result goes with its task.
        CHECK(vm.openFolder(a));
        vm.intellisense().ensureIndex();
    }
    waitForBuilds();
    CHECK_EQ(os::mem::liveBytes("intellisense"), before);
    CHECK_EQ(viewmodel::IntellisenseViewModel::runningBuilds(), 0);
}

TEST(measurement_pool_is_a_fixed_first_fit_manager)
{
    // Measuring must not use the heap it measures: snapshots and traces live in a fixed pool with
    // its own manager. Blocks come 16-byte aligned, free blocks merge, and exhaustion is a nullptr.
    namespace pool = os::mem::pool;
    const pool::Usage before = pool::usage();
    CHECK(before.capacity >= (1u << 20));
    void* a = pool::allocate(100);
    void* b = pool::allocate(1000);
    void* c = pool::allocate(10);
    CHECK(a && b && c);
    CHECK(reinterpret_cast<std::uintptr_t>(a) % 16 == 0 && reinterpret_cast<std::uintptr_t>(b) % 16 == 0);
    CHECK_EQ(pool::usage().blocks, before.blocks + 3);
    CHECK(pool::usage().used >= before.used + 1110);
    std::memset(b, 0xAB, 1000); // ours to use
    pool::release(b);
    pool::release(a); // a and b merge: a block of at least 1100 bytes fits where they were
    void* d = pool::allocate(1100);
    CHECK(d == a);
    pool::release(d);
    pool::release(c);
    CHECK_EQ(pool::usage().blocks, before.blocks);
    CHECK_EQ(pool::usage().used, before.used);
    CHECK(pool::allocate(before.capacity) == nullptr); // no heap fallback
    pool::release(nullptr);                            // harmless
    const os::mem::Snapshot& s1 = os::mem::snapshot(); // snapshots come from the pool, not the heap
    const os::mem::Stats heapBefore = os::mem::stats();
    for (int i = 0; i < 3 * os::mem::kSnapshotPool; ++i)
        (void)os::mem::snapshot();
    CHECK_EQ(os::mem::stats().liveBlocks, heapBefore.liveBlocks);
    CHECK(s1.count >= 1);
}

TEST(closing_a_folder_stops_the_agent_dissection_of_it)
{
    // A late answer about a closed folder must not land in the next one (its map, its cache).
    struct SlowAgent : interfaces::IAgentCliService {
        std::function<void(bool, const std::string&)> pending;
        int cancels = 0;
        bool available() const override { return true; }
        std::string command() const override { return "fake-agent"; }
        void run(const std::string&, const std::string&, std::function<void(bool, const std::string&)> done) override { pending = std::move(done); }
        void cancel() override { ++cancels; }
    } agent;
    services::FileService files;
    const std::string a = scratchDirectory() + "/enp-test-dissect-a";
    const std::string b = scratchDirectory() + "/enp-test-dissect-b";
    os::fs::createDirectories(a + "/src");
    os::fs::createDirectories(b + "/src");
    files.write(a + "/src/x.cpp", "int a();\n", nullptr);
    files.write(b + "/src/y.cpp", "int b();\n", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files, &agent);
    CHECK(vm.openFolder(a));
    vm.logicalMap().dissectWithAgent();
    CHECK(vm.logicalMap().analyzing.get() && agent.pending);
    CHECK(vm.openFolder(b));
    CHECK(!vm.logicalMap().analyzing.get());
    CHECK_EQ(agent.cancels, 1);
    const size_t nodes = vm.logicalMap().map().nodes.size();
    if (agent.pending)
        agent.pending(true, "{\"features\": [{\"name\": \"Stale\", \"summary\": \"from folder a\"}]}");
    CHECK_EQ(vm.logicalMap().map().nodes.size(), nodes); // ignored
    CHECK(!os::fs::exists(b + "/.easynotepad/logical-map.json"));
}

TEST(refreshing_the_map_writes_nothing_into_a_folder_without_a_cache)
{
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-refresh-map";
    os::fs::deletePath(root); // a previous run leaves the cache of the second half behind
    os::fs::createDirectories(root + "/src");
    files.write(root + "/src/a.cpp", "int a();\n", nullptr);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    CHECK(vm.openFolder(root));
    vm.logicalMap().refresh();
    CHECK(!os::fs::exists(root + "/.easynotepad"));
    // With an agent's cache, the refresh clears it.
    os::fs::createDirectories(root + "/.easynotepad");
    files.write(root + "/.easynotepad/logical-map.json", "{\"source\": \"agent\"}", nullptr);
    vm.logicalMap().refresh();
    CHECK(files.read(root + "/.easynotepad/logical-map.json").value_or("x").empty());
}

TEST(new_terminals_start_in_the_open_folder_else_at_home)
{
    services::FileService files;
    const std::string root = scratchDirectory() + "/enp-test-terminal-folder";
    os::fs::createDirectories(root);
    viewmodel::NotepadViewModel vm(nullptr, &files);
    const std::string home = files.homeDirectory();
    CHECK(!home.empty());
    CHECK_EQ(vm.terminal().workingDirectory(), home); // no folder yet
    CHECK(vm.openFolder(root));
    CHECK_EQ(vm.terminal().workingDirectory(), files.absolute(root));
    vm.closeFolder();
    CHECK_EQ(vm.terminal().workingDirectory(), home);
}

TEST(ribbon_tabs_are_arranged_per_workspace_saved_and_restored)
{
    using viewmodel::RibbonTabSetting;
    using viewmodel::WorkspaceMode;
    services::FileService files;
    const std::string dir = scratchDirectory() + "/enp-test-ribbon";
    const std::string path = dir + "/store.json";
    os::fs::createDirectories(dir);
    std::remove(path.c_str());
    {
        viewmodel::NotepadViewModel vm(nullptr, &files);
        vm.setSettingsPath(path);
        CHECK(vm.ribbonTabs(WorkspaceMode::Code).empty()); // the built-in layout
        const int before = vm.ribbonRevision();
        // Git first, Read and Review hidden, for the Code workspace only.
        vm.setRibbonTabs(WorkspaceMode::Code, {{"Git", true}, {"Home", true}, {"Read", false}, {"Review", false}});
        CHECK(vm.ribbonRevision() > before);
        const int same = vm.ribbonRevision();
        vm.setRibbonTabs(WorkspaceMode::Code, {{"Git", true}, {"Home", true}, {"Read", false}, {"Review", false}});
        CHECK_EQ(vm.ribbonRevision(), same); // nothing changed: no rebuild
        CHECK(vm.ribbonTabs(WorkspaceMode::Agent).empty());
        // Pinned commands keep the order they are given (start from none: some are pinned by default).
        const std::vector<CommandId> defaults = vm.pinnedCommands();
        for (CommandId id : defaults)
            vm.setPinned(id, false);
        vm.setPinned(CommandId::FindInFiles, true);
        vm.setPinned(CommandId::GoToLine, true);
        vm.movePinned(CommandId::GoToLine, -1);
        CHECK(vm.pinnedCommands().size() == 2 && vm.pinnedCommands()[0] == CommandId::GoToLine);
        vm.tick(100.0);
        vm.tick(101.0);
        CHECK(files.read(path)->find("\"tabs\"") != std::string::npos);
    }
    viewmodel::NotepadViewModel vm(nullptr, &files);
    vm.setSettingsPath(path);
    const std::vector<RibbonTabSetting>& code = vm.ribbonTabs(WorkspaceMode::Code);
    CHECK(code.size() == 4 && code[0].name == "Git" && !code[2].visible && code[3].name == "Review" && !code[3].visible);
    CHECK(vm.pinnedCommands().size() == 2 && vm.pinnedCommands()[0] == CommandId::GoToLine);
    vm.restoreRibbonTabs(WorkspaceMode::Code);
    CHECK(vm.ribbonTabs(WorkspaceMode::Code).empty());
    vm.tick(200.0);
    vm.tick(201.0);
    CHECK(files.read(path)->find("\"tabs\"") == std::string::npos); // the default is not written at all
    std::remove(path.c_str());
}

namespace {
model::bin::Image readBinaryFixture(const std::string& name, std::string* bytes = nullptr)
{
    const std::string path = std::string(ENP_SOURCE_DIR) + "/tests/data/binary/" + name;
    const std::string data = os::fs::readFile(path).value_or(std::string());
    if (bytes)
        *bytes = data;
    return model::bin::read(
        [&data](std::uint64_t offset, size_t size, std::string& out) {
            out = offset < data.size() ? data.substr(size_t(offset), size) : std::string();
            return true;
        },
        data.size());
}
const model::bin::Symbol* findSymbol(const model::bin::Image& img, std::string_view shown)
{
    for (const model::bin::Symbol& s : img.symbols)
        if (img.text(s.shown) == shown)
            return &s;
    return nullptr;
}
std::string overviewValue(const model::bin::Image& img, std::string_view key)
{
    for (const model::bin::Property& p : img.overview)
        if (img.text(p.key) == key)
            return std::string(img.text(p.value));
    return {};
}
} // namespace

TEST(names_are_demangled_and_marked_by_scheme)
{
    namespace bin = model::bin;
    bin::Mangling m = bin::Mangling::None;
    CHECK_EQ(bin::demangle("_ZN3foo3barEv", &m), std::string("foo::bar()"));
    CHECK(m == bin::Mangling::Itanium);
    CHECK_EQ(bin::demangle("__ZN3foo3barEv", &m), std::string("foo::bar()")); // the platform's extra underscore
    CHECK_EQ(bin::demangle("_ZN3foo3barEv@@VERS_1", &m), std::string("foo::bar()@@VERS_1")); // an ELF version stays
    CHECK_EQ(bin::demangle("?area@Widget@@QEBAHHH@Z", &m), std::string("public: int __cdecl Widget::area(int, int) const"));
    CHECK(m == bin::Mangling::Microsoft);
    CHECK_EQ(bin::demangle("__imp_?counter@@3HA", &m), std::string("__imp_ int counter"));
    CHECK_EQ(bin::demangle("_RNvC7mycrate3foo", &m), std::string("mycrate::foo"));
    CHECK(m == bin::Mangling::Rust);
    CHECK(!bin::demangle("_ZN4core3fmt5write17h0123456789abcdefE", &m).empty() && m == bin::Mangling::Rust); // legacy Rust
    // C names are not mangled, even when they look a little like it.
    CHECK(bin::demangle("printf", &m).empty() && m == bin::Mangling::None);
    CHECK(bin::demangle("_ZombifyActCtx@4", &m).empty() && m == bin::Mangling::None); // 32-bit Windows C name
    CHECK(bin::demangle("_RTC_CheckEsp", &m).empty() && m == bin::Mangling::None);
    // Mangled, but in a form the demangler does not know: marked, shown as stored.
    CHECK(bin::demangle("_ZGTtdlPv", &m).empty() && m == bin::Mangling::Itanium);
}

TEST(libraries_list_functions_imports_sections_and_characteristics)
{
    namespace bin = model::bin;
    using model::FileKind;
    std::string bytes;
    {   // Linux shared library
        const bin::Image so = readBinaryFixture("libfixture.so", &bytes);
        CHECK(so.error.empty());
        CHECK(so.format == bin::Format::Elf && so.summary == "ELF 64-bit shared library, x86-64");
        CHECK(model::detectFileType("libfixture.so", bytes).format == "ELF shared library");
        CHECK_EQ(overviewValue(so, "SONAME"), std::string("libfixture.so.1"));
        const bin::Symbol* area = findSymbol(so, "sample::Widget::area(int, int) const");
        CHECK(area && area->side == bin::Side::Export && area->kind == bin::SymbolKind::Function && area->demangled &&
              area->mangling == bin::Mangling::Itanium && area->size > 0 && area->offset != bin::kNone);
        const bin::Symbol* counter = findSymbol(so, "sample::counter");
        CHECK(counter && counter->kind == bin::SymbolKind::Data && counter->size == 4 && so.text(counter->section) == ".data");
        const bin::Symbol* plain = findSymbol(so, "plain_add");
        CHECK(plain && plain->mangling == bin::Mangling::None && !plain->demangled);
        // The bytes at the offset are the function's (its file offset is inside .text).
        bool inText = false;
        for (const bin::Section& sec : so.sections)
            if (so.text(sec.name) == ".text")
                inText = plain && plain->offset >= sec.offset && plain->offset < sec.offset + sec.fileSize && sec.execute;
        CHECK(inText);
        const bin::Symbol* tick = findSymbol(so, "tick");
        CHECK(tick && tick->side == bin::Side::Import);
        bool functions = false;
        for (const bin::ListNode& n : so.lists)
            functions = functions || (so.text(n.title) == "Exported functions" && n.count == 3);
        CHECK(functions);
    }
    {   // Windows DLL: exports by name and ordinal, a forwarder, imports and delay-loaded imports
        const bin::Image dll = readBinaryFixture("fixture.dll", &bytes);
        CHECK(dll.error.empty());
        CHECK_EQ(dll.summary, std::string("PE32+ DLL, x86-64"));
        CHECK(model::detectFileType("fixture.dll", bytes).format == "Windows DLL");
        CHECK_EQ(overviewValue(dll, "Export name"), std::string("fixture.dll"));
        const bin::Symbol* area = findSymbol(dll, "public: int __cdecl sample::Widget::area(int, int) const");
        CHECK(area && area->mangling == bin::Mangling::Microsoft && dll.text(area->name) == "?area@Widget@sample@@QEBAHHH@Z" &&
              area->ordinal != bin::kNoIndex && area->address != bin::kNone && area->offset != bin::kNone);
        const bin::Symbol* counter = findSymbol(dll, "int sample::counter");
        CHECK(counter && counter->kind == bin::SymbolKind::Data);
        const bin::Symbol* forwarded = findSymbol(dll, "forwarded");
        CHECK(forwarded && dll.text(forwarded->detail) == "→ NTDLL.RtlGetVersion" && forwarded->address == bin::kNone);
        const bin::Symbol* byOrdinal = findSymbol(dll, "#40");
        CHECK(byOrdinal && byOrdinal->ordinal == 40);
        const bin::Symbol* plain = findSymbol(dll, "plain_add");
        CHECK(plain && plain->size > 0); // from the x64 unwind table
        const bin::Symbol* tick = findSymbol(dll, "GetTickCount");
        CHECK(tick && tick->side == bin::Side::Import && dll.text(dll.libraries[tick->library].name) == "KERNEL32.dll" && !tick->delayed);
        const bin::Symbol* beep = findSymbol(dll, "MessageBeep");
        CHECK(beep && beep->delayed && dll.text(dll.libraries[beep->library].name) == "USER32.dll");
        bool delayedNode = false;
        for (const bin::ListNode& n : dll.lists)
            delayedNode = delayedNode || (dll.text(n.title) == "USER32.dll (delay-loaded)" && n.count == 1);
        CHECK(delayedNode);
        CHECK_EQ(overviewValue(dll, "ASLR (dynamic base)"), std::string("Yes"));
    }
    {   // COFF object
        const bin::Image obj = readBinaryFixture("fixture.obj", &bytes);
        CHECK(obj.format == bin::Format::Coff && obj.error.empty());
        CHECK(model::detectFileType("fixture.obj", bytes).format == "COFF object file");
        const bin::Symbol* imp = findSymbol(obj, "__imp_GetTickCount");
        CHECK(imp && imp->side == bin::Side::Import);
        const bin::Symbol* area = findSymbol(obj, "public: int __cdecl sample::Widget::area(int, int) const");
        CHECK(area && area->side == bin::Side::Export && area->kind == bin::SymbolKind::Function);
    }
    {   // Windows import library (short import entries)
        const bin::Image lib = readBinaryFixture("fixture.lib", &bytes);
        CHECK(lib.format == bin::Format::Archive && lib.summary == "Windows import library");
        CHECK(model::detectFileType("fixture.lib", bytes).format == "static library");
        const bin::Symbol* plain = findSymbol(lib, "plain_add");
        CHECK(plain && lib.text(plain->detail) == "fixture.dll" && plain->member != bin::kNoIndex);
        const bin::Symbol* byOrdinal = findSymbol(lib, "by_ordinal");
        CHECK(byOrdinal && byOrdinal->ordinal == 40);
        CHECK(lib.libraries.size() == 1 && lib.text(lib.libraries[0].name) == "fixture.dll");
    }
    {   // GNU static library
        const bin::Image a = readBinaryFixture("libfixture.a");
        CHECK(a.format == bin::Format::Archive && a.members.size() == 1 && a.text(a.members[0].name) == "fixture.o");
        const bin::Symbol* area = findSymbol(a, "sample::Widget::area(int, int) const");
        CHECK(area && area->member == 0 && area->side == bin::Side::Export);
        bool memberNode = false;
        for (const bin::ListNode& n : a.lists)
            memberNode = memberNode || (n.member == 0 && n.count == a.members[0].symbols && n.count > 0);
        CHECK(memberNode);
    }
}

TEST(damaged_libraries_are_read_without_crashing)
{
    // Every prefix of each fixture, and copies with bytes overwritten: the reader stops at what
    // is missing or wrong and never reads outside the file.
    for (const char* name : {"libfixture.so", "fixture.dll", "fixture.obj", "fixture.lib", "libfixture.a"}) {
        std::string bytes;
        readBinaryFixture(name, &bytes);
        CHECK(!bytes.empty());
        auto readBytes = [](const std::string& data) {
            return model::bin::read(
                [&data](std::uint64_t offset, size_t size, std::string& out) {
                    out = offset < data.size() ? data.substr(size_t(offset), size) : std::string();
                    return true;
                },
                data.size());
        };
        for (size_t n = 0; n < bytes.size(); n += 37)
            readBytes(bytes.substr(0, n));
        std::uint32_t seed = 12345;
        for (int round = 0; round < 300; ++round) {
            std::string copy = bytes;
            for (int k = 0; k < 8; ++k) {
                seed = seed * 1103515245u + 12345u;
                const size_t at = 4 + (seed >> 8) % (copy.size() - 4); // keep the magic, damage the rest
                seed = seed * 1103515245u + 12345u;
                copy[at] = char(seed >> 16);
            }
            readBytes(copy);
        }
    }
}

TEST(source_rules_no_raw_allocation_outside_the_os_layer)
{
    // docs/MEMORY.md rule 1: ownership is a value, a container or a smart pointer. Only src/os
    // (the C API wrappers) may call new, delete, malloc or free.
    std::vector<std::string> violations;
    const std::regex pattern(R"((^|[^A-Za-z0-9_])(new|delete)\s+[A-Za-z_(\[]|(^|[^A-Za-z0-9_])(malloc|calloc|realloc|free)\s*\()");
    auto scan = [&](auto&& self, const std::string& dir) -> void {
        for (const os::fs::DirEntry& e : os::fs::listDirectory(dir)) {
            const std::string path = dir + "/" + e.name;
            if (e.isDirectory) {
                if (e.name != "os")
                    self(self, path);
                continue;
            }
            const size_t dot = e.name.find_last_of('.');
            const std::string ext = dot == std::string::npos ? "" : e.name.substr(dot);
            if (ext != ".cpp" && ext != ".h")
                continue;
            const std::optional<std::string> text = os::fs::readFile(path);
            if (!text)
                continue;
            size_t start = 0;
            int lineNo = 0;
            while (start < text->size()) {
                size_t end = text->find('\n', start);
                if (end == std::string::npos)
                    end = text->size();
                ++lineNo;
                std::string line = text->substr(start, end - start);
                start = end + 1;
                // Drop string literals and line comments: the words matter only in code.
                std::string code;
                bool inString = false;
                for (size_t i = 0; i < line.size(); ++i) {
                    const char c = line[i];
                    if (inString) {
                        if (c == '\\')
                            ++i;
                        else if (c == '"')
                            inString = false;
                        continue;
                    }
                    if (c == '"') {
                        inString = true;
                        continue;
                    }
                    if (c == '/' && i + 1 < line.size() && line[i + 1] == '/')
                        break;
                    code += c;
                }
                const size_t first = code.find_first_not_of(" \t");
                if (first != std::string::npos && (code[first] == '*' || code.compare(first, 2, "/*") == 0))
                    continue; // a block comment's line
                if (std::regex_search(code, pattern))
                    violations.push_back(path + ":" + std::to_string(lineNo) + ": " + line);
            }
        }
    };
    const std::string source = ENP_SOURCE_DIR;
    scan(scan, source + "/src");
    scan(scan, source + "/include/enp");
    for (const std::string& v : violations)
        std::printf("    raw allocation: %s\n", v.c_str());
    CHECK(violations.empty());
}

// A crash names the test it happened in, with a backtrace where the platform has one: stdout is
// flushed after every test, so the log shows how far the run got.
const char* volatile g_currentTest = "";

#if !defined(_WIN32)
extern "C" void onFatalSignal(int sig)
{
    const char* name = g_currentTest;
    const char head[] = "\n*** fatal signal in test: ";
    (void)!::write(2, head, sizeof(head) - 1);
    (void)!::write(2, name, std::strlen(name));
    (void)!::write(2, "\n", 1);
#if defined(ENP_TEST_BACKTRACE)
    void* frames[64];
    const int n = ::backtrace(frames, 64);
    ::backtrace_symbols_fd(frames, n, 2);
#endif
    std::signal(sig, SIG_DFL);
    std::raise(sig);
}

void installCrashReport()
{
    // An alternate stack, so that a stack overflow on the main thread is reported too.
    static char altStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = altStack;
    ss.ss_size = sizeof(altStack);
    ::sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_handler = onFatalSignal;
    sa.sa_flags = SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        ::sigaction(sig, &sa, nullptr);
}
#else
void installCrashReport() {}
#endif

int main(int argc, char** argv)
{
    installCrashReport();
    os::InitParams params;
    params.argc = argc;
    params.argv = argv;
    os::init(params);
    // ENP_TEST_FILTER=text runs only the tests whose name contains it.
    const char* filter = std::getenv("ENP_TEST_FILTER");
    for (auto& [name, fn] : registry()) {
        if (filter && *filter && !std::strstr(name, filter))
            continue;
        const int before = g_failures;
        g_currentTest = name;
        fn();
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", name);
        std::fflush(stdout);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
