// IntelliSense for the editor: completion as you type (or on Ctrl+Space) from the names of the
// workspace, the document and the language, Go to Definition (F12), and the declaration of the
// name under the mouse.
//
// AI completion (plan 21) plugs in as a provider: every time the suggestions change it gets a
// Request (the code around the caret and the context snippets IntelliSense chose for it), and
// answers later on the main thread with provide(). Its suggestions are shown first, may insert
// more than a name (insertText, several lines), and an answer to an older request or a closed
// list is dropped. IntelliSense decides what a model sees: contextFor() picks the declarations
// relevant to the caret (what is being typed, what the code around it uses, what the file
// declares), ranks them and keeps them within a budget; new kinds of snippets are added there.
//
// Code from comments: with a provider that implements (the local model, LocalCodeAssistant), a
// comment followed by Enter (after a pause) or Ctrl+Shift+Space asks it for the code the comment
// describes; the answer is offered in the same list, and inserted indented like the comment.
//
// C and C++: the headers a file includes (directly and through the workspace's headers) and the
// chosen standard decide which names of the standard library are suggested; a name from a header
// the file does not include says which #include it needs.
//
// Memory (docs/MEMORY.md): the workspace's symbol index is built on a background thread in the
// scope "intellisense", owned here, and released by workspaceChanged() when the folder closes or
// another opens; a build still running then is cancelled and its late result dropped. The
// suggestions of an open completion are released when it closes. Nothing here keeps a pointer to
// a document: the session remembers its id.
#pragma once

#include "enp/interfaces/IFileService.h"
#include "enp/model/CLibrary.h"
#include "enp/model/Document.h"
#include "enp/model/Symbols.h"
#include "enp/viewmodel/Observable.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::viewmodel {

class NotepadViewModel;

class IntellisenseViewModel {
public:
    // A piece of the workspace chosen as context for a model, and why.
    enum class ContextReason : unsigned char {
        Typed,     // its name matches the word being typed
        UsedNearby, // the code around the caret uses it
        SameFile,  // declared in the file being edited
    };
    struct ContextSnippet {
        ContextReason reason = ContextReason::Typed;
        model::SymbolKind kind = model::SymbolKind::Function;
        std::string name;
        std::string container;
        std::string path; // workspace-relative
        int line = 0;     // 0-based
        std::string text; // the declaration as written
        int score = 0;    // higher first
    };
    struct ContextBudget {
        size_t maxBytes = 3072; // of formatted text
        int maxSnippets = 48;
    };
    static const char* contextReasonName(ContextReason reason); // "typed", "used nearby", "same file"

    // Where a suggestion comes from.
    enum class Source : unsigned char { Language, Document, Workspace, Provider };
    struct Item {
        std::string label;
        std::string insertText; // replaces the typed part; empty: the label (a provider's may span lines)
        std::string detail;     // the declaring line, "keyword", or empty for a word of the document
        std::string location;   // "src/view/Editor.cpp:42" for an indexed declaration
        std::string note;       // a clue: "<vector> · C++11", "needs #include \"widget.h\""
        model::SymbolKind kind = model::SymbolKind::Word;
        Source source = Source::Document;
        int score = 0;
    };

    // What an outside completion source (the local model) is asked: the code around the caret.
    struct Request {
        enum class Purpose { Complete, Implement };
        Purpose purpose = Purpose::Complete; // the word at the caret, or the code a comment describes
        std::string instruction;             // Implement: the comment, without its markers
        std::string standard;                // "C++17", "C11"; empty outside C and C++
        std::vector<std::string> headers;    // what the file includes, as written: "<vector>", "\"widget.h\""
        int serial = 0;     // pass back to provide(): answers to older requests are dropped
        int documentId = -1;
        std::string path;   // workspace-relative, empty for an untitled document
        model::Language language = model::Language::PlainText;
        std::string prefix; // the part of the word typed so far
        std::string before; // the text before the caret (at most kRequestBefore bytes)
        std::string after;  // the text after it (at most kRequestAfter bytes)
        std::vector<ContextSnippet> context; // chosen by contextFor(), best first
    };
    // An outside source of suggestions. `request` must not block (start the work, answer later
    // with provide()); `cancel` stops the work for the last request (the list closed or changed).
    struct Provider {
        std::function<void(const Request&)> request;
        std::function<void()> cancel;
        bool completes = true;   // asked while a word is typed
        bool implements = false; // asked for the code of a comment
    };
    static constexpr size_t kRequestBefore = 4096;
    static constexpr size_t kRequestAfter = 1024;
    static constexpr int kMaxProvided = 8;

