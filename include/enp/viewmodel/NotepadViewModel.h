// Main view-model: workspace mode, open documents, editing, find, commands. Owns the agent
// and review view-models. No services yet: documents come from DummyData and are not saved.
#pragma once

#include "enp/model/Document.h"
#include "enp/viewmodel/AgentViewModel.h"
#include "enp/viewmodel/CommandId.h"
#include "enp/viewmodel/ExplorerItem.h"
#include "enp/viewmodel/Observable.h"
#include "enp/viewmodel/ReviewViewModel.h"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace enp::viewmodel {

// Agent: describe goals, the agent edits, you review. Code: classic manual editor.
enum class WorkspaceMode { Agent, Code };
enum class ClipboardAction { Cut, Copy, Paste };
enum class ReviewTab { Problems, Diff, Comments, Checklist };

struct FindMatch {
    model::TextPos start;
    model::TextPos end;
};

class NotepadViewModel {
public:
    NotepadViewModel();
    ~NotepadViewModel();
    NotepadViewModel(const NotepadViewModel&) = delete;
    NotepadViewModel& operator=(const NotepadViewModel&) = delete;

    // ---- Bindable state ---------------------------------------------------------------
    Property<WorkspaceMode> workspace{WorkspaceMode::Code};
    Property<int> activeIndex{-1};
    Property<std::string> statusText{"Ready"};

    Property<bool> darkTheme{true};
    Property<bool> showExplorer{true};
    Property<bool> showReviewPanel{true};
    Property<bool> showAgentPanel{true};
    Property<bool> wordWrap{false};
    Property<bool> showWhitespace{false};
    Property<bool> showLineNumbers{true};
    Property<bool> showIndentGuides{true};
    Property<bool> recording{false};
    Property<float> zoom{1.0f};
    Property<ReviewTab> reviewTab{ReviewTab::Problems};

    Property<bool> findVisible{false};
    Property<bool> replaceVisible{false};
    Property<std::string> findQuery;
    Property<std::string> replaceText;
    Property<bool> matchCase{false};
    Property<int> currentMatch{-1};

    // ---- One-shot requests to the view -----------------------------------------------
    Event<ClipboardAction> clipboardRequested;
    Event<> focusFindRequested;
    Event<> focusPromptRequested;
    Event<> goToLineRequested;
    Event<> addCommentRequested;
    Event<> aboutRequested;
    Event<> preferencesRequested;
    Event<> quitRequested;
    Event<> caretRevealRequested; // scroll the editor so the caret is visible
    Event<> editorRequested;      // a location was opened: show the editor if it is hidden

    // ---- Commands ----------------------------------------------------------------------
    const Command& command(CommandId id) const { return commands_[size_t(id)]; }
    void execute(CommandId id) const { command(id).execute(); }
    // Toggle / radio state for ribbon buttons.
    bool isChecked(CommandId id) const;

    // ---- Documents ---------------------------------------------------------------------
    const std::vector<std::unique_ptr<model::Document>>& documents() const { return documents_; }
    model::Document* activeDocument();
    const model::Document* activeDocument() const;
    void activate(int index);
    void closeDocument(int index);
    void newDocument();
    void openFile(const std::string& path);
    void openStartupDocuments(const std::vector<std::string>& arguments);
    // Opens `path` and moves the caret to the 0-based `line`.
    void navigateTo(const std::string& path, int line);
    const ExplorerItem& explorer() const { return explorer_; }

    // ---- Editing (active document) -----------------------------------------------------
    void setCaret(model::TextPos pos, bool extendSelection);
    void setSelection(model::TextPos anchor, model::TextPos caret);
    void selectAll();
    std::string selectedText() const;
    void insertText(std::string_view text); // replaces the selection
    void newline();                          // keeps the indentation of the current line
    void backspace();
    void deleteForward();
    void deleteSelection();
    void goToLine(int line); // 0-based
    bool canUndo() const;
    bool canRedo() const;
    // "src/main.cpp:12-20", used as agent prompt context.
    std::string selectionContextLabel() const;

    // ---- Find --------------------------------------------------------------------------
    const std::vector<FindMatch>& findMatches() const { return findMatches_; }
    void findNext(int direction); // +1 forward, -1 backward, wraps
    void replaceCurrent();
    void replaceAll();

    AgentViewModel& agent() { return agent_; }
    const AgentViewModel& agent() const { return agent_; }
    ReviewViewModel& review() { return review_; }
    const ReviewViewModel& review() const { return review_; }

    // Called once per frame with a monotonic time in seconds.
    void tick(double now);

private:
    enum class EditKind { Typing, Deleting, Other };
    struct Snapshot {
        std::string text;
        model::TextPos caret;
        model::TextPos anchor;
    };
    struct History {
        std::vector<Snapshot> undo;
        std::vector<Snapshot> redo;
        EditKind lastKind = EditKind::Other;
        model::TextPos lastCaret{-1, -1};
    };

    void initCommands();
    void set(CommandId id, std::function<void()> execute, std::function<bool()> canExecute = {});
    void notImplemented(CommandId id);
    model::Document& addDocument(std::string title, std::string path, const std::string& text);
    void applyText(const std::string& path, const std::string& text);

    void beginEdit(EditKind kind);
    void endEdit();
    void undo();
    void redo();
    void restore(const Snapshot& snapshot);

    // Line range covered by the selection (or the caret line).
    void selectedLines(int& first, int& last) const;
    void transformLines(const std::function<void(std::vector<std::string>&)>& transform);
    void transformSelection(const std::function<std::string(const std::string&)>& transform);
    void duplicateLines();
    void deleteLines();
    void moveLines(int direction);
    void toggleComment();
    void trimTrailing();
    void toggleBookmark();
    void jumpToBookmark(int direction);
    void compareWithSaved();
    void askAgent(const std::string& prompt);
    void setZoom(float value);

    void refreshFind();

    std::vector<std::unique_ptr<model::Document>> documents_;
    std::map<int, History> history_; // by document id
    std::vector<Command> commands_;
    std::vector<FindMatch> findMatches_;
    ExplorerItem explorer_;
    AgentViewModel agent_;
    ReviewViewModel review_;
    int nextDocumentId_ = 1;
    int untitledCounter_ = 0;
    std::vector<Subscription> subscriptions_;
};

} // namespace enp::viewmodel
