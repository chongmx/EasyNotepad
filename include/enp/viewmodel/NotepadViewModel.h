// Main view-model: workspace mode, open documents, editing, find, commands. Owns the agent,
// review and terminal view-models. Documents still come from DummyData and are not saved; the
// terminal runs on ITerminalService when one is supplied.
#pragma once

#include "enp/interfaces/IFileService.h"
#include "enp/model/Document.h"
#include "enp/model/Outline.h"
#include "enp/viewmodel/AgentViewModel.h"
#include "enp/viewmodel/CommandId.h"
#include "enp/viewmodel/EditHistory.h"
#include "enp/viewmodel/EngineLabViewModel.h"
#include "enp/viewmodel/ExplorerItem.h"
#include "enp/viewmodel/FileDialogViewModel.h"
#include "enp/viewmodel/FindInFilesViewModel.h"
#include "enp/interfaces/IAgentCliService.h"
#include "enp/interfaces/IProcessService.h"
#include "enp/viewmodel/LayersViewModel.h"
#include "enp/viewmodel/LogicalMapViewModel.h"
#include "enp/viewmodel/Observable.h"
#include "enp/viewmodel/PointerViewModel.h"
#include "enp/viewmodel/GitViewModel.h"
#include "enp/viewmodel/ReviewViewModel.h"
#include "enp/viewmodel/Settings.h"
#include "enp/viewmodel/SyntaxColorSettings.h"
#include "enp/viewmodel/TerminalViewModel.h"

#include <functional>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::viewmodel {

// What the editor area shows: the text, or the logical map of the program.
enum class EditorMode { Code, Map };

// Agent: describe goals, the agent edits, you review. Code: classic manual editor.
enum class WorkspaceMode { Agent, Code };
enum class ClipboardAction { Cut, Copy, Paste };
// Tabs of the bottom panel (VS Code's "Panel").
enum class PanelTab { Problems, Diff, Comments, Checklist, Terminal, Search, History, Status }; // append only: saved as numbers

// A user-defined ribbon button that runs a shell command in the integrated terminal.
struct CustomAction {
    std::string name;
    std::string command;
};

// The agent's on-screen cursor: it glides to a UI element (by automation id), optionally plays a
// click, and shows a message. It never moves the user's mouse.
struct AgentPointer {
    bool visible = false;
    std::string target;  // UI element id, e.g. "command:ToggleTerminal"
    std::string message;
    bool click = false;
    int serial = 0;      // changes with every request, restarting the animation

    bool operator==(const AgentPointer& o) const
    {
        return visible == o.visible && target == o.target && message == o.message && click == o.click && serial == o.serial;
    }
};

struct FindMatch {
    model::TextPos start;
    model::TextPos end;
};

class NotepadViewModel {
public:
    // Services may be null (tests, platforms without them): the sample workspace is used instead.
    explicit NotepadViewModel(interfaces::ITerminalService* terminalService = nullptr,
                              interfaces::IFileService* fileService = nullptr,
                              interfaces::IAgentCliService* agentCli = nullptr,
                              interfaces::IProcessService* processService = nullptr,
                              interfaces::ILocalEngineService* localEngine = nullptr);
    ~NotepadViewModel();
    NotepadViewModel(const NotepadViewModel&) = delete;
    NotepadViewModel& operator=(const NotepadViewModel&) = delete;

    // ---- Bindable state ---------------------------------------------------------------
    Property<WorkspaceMode> workspace{WorkspaceMode::Code};
    Property<int> activeIndex{-1};
    Property<std::string> statusText{"Ready"};