    static constexpr int kMaxItems = 100;              // suggestions shown at most
    static constexpr int kAutoTriggerLength = 2;       // typed characters before suggestions appear by themselves
    static constexpr size_t kMaxFiles = 5000;          // files indexed at most
    static constexpr size_t kMaxFileBytes = 2u << 20;  // larger files are skipped (generated code)
    static constexpr size_t kMaxReadBytes = 128u << 20; // read for one index at most
    static constexpr size_t kMaxSymbols = 400000;
    static constexpr size_t kMaxScanBytes = 4u << 20;  // of the document, for its words

    IntellisenseViewModel(NotepadViewModel& owner, interfaces::IFileService* files);
    ~IntellisenseViewModel();
    IntellisenseViewModel(const IntellisenseViewModel&) = delete;
    IntellisenseViewModel& operator=(const IntellisenseViewModel&) = delete;

    Property<bool> autoComplete{true}; // suggestions appear while typing code (saved)
    Property<bool> aiSuggestions{false}; // code from comments by the local model (saved; off until chosen)
    Property<int> cppStandard{0};      // 2011, 2017, 2020...; 0: from the project (saved)
    Property<int> cStandard{0};        // 1999, 2011, 2017...; 0: from the project (saved)
    Property<std::string> includeFolders; // workspace-relative, ';'-separated: where #include "..." looks (saved)
    Property<bool> indexing{false};    // the workspace is being read

    // ---- C and C++ -------------------------------------------------------------------------
    // The standard in use: the one chosen, else the project's (CMakeLists.txt, Makefile,
    // meson.build), else C++17 / C17.
    int cppStandardInUse();
    int cStandardInUse();
    // Where the standard in use comes from: "chosen", "CMakeLists.txt", "default".
    const std::string& standardSource(bool c);
    // The headers `doc` includes, directly and through the workspace's headers: as written for
    // its own lines ("<vector>", "\"widget.h\""), then those reached through them.
    std::vector<std::string> includedHeaders(const model::Document& doc) const;
    // A name of the standard library for a tooltip: available in this document's language and
    // standard. Allocates nothing.
    const model::LibraryName* libraryLookup(std::string_view name, const model::Document& doc);

    // ---- The workspace's symbol index ------------------------------------------------------
    // Builds it in the background unless it is built or being built for this workspace.
    void ensureIndex();
    void rebuildIndex();   // reads the workspace again (the old index is freed first)
    void buildIndexNow();  // synchronous: tests and small workspaces
    bool indexReady() const { return built_; }
    const model::SymbolIndex& index() const { return index_; }
    // The folder closed or another opened: the index, a running build and the session go.
    void workspaceChanged();
    // Files were renamed or deleted: the index is dropped and read again on next use.
    void invalidate();
    // A document was saved: its declarations in the index are replaced.
    void documentSaved(const model::Document& doc);
    // Background builds still running (tests wait for 0 before measuring).
    static int runningBuilds();

    // ---- Completion (active document) ------------------------------------------------------
    bool active() const { return active_; }
    int documentId() const { return documentId_; }
    model::TextPos anchor() const { return anchor_; } // where the completed word starts
    const std::vector<Item>& items() const { return items_; }
    int selected() const { return selected_; }
    int selectionSerial() const { return selectionSerial_; } // bumps when the selection moves
    const std::string& prefix() const { return prefix_; }

