// Unit tests for the model and view-models (no framework dependency).
#include "enp/model/Diff.h"
#include "enp/model/Document.h"
#include "enp/model/TextBuffer.h"
#include "enp/viewmodel/DummyData.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <cstdio>
#include <functional>
#include <string>
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
    CHECK(f.vm.reviewTab.get() == viewmodel::ReviewTab::Diff);
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

} // namespace

int main()
{
    for (auto& [name, fn] : registry()) {
        const int before = g_failures;
        fn();
        std::printf("%s %s\n", g_failures == before ? "[ OK ]" : "[FAIL]", name);
    }
    std::printf("\n%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