    Property<bool> darkTheme{true};
    Property<bool> showExplorer{true};
    Property<bool> slimRibbon{false}; // icons only, one row: more room for the work
    // Developer and power-user pages (the Engine lab). Turned on by tapping the version in About
    // seven times; nothing of it shows otherwise.
    Property<bool> developerMode{false};
    Property<bool> showPanel{true};
    Property<bool> panelMaximized{false};
    Property<bool> showAgentPanel{true};
    Property<bool> wordWrap{false};
    Property<bool> showWhitespace{false};
    Property<bool> showLineNumbers{true};
    Property<bool> showIndentGuides{true};
    Property<bool> terminalPoppedOut{false}; // the terminal lives in its own window
    Property<bool> smartIndent{true}; // language-aware Enter / closers; off: Enter keeps the indent

    // Performance (saved): the app draws only when something changes; while it does, at most
    // maxFramesPerSecond (0: as fast as the display allows). Without a blinking caret an idle app
    // draws nothing at all.
    static constexpr int kDefaultMaxFps = 30;
    Property<int> maxFramesPerSecond{kDefaultMaxFps};
    Property<bool> blinkCaret{true};
    Property<bool> recording{false};

    // ---- Macros and tools ---------------------------------------------------------------------
    // A macro: typed text, Enter / Backspace / Delete / Tab, caret moves (relative to where the
    // caret was, so it replays anywhere) and editing commands, recorded between Record and Stop.
    struct MacroStep {
        enum class Kind { Text, Newline, Backspace, Delete, Indent, Move, Command };
        Kind kind = Kind::Text;
        std::string text;
        int line = 0, column = 0; // Move: the change of line and column
        bool extend = false;      // Move: extends the selection
        CommandId command = CommandId::Count;
    };
    const std::vector<MacroStep>& macro() const { return macro_; }
    void playMacro(int times = 1); // one undo step for all of it
    Property<std::string> hashResult; // the last SHA-256 (the view shows and clears it)
    Property<float> zoom{1.0f};
    // Reading: how much code is shown. 0 Full, 1 Balanced, 2 Overview, 3 Outline.
    Property<int> detailLevel{0};
    // Side bar beside the editor: the layers view or a classic minimap (or hidden).
    enum class SideBar { Layers, Minimap };
    Property<bool> showLayersMap{true}; // side bar visible
    Property<SideBar> sideBar{SideBar::Layers};
    Property<EditorMode> editorMode{EditorMode::Code};
    // Syntax folding follows brackets; logical folding turns function bodies into summarized steps.
    Property<model::OutlineMode> foldingMode{model::OutlineMode::Syntax};
    Property<AgentPointer> agentPointer;
    Property<PanelTab> panelTab{PanelTab::Problems};

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
    Event<> engineLabRequested;
    Event<> preferencesRequested;
    Event<> syntaxColorsRequested;
    Event<> languagePickerRequested;
    Event<> gitTabRequested;       // the explorer's Git tab
    Event<> branchPickerRequested;
    Event<> gitStashRequested;
    Event<> gitGuideRequested;
    Event<> gitDiscardAllRequested; // the view asks first
    Event<std::string> gitShowCommitRequested; // a commit hash (blame): show it in History
    Event<std::string> gitFileHistoryRequested; // a repository path: its history in History
    Event<> quitRequested; // safe to quit now (nothing unsaved, or the user decided)

    // ---- Unsaved changes ---------------------------------------------------------------------
    // Closing a modified document or quitting with modified documents asks first. The view shows
    // a prompt while pendingClose / quitConfirmPending are set and answers with resolve*().
    enum class CloseChoice { Save, Discard, Cancel };
    Property<int> pendingClose{-1};          // id of the document waiting for an answer, -1: none
    Property<bool> quitConfirmPending{false};
    // Close Folder with unsaved documents: the view asks, then resolveCloseFolder().
    Property<bool> closeFolderPending{false};
    void requestCloseFolder();
    void resolveCloseFolder(CloseChoice choice);
    void closeFolder(); // closes every document, unsaved or not
    bool folderOpen() const { return !workspaceRoot_.empty(); }
    // No folder, after Close Folder (rather than the sample workspace shown at first start).
    bool noFolder() const { return workspaceRoot_.empty() && explorer_.children.empty(); }
    void requestCloseDocument(int index);    // closes at once when there is nothing to lose
    void resolvePendingClose(CloseChoice choice);
    void requestQuit();
    void resolveQuit(CloseChoice choice);    // Save: save all then quit; Discard: quit anyway
    std::vector<const model::Document*> unsavedDocuments() const;
    int documentIndexById(int id) const;
    // A document that can be saved without asking for a path (it has one).
    bool canSaveInPlace(const model::Document& doc) const;
    Event<> caretRevealRequested; // scroll the editor so the caret is visible
    Event<> editorRequested;      // a location was opened: show the editor if it is hidden
    Event<CommandId> ribbonRevealRequested; // switch the ribbon to a tab showing this command