    void trigger();                    // Ctrl+Space: shows suggestions even before anything is typed
    void typed(std::string_view text); // the editor inserted typed text: may open suggestions
    // Every frame while active: follows the caret (refiltering as the word grows or shrinks) and
    // closes when the caret leaves the word, the document changes or another is shown. Does
    // nothing (and allocates nothing) while the document and caret stay as they are.
    void sync();
    void move(int delta);  // the selection, clamped
    void select(int index);
    bool accept();         // replaces the typed part with the selected suggestion
    void dismiss();        // closes and releases the suggestions

    // ---- AI completion -----------------------------------------------------------------------
    // The provider is asked whenever the suggestions change (completes) and for the code of a
    // comment (implements). Kept here; whatever it captures must outlive this object or be reset
    // with setProvider({}).
    void setProvider(Provider provider);
    bool hasProvider() const { return bool(provider_.request); }
    // Main thread: what the provider is doing for request `serial` ("Loading the local model"),
    // shown while the list waits for it.
    void setProviderStatus(int serial, const std::string& text);
    // Main thread: the provider cannot answer request `serial`; says why and closes the list.
    void providerFailed(int serial, const std::string& why);
    bool awaiting() const { return awaiting_; }
    const std::string& providerStatus() const { return providerStatus_; }

    // ---- Code from comments ----------------------------------------------------------------
    // Asks the provider for the code the comment at the caret describes (the caret on the
    // comment, or on the empty line below it). Explicit (Ctrl+Shift+Space) explains in the status
    // bar why it cannot; otherwise it is silent.
    bool implementComment(bool explicitRequest);
    // The editor inserted a line break: with code from comments on and a comment above, the
    // provider is asked once the caret has stayed there for kCommentPause seconds.
    void afterNewline();
    void tick(double now);
    static constexpr double kCommentPause = 0.8;
    bool block() const { return block_; } // the open list offers the code of a comment
    // Bumps whenever a list (or a request for code) opens: the editor takes the keyboard back
    // then, so a list opened from the ribbon or by an agent stays open.
    int sessionSerial() const { return sessionSerial_; }
    // Main thread: the provider's suggestions for request `serial`. Dropped when the list closed
    // or changed since (a newer request); otherwise shown first.
    void provide(int serial, std::vector<Item> items);
    int requestSerial() const { return requestSerial_; }
    // The context a model gets for code at the caret, chosen from the index so it uses real names:
    // declarations matching `prefix`, those of names the code `nearby` uses (defined elsewhere),
    // and the types and functions of `path`. One snippet per declaration, best first, within
    // `budget`. Empty until the index is built.
    std::vector<ContextSnippet> contextFor(std::string_view prefix, std::string_view path, std::string_view nearby,
                                           const ContextBudget& budget) const;
    std::vector<ContextSnippet> contextFor(std::string_view prefix, std::string_view path, std::string_view nearby) const;
    // One snippet per line: "function resize in Widget (src/widget.h:12, used nearby): void resize(int n);"
    static std::string formatContext(const std::vector<ContextSnippet>& snippets);

    // ---- Navigation ------------------------------------------------------------------------
    // The declaration of the name at the caret: in this document first (its current text), then
    // the workspace (definitions before declarations). On a definition already, goes to another
    // place the name is declared (a header's prototype). Waits for the index when it is still
    // being built.
    bool goToDefinition();
    // The indexed declaration of `name` for a tooltip: a definition before a declaration, one in
    // `preferPath` before others. Null when unknown. Allocates nothing; valid until the index
    // changes (use it within the frame).
    std::optional<model::SymbolView> lookup(std::string_view name, std::string_view preferPath) const;