    // ---- Commands ----------------------------------------------------------------------
    const Command& command(CommandId id) const { return commands_[size_t(id)]; }
    void execute(CommandId id) const; // (recorded into a macro while recording)
    // Toggle / radio state for ribbon buttons.
    bool isChecked(CommandId id) const;

    // ---- Documents ---------------------------------------------------------------------
    const std::vector<std::unique_ptr<model::Document>>& documents() const { return documents_; }
    model::Document* activeDocument();
    const model::Document* activeDocument() const;
    void activate(int index);
    void closeDocument(int index);
    // Bulk closing keeps documents with unsaved changes open (and says so in the status bar).
    void closeOthers(int index);
    void closeToTheRight(int index);
    void closeSaved();
    void closeAll();
    // Tab order: drag a tab to `to`.
    void moveDocument(int from, int to);
    // Index of the open document for a workspace path, or -1.
    int documentIndex(const std::string& path) const;
    // Distinct tab labels: the file name, plus enough of its folder when another open document
    // has the same name ("MainView.h" / "view" vs "MainView.h" / "app").
    std::string documentHint(int index) const;
    void newDocument();
    void openFile(const std::string& path);
    void openStartupDocuments(const std::vector<std::string>& arguments);
    // Opens `path` and moves the caret to the 0-based `line`.
    void navigateTo(const std::string& path, int line);
    const ExplorerItem& explorer() const { return explorer_; }
    // Opens a real folder as the workspace (needs the file service); closes the sample documents.
    bool openFolder(const std::string& path);
    // Absolute root of the opened folder; empty while showing the sample workspace.
    const std::string& workspaceRoot() const { return workspaceRoot_; }
    // Text of a workspace file: the open document's (unsaved edits included), the file on disk,
    // or the sample content.
    std::string readWorkspaceFile(const std::string& path) const;
    // Pulses whenever the explorer should reveal the active document (opening, switching).
    int revealSerial() const { return revealSerial_; }
    // Writes a file under the workspace (creating folders); false without a real folder.
    bool writeWorkspaceFile(const std::string& path, const std::string& text);

    // ---- Editing (active document) -----------------------------------------------------
    void setCaret(model::TextPos pos, bool extendSelection);
    void setSelection(model::TextPos anchor, model::TextPos caret);
    void selectAll();
    std::string selectedText() const;
    void insertText(std::string_view text); // replaces the selection
    void newline();                          // smart indent (or keeps the current line's indent)
    void insertIndent();                     // Tab: one level (a tab, or spaces to the next stop)
    void backspace();                        // in leading spaces: back to the previous stop
    void setIndentStyle(model::IndentStyle style); // the active document's
    void deleteForward();
    void deleteSelection();
    void goToLine(int line); // 0-based
    bool canUndo() const;
    size_t historyBytes() const; // text held by the active document's undo and redo steps
    bool canRedo() const;
    // "src/main.cpp:12-20", used as agent prompt context.
    std::string selectionContextLabel() const;