    // Where Go to Definition jumped from, to go back (Alt+Left) and forward again (Alt+Right).
    // Places are remembered by document id and path, never by pointer: a closed document is
    // opened again from its path, an untitled one that closed is skipped. At most kMaxHistory
    // each way; released with the workspace.
    static constexpr size_t kMaxHistory = 50;
    bool canGoBack() const { return !back_.empty(); }
    bool canGoForward() const { return !forward_.empty(); }
    bool goBack();
    bool goForward();
    size_t historySize() const { return back_.size() + forward_.size(); }

private:
    struct IndexSource { // a file to read for the index
        std::string path;
        std::string disk; // absolute, read on the background thread
        std::string text; // open documents (unsaved edits) and the sample workspace
        bool hasText = false;
        model::Language language = model::Language::PlainText;
    };
    std::vector<IndexSource> collect() const;
    static void build(std::vector<IndexSource>& sources, interfaces::IFileService* files, const std::atomic<bool>* cancel,
                      model::SymbolIndex& out);
    void apply(model::SymbolIndex index);
    void start(bool explicitRequest); // what comes before the word (".", "::") is read from the text
    void refresh();
    void refreshItems(const model::Document& doc);
    void status(const std::string& text); // the status bar, not counted as IntelliSense's memory
    void ask(const model::Document& doc, Request::Purpose purpose, std::string_view instruction);
    // The files of the index a C or C++ document reaches through its #include lines (one byte per
    // file), and the standard-library headers among them (without <>).
    struct Reach {
        std::vector<char> files;
        std::vector<std::string> library;
    };
    Reach reachFrom(const model::Document& doc) const;
    int resolveInclude(std::string_view name, bool system, std::string_view from) const; // -1: not in the index
    std::string includeSpelling(int file, std::string_view from) const; // "widget.h", "view/Editor.h"
    void detectStandards();
    bool inCodeAt(const model::Document& doc, model::TextPos pos) const; // not in a comment or string
    bool jumpTo(const std::string& path, int line, int column);
    // The position of the best declaration named `name` (a definition first, then one in
    // `preferPath`), leaving out those in `skipPath`; -1 when there is none.
    long best(std::string_view name, std::string_view preferPath, std::string_view skipPath) const;
    struct Place {
        int documentId = -1;
        std::string path; // empty for an untitled document
        model::TextPos caret;
    };
    Place here() const;                 // the active document and its caret
    bool show(const Place& place);      // activates or reopens it; false when it is gone
    void remember();                    // here() onto the back list; the forward list is dropped
    bool step(std::vector<Place>& from, std::vector<Place>& to, const char* done, const char* none);
    std::vector<Place> back_;
    std::vector<Place> forward_;

    NotepadViewModel& owner_;
    interfaces::IFileService* files_;

    model::SymbolIndex index_;
    bool built_ = false;
    bool building_ = false;
    int generation_ = 0; // bumped when the workspace changes: a late build result is dropped
    std::shared_ptr<std::atomic<bool>> cancel_; // set to stop the running build
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    std::string pendingDefinition_; // Go to Definition waiting for the index
    int pendingDocument_ = -1;

    bool active_ = false;
    bool explicit_ = false;
    bool member_ = false;       // after "." or "->": members
    std::string memberOf_;      // after "Name::": members of Name
    int documentId_ = -1;
    uint64_t revision_ = 0;
    model::TextPos caret_;
    model::TextPos anchor_;
    std::string prefix_;
    std::vector<Item> items_;
    int selected_ = 0;
    int selectionSerial_ = 0;

    Provider provider_;
    int requestSerial_ = 0;      // bumped by every request and when the list closes
    std::vector<Item> provided_; // the provider's answer to the current request
    bool awaiting_ = false;      // a provider's answer is on its way
    std::string providerStatus_;
    bool block_ = false;         // the list is the code of a comment
    int sessionSerial_ = 0;
    bool blockOnComment_ = false; // the caret was on the comment itself (the code goes below it)
    int blockFirst_ = 0, blockLast_ = 0; // the comment's lines

    // Enter below a comment: ask once the caret rests (code from comments).
    int pendingDocument2_ = -1;
    uint64_t pendingRevision_ = 0;
    model::TextPos pendingCaret_;
    double pendingSince_ = -1.0;

    // The standards the project asks for (0: none found) and where they were read.
    bool detected_ = false;
    int detectedCpp_ = 0, detectedC_ = 0;
    std::string cppSource_, cSource_;
};

} // namespace enp::viewmodel