    // ---- Reading: outline, folding, summaries (active document) --------------------------
    static constexpr int kDetailLevels = 4;
    static const char* detailLevelName(int level);
    static const char* detailLevelDescription(int level);
    const std::vector<model::OutlineNode>& outline() const;
    // Innermost region whose header is `line`, or null.
    const model::OutlineNode* regionAt(int line) const;
    // AI summary of a region (sample data until the summarizer CLI is connected).
    const std::string& summary(const model::OutlineNode& node) const;
    bool isFolded(const model::OutlineNode& node) const;
    void toggleFold(int startLine);
    void setFolded(int startLine, bool folded);
    void foldAll();
    void unfoldAll();
    // Unfolds every region that hides `line`.
    void revealLine(int line);

    // ---- Settings -----------------------------------------------------------------------
    // Where settings are kept (a JSON file read and written through the file service); loads it.
    // Without a path, changes last for the session only.
    void setSettingsPath(const std::string& path);
    const std::string& settingsPath() const { return settings_.path(); }
    // The settings store: views keep their own state (panel sizes) in it too.
    Settings& settings() { return settings_; }

    // ---- Recent files and the session -----------------------------------------------------
    static constexpr size_t kMaxRecentFiles = 15;
    static constexpr size_t kMaxRecentFolders = 10;
    const std::vector<std::string>& recentFiles() const { return recentFiles_; }   // absolute, newest first
    const std::vector<std::string>& recentFolders() const { return recentFolders_; }
    void openRecent(const std::string& absolutePath); // a file or a folder; drops it when it is gone
    void clearRecent();
    // Reopens the last session's folder and files (with their caret lines); false when there was
    // none to restore.
    bool restoreSession();

    // ---- Files changed by other programs ----------------------------------------------------
    // Checked every couple of seconds: an unmodified document reloads by itself; a modified one is
    // marked (diskState) for the user to choose; saving over a changed file waits for that choice.
    void checkDiskChanges();
    void reloadFromDisk(model::Document& doc); // takes the file's text (the edits are dropped)
    void keepMine(model::Document& doc);       // the next save overwrites the file
    void compareWithDisk(model::Document& doc); // the Diff tab: on disk vs in the editor

    // Syntax colours: overrides of the theme's colours as 0xRRGGBB, per theme (dark / light), for
    // one language (its table key) or for all of them (kAllLanguages). The editor uses the
    // language's colour, else the all-languages colour, else the theme default.
    static constexpr const char* kAllLanguages = "*";

    // Indentation for new documents and files with nothing indented yet: the user's choice (for
    // example 2 spaces everywhere), or nullopt for each language's convention. Saved in settings.
    // Files that are already indented keep their own style (detected).
    const std::optional<model::IndentStyle>& preferredIndent() const { return preferredIndent_; }
    void setPreferredIndent(std::optional<model::IndentStyle> style);
    model::IndentStyle indentFor(model::Language language) const;
    void setDocumentLanguage(model::Language language); // the active document's
    std::optional<uint32_t> syntaxColor(bool dark, const std::string& languageKey, model::Token token) const;
    void setSyntaxColor(bool dark, const std::string& languageKey, model::Token token, std::optional<uint32_t> rgb);
    void resetSyntaxColors(bool dark, const std::string& languageKey); // kAllLanguages + empty key: everything
    bool hasSyntaxColors(bool dark, const std::string& languageKey) const;
    int syntaxColorsRevision() const { return syntaxColors_.revision(); }

    // ---- Ribbon customization ("My Tools") ----------------------------------------------
    // Not persisted yet: kept until the settings service exists.
    const std::vector<CommandId>& pinnedCommands() const { return pinned_; }
    bool isPinned(CommandId id) const;
    void setPinned(CommandId id, bool pinned);
    const std::vector<CustomAction>& customActions() const { return customActions_; }
    void addCustomAction(std::string name, std::string command);
    void removeCustomAction(int index);
    // Opens the terminal (starting a shell if needed) and runs the action's command.
    void runCustomAction(int index);
    // Opens the terminal (starting a shell if needed) and runs `command` in it.
    void runInTerminal(const std::string& command);
    // A new tab holding `text` (not a file on disk); `pathForLanguage` picks the colouring.
    void openTextDocument(const std::string& title, const std::string& pathForLanguage, const std::string& text);
    // Explorer actions on workspace paths (folder "" is the workspace itself). They report problems in
    // the status bar and return false.
    bool createFile(const std::string& folder, const std::string& name);   // and opens it
    bool createFolder(const std::string& folder, const std::string& name);
    bool renamePath(const std::string& path, const std::string& newName); // open documents follow
    bool deletePath(const std::string& path);                             // closes its open documents
    void revealInFileManager(const std::string& path);
    void openTerminalIn(const std::string& folder);
    void findInFolder(const std::string& folder);
    bool canEditFiles() const { return files_ && !workspaceRoot_.empty(); }
    // Replaces lines [first, last] of the active document with `lines` (last < first: inserts them
    // before `first`); one undo step.
    void replaceLines(int first, int last, const std::vector<std::string>& lines);
    GitViewModel& git() { return git_; }
    const GitViewModel& git() const { return git_; }

    // ---- Agent cursor ----------------------------------------------------------------------
    // Points the agent cursor at a UI element; `seconds` > 0 hides it again after that long.
    void pointAt(std::string target, std::string message, bool click = false, double seconds = 6.0);
    void hidePointer();
    void startTour();
    void stopTour();
    bool tourRunning() const { return pointer_.tourRunning(); }

    // ---- Find --------------------------------------------------------------------------
    const std::vector<FindMatch>& findMatches() const { return findMatches_; }
    void findNext(int direction); // +1 forward, -1 backward, wraps
    void replaceCurrent();
    void replaceAll();

    EngineLabViewModel& engineLab() { return engineLab_; }
    AgentViewModel& agent() { return agent_; }
    const AgentViewModel& agent() const { return agent_; }
    ReviewViewModel& review() { return review_; }
    const ReviewViewModel& review() const { return review_; }
    LayersViewModel& layers() { return layers_; }
    LogicalMapViewModel& logicalMap() { return logicalMap_; }
    // The agent is Claude Code: a streaming conversation that can operate the app.
    bool agentCanStream() const { return agentCli_ && agentCli_->canStream(); }
    // Agent set-up (the card in the Agent panel).
    struct AgentSetup {
        bool available = false;
        bool checking = false;
        bool claude = false;    // the command is Claude Code
        std::string command;    // the command line in use
        std::string executable; // where it was found
    };
    AgentSetup agentSetup() const;
    void recheckAgent();
    // Claude Code's official installer for this system, as typed in a terminal.
    static std::string agentInstallCommand();
    void installAgent(); // runs the installer in the terminal
    void loginAgent();   // runs `claude` in the terminal, which asks the user to log in
    void openLink(const std::string& url);
    static constexpr const char* kAgentSetupUrl = "https://code.claude.com/docs/en/setup";
    FileDialogViewModel& fileDialog() { return fileDialog_; }
    FindInFilesViewModel& findInFiles() { return findInFiles_; }
    Event<> searchFocusRequested; // Find in Files: focus the query box
    std::string absolutePath(const std::string& path) const; // workspace-relative -> absolute
    // Replaces a document's whole text as one undoable edit (opening it if needed).
    void applyText(const std::string& path, const std::string& text);
    // Writes the document to `path` (absolute or workspace-relative); it takes the new name, path
    // and language. Paths inside the workspace are kept workspace-relative.
    bool saveDocumentAs(model::Document& doc, const std::string& path);
    std::string homeDirectory() const { return files_ ? files_->homeDirectory() : std::string(); }
    // Workspace-relative when `path` is inside the workspace, else unchanged.
    std::string workspaceRelative(const std::string& path) const;
    TerminalViewModel& terminal() { return terminal_; }
    const TerminalViewModel& terminal() const { return terminal_; }

    // Shows the bottom panel on `tab`; the terminal tab starts a shell on first use.
    void showPanelTab(PanelTab tab);

    // Called once per frame with a monotonic time in seconds.
    void tick(double now);

private:
    using EditKind = EditHistory::Kind;

    void initCommands();
    void set(CommandId id, std::function<void()> execute, std::function<bool()> canExecute = {});
    void notImplemented(CommandId id);
    model::Document& addDocument(std::string title, std::string path, const std::string& text);
    void refreshExplorer(); // re-reads the workspace folder tree
    bool saveDocument(model::Document& doc);

    void beginEdit(EditKind kind);
    void endEdit();
    void undo();
    void redo();
    void afterHistoryMove(model::Document& doc);

    // Line range covered by the selection (or the caret line).
    void selectedLines(int& first, int& last) const;
    void transformLines(const std::function<void(std::vector<std::string>&)>& transform, bool wholeDocument = false);
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
    void applyDetailLevel(model::Document& doc);

    struct OutlineCache {
        int documentId = -1;
        uint64_t revision = ~uint64_t(0);
        model::Language language = model::Language::PlainText;
        model::OutlineMode mode = model::OutlineMode::Syntax;
        std::vector<model::OutlineNode> nodes;
        std::map<int, std::string> summaries; // by start line
    };
    mutable OutlineCache outline_;

    std::vector<CommandId> pinned_;
    std::vector<CustomAction> customActions_;

    std::vector<std::unique_ptr<model::Document>> documents_;
    EditHistory history_;
    std::vector<Command> commands_;
    std::vector<FindMatch> findMatches_;
    ExplorerItem explorer_;
    interfaces::IFileService* files_ = nullptr;
    std::string workspaceRoot_;
    AgentViewModel agent_;
    EngineLabViewModel engineLab_;
    ReviewViewModel review_;
    TerminalViewModel terminal_;
    LayersViewModel layers_{*this};
    LogicalMapViewModel logicalMap_;
    FileDialogViewModel fileDialog_;
    FindInFilesViewModel findInFiles_;
    int nextDocumentId_ = 1;
    int untitledCounter_ = 0;
    std::vector<Subscription> subscriptions_;
    int revealSerial_ = 0;
    void closeWhere(const std::function<bool(int index, const model::Document&)>& close);
    void bindSettings();
    bool reindentClosingLine(model::Document& doc, int line); // in an open edit
    void applyDetectedIndent(model::Document& doc);
    std::optional<model::IndentStyle> preferredIndent_;
    interfaces::IAgentCliService* agentCli_ = nullptr;
    interfaces::IProcessService* processes_ = nullptr;
    std::vector<std::string> recentFiles_;
    std::vector<std::string> recentFolders_;
    model::Json savedSession_;
    void remember(std::vector<std::string>& list, const std::string& path, size_t max);
    std::vector<MacroStep> macro_;
    mutable std::vector<MacroStep> macroRecording_;
    mutable int macroDepth_ = 0; // only the outermost action of a call is recorded
    bool playingMacro_ = false;
    void recordMacro(MacroStep step) const;
    struct MacroScope {
        const NotepadViewModel& vm;
        bool outer;
        explicit MacroScope(const NotepadViewModel& v) : vm(v), outer(v.macroDepth_++ == 0) {}
        ~MacroScope() { --vm.macroDepth_; }
    };
    void recordDiskStamp(model::Document& doc);
    std::optional<std::string> readDiskText(const model::Document& doc) const; // without BOM
    double nextDiskCheck_ = 0.0;
    // Last: destroyed first, so its final write still sees every property and section.
    Settings settings_;
    // After the store, which they register with (and unregister from when destroyed).
    SyntaxColorSettings syntaxColors_{settings_};
    PointerViewModel pointer_{*this};
    GitViewModel git_{*this};
};

} // namespace enp::viewmodel
