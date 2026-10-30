#include "enp/viewmodel/NotepadViewModel.h"

#include "enp/viewmodel/DummyData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

namespace enp::viewmodel {

namespace {

constexpr int kIndentWidth = 4;
constexpr float kMinZoom = 0.5f;
constexpr float kMaxZoom = 3.0f;
constexpr size_t kMaxUndo = 500;

const char* lineCommentToken(model::Language language)
{
    switch (language) {
    case model::Language::Cpp:
    case model::Language::JavaScript:
    case model::Language::Json:
        return "//";
    case model::Language::Python:
    case model::Language::CMake:
        return "#";
    case model::Language::PlainText:
    case model::Language::Markdown:
        break;
    }
    return nullptr;
}

std::string leadingWhitespace(const std::string& line)
{
    size_t n = 0;
    while (n < line.size() && (line[n] == ' ' || line[n] == '\t'))
        ++n;
    return line.substr(0, n);
}

std::string fileName(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

NotepadViewModel::NotepadViewModel()
    : commands_(size_t(CommandId::Count)), explorer_(dummy::workspace())
{
    initCommands();

    subscriptions_.push_back(review_.statusMessage.subscribe([this](const std::string& s) { statusText.set(s); }));
    subscriptions_.push_back(agent_.statusMessage.subscribe([this](const std::string& s) { statusText.set(s); }));
    subscriptions_.push_back(review_.navigateRequested.subscribe(
        [this](const std::string& path, const int& line) { navigateTo(path, line); }));
    subscriptions_.push_back(agent_.changeAccepted.subscribe(
        [this](const std::string& path, const std::string& text) { applyText(path, text); }));
    subscriptions_.push_back(findQuery.subscribe([this](const std::string&) { refreshFind(); }));
    subscriptions_.push_back(matchCase.subscribe([this](const bool&) { refreshFind(); }));
    subscriptions_.push_back(activeIndex.subscribe([this](const int&) { refreshFind(); }));
}

NotepadViewModel::~NotepadViewModel() = default;

// ---- Commands -----------------------------------------------------------------------------------

void NotepadViewModel::set(CommandId id, std::function<void()> execute, std::function<bool()> canExecute)
{
    commands_[size_t(id)] = Command(std::move(execute), std::move(canExecute));
}

void NotepadViewModel::notImplemented(CommandId id)
{
    statusText.set(std::string(commandInfo(id).label) + ": not available yet (no services connected)");
}

void NotepadViewModel::initCommands()
{
    using C = CommandId;
    auto hasDoc = [this]() { return activeDocument() != nullptr; };
    auto hasSelection = [this]() { return activeDocument() && activeDocument()->hasSelection(); };
    auto stub = [this](CommandId id) { return [this, id]() { notImplemented(id); }; };

    set(C::NewFile, [this]() { newDocument(); });
    set(C::OpenFile, stub(C::OpenFile));
    set(C::Save, [this]() {
        model::Document* doc = activeDocument();
        doc->savedText = doc->buffer.text();
        doc->modified = false;
        statusText.set("Saved " + doc->title + " (dummy: nothing was written to disk)");
    }, hasDoc);
    set(C::SaveAll, [this]() {
        for (auto& doc : documents_) {
            doc->savedText = doc->buffer.text();
            doc->modified = false;
        }
        statusText.set("Saved all documents (dummy: nothing was written to disk)");
    }, hasDoc);
    set(C::CloseFile, [this]() { closeDocument(activeIndex.get()); }, hasDoc);

    set(C::Cut, [this]() { clipboardRequested.emit(ClipboardAction::Cut); }, hasSelection);
    set(C::Copy, [this]() { clipboardRequested.emit(ClipboardAction::Copy); }, hasSelection);
    set(C::Paste, [this]() { clipboardRequested.emit(ClipboardAction::Paste); }, hasDoc);
    set(C::Undo, [this]() { undo(); }, [this]() { return canUndo(); });
    set(C::Redo, [this]() { redo(); }, [this]() { return canRedo(); });
    set(C::SelectAll, [this]() { selectAll(); }, hasDoc);

    set(C::DuplicateLine, [this]() { duplicateLines(); }, hasDoc);
    set(C::DeleteLine, [this]() { deleteLines(); }, hasDoc);
    set(C::MoveLineUp, [this]() { moveLines(-1); }, hasDoc);
    set(C::MoveLineDown, [this]() { moveLines(+1); }, hasDoc);
    set(C::SortLines, [this]() {
        transformLines([](std::vector<std::string>& lines) { std::sort(lines.begin(), lines.end()); });
    }, hasDoc);
    set(C::UpperCase, [this]() {
        transformSelection([](std::string s) {
            for (char& c : s)
                c = char(std::toupper(static_cast<unsigned char>(c)));
            return s;
        });
    }, hasDoc);
    set(C::LowerCase, [this]() {
        transformSelection([](std::string s) {
            for (char& c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        });
    }, hasDoc);
    set(C::ToggleComment, [this]() { toggleComment(); }, hasDoc);
    set(C::Indent, [this]() {
        transformLines([](std::vector<std::string>& lines) {
            for (std::string& l : lines)
                if (!l.empty())
                    l.insert(0, size_t(kIndentWidth), ' ');
        });
    }, hasDoc);
    set(C::Outdent, [this]() {
        transformLines([](std::vector<std::string>& lines) {
            for (std::string& l : lines) {
                if (!l.empty() && l[0] == '\t') {
                    l.erase(0, 1);
                    continue;
                }
                size_t n = 0;
                while (n < l.size() && n < size_t(kIndentWidth) && l[n] == ' ')
                    ++n;
                l.erase(0, n);
            }
        });
    }, hasDoc);
    set(C::TrimTrailing, [this]() { trimTrailing(); }, hasDoc);

    set(C::Find, [this]() {
        if (model::Document* doc = activeDocument(); doc && doc->hasSelection() && doc->anchor.line == doc->caret.line)
            findQuery.set(selectedText());
        findVisible.set(true);
        replaceVisible.set(false);
        focusFindRequested.emit();
    }, hasDoc);
    set(C::Replace, [this]() {
        findVisible.set(true);
        replaceVisible.set(true);
        focusFindRequested.emit();
    }, hasDoc);
    set(C::FindNext, [this]() { findNext(+1); }, hasDoc);
    set(C::FindPrevious, [this]() { findNext(-1); }, hasDoc);
    set(C::FindInFiles, stub(C::FindInFiles));
    set(C::GoToLine, [this]() { goToLineRequested.emit(); }, hasDoc);
    set(C::ToggleBookmark, [this]() { toggleBookmark(); }, hasDoc);
    set(C::NextBookmark, [this]() { jumpToBookmark(+1); }, hasDoc);
    set(C::PreviousBookmark, [this]() { jumpToBookmark(-1); }, hasDoc);
    set(C::ClearBookmarks, [this]() { activeDocument()->bookmarks.clear(); }, hasDoc);

    set(C::ToggleExplorer, [this]() { showExplorer.set(!showExplorer.get()); });
    set(C::ToggleReviewPanel, [this]() { showReviewPanel.set(!showReviewPanel.get()); });
    set(C::ToggleAgentPanel, [this]() { showAgentPanel.set(!showAgentPanel.get()); });
    set(C::ToggleWordWrap, [this]() { wordWrap.set(!wordWrap.get()); });
    set(C::ToggleWhitespace, [this]() { showWhitespace.set(!showWhitespace.get()); });
    set(C::ToggleLineNumbers, [this]() { showLineNumbers.set(!showLineNumbers.get()); });
    set(C::ToggleIndentGuides, [this]() { showIndentGuides.set(!showIndentGuides.get()); });
    set(C::ZoomIn, [this]() { setZoom(zoom.get() + 0.1f); }, [this]() { return zoom.get() < kMaxZoom; });
    set(C::ZoomOut, [this]() { setZoom(zoom.get() - 0.1f); }, [this]() { return zoom.get() > kMinZoom; });
    set(C::ZoomReset, [this]() { setZoom(1.0f); });
    set(C::ToggleTheme, [this]() { darkTheme.set(!darkTheme.get()); });

    auto setEol = [this](model::EolMode eol) {
        return [this, eol]() {
            activeDocument()->eol = eol;
            statusText.set(std::string("Line endings: ") + model::eolName(eol));
        };
    };
    set(C::EolCrLf, setEol(model::EolMode::CrLf), hasDoc);
    set(C::EolLf, setEol(model::EolMode::Lf), hasDoc);
    set(C::EolCr, setEol(model::EolMode::Cr), hasDoc);
    auto setEncoding = [this](model::Encoding encoding) {
        return [this, encoding]() {
            activeDocument()->encoding = encoding;
            statusText.set(std::string("Encoding: ") + model::encodingName(encoding));
        };
    };
    set(C::EncodingUtf8, setEncoding(model::Encoding::Utf8), hasDoc);
    set(C::EncodingUtf8Bom, setEncoding(model::Encoding::Utf8Bom), hasDoc);
    set(C::EncodingUtf16Le, setEncoding(model::Encoding::Utf16Le), hasDoc);
    set(C::EncodingUtf16Be, setEncoding(model::Encoding::Utf16Be), hasDoc);
    set(C::EncodingAnsi, setEncoding(model::Encoding::Ansi), hasDoc);
    auto setLanguage = [this](model::Language language) {
        return [this, language]() { activeDocument()->language = language; };
    };
    set(C::LanguagePlainText, setLanguage(model::Language::PlainText), hasDoc);
    set(C::LanguageCpp, setLanguage(model::Language::Cpp), hasDoc);
    set(C::LanguagePython, setLanguage(model::Language::Python), hasDoc);
    set(C::LanguageJavaScript, setLanguage(model::Language::JavaScript), hasDoc);
    set(C::LanguageMarkdown, setLanguage(model::Language::Markdown), hasDoc);
    set(C::LanguageJson, setLanguage(model::Language::Json), hasDoc);
    set(C::LanguageCMake, setLanguage(model::Language::CMake), hasDoc);

    auto runTool = [this](model::ReviewTool tool) {
        return [this, tool]() {
            review_.runTool(tool);
            showReviewPanel.set(true);
            reviewTab.set(ReviewTab::Problems);
        };
    };
    set(C::RunLint, runTool(model::ReviewTool::Lint));
    set(C::RunStaticAnalysis, runTool(model::ReviewTool::StaticAnalysis));
    set(C::RunSecurityScan, runTool(model::ReviewTool::Security));
    set(C::RunStyleCheck, runTool(model::ReviewTool::Style));
    set(C::RunComplexity, runTool(model::ReviewTool::Complexity));
    set(C::RunAgentReview, runTool(model::ReviewTool::AgentReview));
    set(C::RunAllReviews, [this]() {
        review_.runAll();
        showReviewPanel.set(true);
        reviewTab.set(ReviewTab::Problems);
    });
    set(C::CompareWithSaved, [this]() { compareWithSaved(); }, hasDoc);
    set(C::CompareAgentChanges, [this]() {
        if (const model::FileChange* change = agent_.currentChange())
            review_.setDiff("Agent change: " + change->path, change->diff);
        showReviewPanel.set(true);
        reviewTab.set(ReviewTab::Diff);
    }, [this]() { return agent_.currentChange() != nullptr; });
    set(C::AddReviewComment, [this]() { addCommentRequested.emit(); }, hasDoc);
    set(C::NextFinding, [this]() { review_.nextFinding(+1); });
    set(C::PreviousFinding, [this]() { review_.nextFinding(-1); });
    set(C::ClearFindings, [this]() { review_.clearFindings(); }, [this]() { return !review_.findings().empty(); });

    auto notBusy = [this]() { return !agent_.busy.get(); };
    set(C::NewChat, [this]() { agent_.newChat(); });
    set(C::AskAgent, [this]() {
        if (workspace.get() == WorkspaceMode::Code)
            showAgentPanel.set(true);
        focusPromptRequested.emit();
    });
    set(C::ExplainSelection, [this]() { askAgent("Explain this code."); }, [=]() { return hasDoc() && notBusy(); });
    set(C::RefactorSelection, [this]() { askAgent("Refactor this code for readability without changing behaviour."); },
        [=]() { return hasDoc() && notBusy(); });
    set(C::GenerateTests, [this]() { askAgent("Write unit tests for this file."); }, [=]() { return hasDoc() && notBusy(); });
    set(C::FixFindings, [this]() {
        askAgent("Fix the open review findings (" + std::to_string(review_.visibleFindings().size()) + ").");
    }, notBusy);
    set(C::AcceptAllChanges, [this]() { agent_.acceptAll(); }, [this]() { return agent_.pendingChangeCount() > 0; });
    set(C::RejectAllChanges, [this]() { agent_.rejectAll(); }, [this]() { return agent_.pendingChangeCount() > 0; });
    set(C::StopAgent, [this]() { agent_.stop(); }, [this]() { return agent_.busy.get(); });

    set(C::SwitchToAgentMode, [this]() { workspace.set(WorkspaceMode::Agent); });
    set(C::SwitchToCodeMode, [this]() { workspace.set(WorkspaceMode::Code); });
    set(C::ToggleWorkspaceMode, [this]() {
        workspace.set(workspace.get() == WorkspaceMode::Agent ? WorkspaceMode::Code : WorkspaceMode::Agent);
    });

    set(C::StartRecording, [this]() {
        recording.set(true);
        statusText.set("Recording macro... (dummy)");
    }, [this]() { return !recording.get(); });
    set(C::StopRecording, [this]() {
        recording.set(false);
        statusText.set("Macro recording stopped (dummy)");
    }, [this]() { return recording.get(); });
    set(C::PlayMacro, stub(C::PlayMacro));
    set(C::HashSha256, stub(C::HashSha256));
    set(C::Preferences, [this]() { preferencesRequested.emit(); });
    set(C::About, [this]() { aboutRequested.emit(); });
    set(C::Exit, [this]() { quitRequested.emit(); });
}

bool NotepadViewModel::isChecked(CommandId id) const
{
    using C = CommandId;
    const model::Document* doc = activeDocument();
    switch (id) {
    case C::ToggleExplorer: return showExplorer.get();
    case C::ToggleReviewPanel: return showReviewPanel.get();
    case C::ToggleAgentPanel: return showAgentPanel.get();
    case C::ToggleWordWrap: return wordWrap.get();
    case C::ToggleWhitespace: return showWhitespace.get();
    case C::ToggleLineNumbers: return showLineNumbers.get();
    case C::ToggleIndentGuides: return showIndentGuides.get();
    case C::ToggleTheme: return darkTheme.get();
    case C::StartRecording: return recording.get();
    case C::SwitchToAgentMode: return workspace.get() == WorkspaceMode::Agent;
    case C::SwitchToCodeMode: return workspace.get() == WorkspaceMode::Code;
    case C::EolCrLf: return doc && doc->eol == model::EolMode::CrLf;
    case C::EolLf: return doc && doc->eol == model::EolMode::Lf;
    case C::EolCr: return doc && doc->eol == model::EolMode::Cr;
    case C::EncodingUtf8: return doc && doc->encoding == model::Encoding::Utf8;
    case C::EncodingUtf8Bom: return doc && doc->encoding == model::Encoding::Utf8Bom;
    case C::EncodingUtf16Le: return doc && doc->encoding == model::Encoding::Utf16Le;
    case C::EncodingUtf16Be: return doc && doc->encoding == model::Encoding::Utf16Be;
    case C::EncodingAnsi: return doc && doc->encoding == model::Encoding::Ansi;
    case C::LanguagePlainText: return doc && doc->language == model::Language::PlainText;
    case C::LanguageCpp: return doc && doc->language == model::Language::Cpp;
    case C::LanguagePython: return doc && doc->language == model::Language::Python;
    case C::LanguageJavaScript: return doc && doc->language == model::Language::JavaScript;
    case C::LanguageMarkdown: return doc && doc->language == model::Language::Markdown;
    case C::LanguageJson: return doc && doc->language == model::Language::Json;
    case C::LanguageCMake: return doc && doc->language == model::Language::CMake;
    default: return false;
    }
}

// ---- Documents ----------------------------------------------------------------------------------

model::Document* NotepadViewModel::activeDocument()
{
    const int index = activeIndex.get();
    return index >= 0 && index < int(documents_.size()) ? documents_[size_t(index)].get() : nullptr;
}

const model::Document* NotepadViewModel::activeDocument() const
{
    const int index = activeIndex.get();
    return index >= 0 && index < int(documents_.size()) ? documents_[size_t(index)].get() : nullptr;
}

model::Document& NotepadViewModel::addDocument(std::string title, std::string path, const std::string& text)
{
    auto doc = std::make_unique<model::Document>();
    doc->id = nextDocumentId_++;
    doc->title = std::move(title);
    doc->path = std::move(path);
    doc->buffer.setText(text);
    doc->savedText = doc->buffer.text();
    doc->language = model::languageFromPath(doc->path.empty() ? doc->title : doc->path);
    if (doc->language == model::Language::Python || doc->language == model::Language::Markdown ||
        doc->language == model::Language::Json)
        doc->eol = model::EolMode::Lf;
    documents_.push_back(std::move(doc));
    activate(int(documents_.size()) - 1);
    return *documents_.back();
}

void NotepadViewModel::activate(int index)
{
    if (index < 0 || index >= int(documents_.size()))
        return;
    activeIndex.set(index);
    caretRevealRequested.emit();
}

void NotepadViewModel::closeDocument(int index)
{
    if (index < 0 || index >= int(documents_.size()))
        return;
    history_.erase(documents_[size_t(index)]->id);
    documents_.erase(documents_.begin() + index);
    int active = activeIndex.get();
    if (index < active || active >= int(documents_.size()))
        --active;
    activeIndex.set(-1); // force notification when the index value stays the same
    activeIndex.set(documents_.empty() ? -1 : std::max(0, active));
}

void NotepadViewModel::newDocument()
{
    addDocument("new " + std::to_string(++untitledCounter_), {}, {});
}

void NotepadViewModel::openFile(const std::string& path)
{
    for (int i = 0; i < int(documents_.size()); ++i)
        if (documents_[size_t(i)]->path == path) {
            activate(i);
            return;
        }
    addDocument(fileName(path), path, dummy::fileContent(path));
    statusText.set("Opened " + path + " (sample content)");
}

void NotepadViewModel::openStartupDocuments(const std::vector<std::string>& arguments)
{
    openFile(dummy::kWelcomePath);
    openFile("src/main.cpp");
    for (const std::string& argument : arguments)
        openFile(argument);
    statusText.set("Ready - running on sample data, no services connected");
}

void NotepadViewModel::navigateTo(const std::string& path, int line)
{
    openFile(path);
    goToLine(line);
    editorRequested.emit();
}

void NotepadViewModel::applyText(const std::string& path, const std::string& text)
{
    openFile(path);
    model::Document* doc = activeDocument();
    beginEdit(EditKind::Other);
    doc->buffer.setText(text);
    doc->caret = doc->anchor = doc->buffer.clamp(doc->caret);
    endEdit();
}

// ---- Editing ------------------------------------------------------------------------------------

void NotepadViewModel::setCaret(model::TextPos pos, bool extendSelection)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    doc->caret = doc->buffer.clamp(pos);
    if (!extendSelection)
        doc->anchor = doc->caret;
}

void NotepadViewModel::setSelection(model::TextPos anchor, model::TextPos caret)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    doc->anchor = doc->buffer.clamp(anchor);
    doc->caret = doc->buffer.clamp(caret);
}

void NotepadViewModel::selectAll()
{
    if (model::Document* doc = activeDocument())
        setSelection({0, 0}, doc->buffer.end());
}

std::string NotepadViewModel::selectedText() const
{
    const model::Document* doc = activeDocument();
    if (!doc || !doc->hasSelection())
        return {};
    return doc->buffer.text(doc->selectionStart(), doc->selectionEnd());
}

void NotepadViewModel::insertText(std::string_view text)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    beginEdit(text.size() == 1 && text[0] != '\n' ? EditKind::Typing : EditKind::Other);
    model::TextPos at = doc->buffer.erase(doc->selectionStart(), doc->selectionEnd());
    doc->caret = doc->anchor = doc->buffer.insert(at, text);
    endEdit();
}

void NotepadViewModel::newline()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    const std::string indent = leadingWhitespace(doc->buffer.line(doc->selectionStart().line));
    insertText("\n" + indent);
}

void NotepadViewModel::backspace()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    if (doc->hasSelection()) {
        deleteSelection();
        return;
    }
    if (doc->caret == model::TextPos{0, 0})
        return;
    beginEdit(EditKind::Deleting);
    doc->caret = doc->anchor = doc->buffer.erase(doc->buffer.prev(doc->caret), doc->caret);
    endEdit();
}

void NotepadViewModel::deleteForward()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    if (doc->hasSelection()) {
        deleteSelection();
        return;
    }
    if (doc->caret == doc->buffer.end())
        return;
    beginEdit(EditKind::Deleting);
    doc->caret = doc->anchor = doc->buffer.erase(doc->caret, doc->buffer.next(doc->caret));
    endEdit();
}

void NotepadViewModel::deleteSelection()
{
    model::Document* doc = activeDocument();
    if (!doc || !doc->hasSelection())
        return;
    beginEdit(EditKind::Other);
    doc->caret = doc->anchor = doc->buffer.erase(doc->selectionStart(), doc->selectionEnd());
    endEdit();
}

void NotepadViewModel::goToLine(int line)
{
    if (!activeDocument())
        return;
    setCaret({line, 0}, false);
    caretRevealRequested.emit();
}

std::string NotepadViewModel::selectionContextLabel() const
{
    const model::Document* doc = activeDocument();
    if (!doc)
        return {};
    const std::string name = doc->path.empty() ? doc->title : doc->path;
    const int first = doc->selectionStart().line + 1;
    const int last = doc->selectionEnd().line + 1;
    return name + ":" + std::to_string(first) + (last != first ? "-" + std::to_string(last) : std::string());
}

// ---- Undo ---------------------------------------------------------------------------------------

void NotepadViewModel::beginEdit(EditKind kind)
{
    model::Document* doc = activeDocument();
    History& h = history_[doc->id];
    // Consecutive typing (or deleting) at the caret coalesces into one undo step.
    const bool coalesce = kind != EditKind::Other && kind == h.lastKind && doc->caret == h.lastCaret &&
                          !doc->hasSelection();
    if (!coalesce) {
        h.undo.push_back({doc->buffer.text(), doc->caret, doc->anchor});
        if (h.undo.size() > kMaxUndo)
            h.undo.erase(h.undo.begin());
    }
    h.redo.clear();
    h.lastKind = kind;
}

void NotepadViewModel::endEdit()
{
    model::Document* doc = activeDocument();
    history_[doc->id].lastCaret = doc->caret;
    doc->modified = doc->buffer.text() != doc->savedText;
    refreshFind();
    caretRevealRequested.emit();
}

bool NotepadViewModel::canUndo() const
{
    const model::Document* doc = activeDocument();
    auto it = doc ? history_.find(doc->id) : history_.end();
    return it != history_.end() && !it->second.undo.empty();
}

bool NotepadViewModel::canRedo() const
{
    const model::Document* doc = activeDocument();
    auto it = doc ? history_.find(doc->id) : history_.end();
    return it != history_.end() && !it->second.redo.empty();
}

void NotepadViewModel::restore(const Snapshot& snapshot)
{
    model::Document* doc = activeDocument();
    doc->buffer.setText(snapshot.text);
    doc->caret = doc->buffer.clamp(snapshot.caret);
    doc->anchor = doc->buffer.clamp(snapshot.anchor);
    doc->modified = doc->buffer.text() != doc->savedText;
    refreshFind();
    caretRevealRequested.emit();
}

void NotepadViewModel::undo()
{
    model::Document* doc = activeDocument();
    History& h = history_[doc->id];
    h.redo.push_back({doc->buffer.text(), doc->caret, doc->anchor});
    Snapshot snapshot = std::move(h.undo.back());
    h.undo.pop_back();
    h.lastKind = EditKind::Other;
    restore(snapshot);
}

void NotepadViewModel::redo()
{
    model::Document* doc = activeDocument();
    History& h = history_[doc->id];
    h.undo.push_back({doc->buffer.text(), doc->caret, doc->anchor});
    Snapshot snapshot = std::move(h.redo.back());
    h.redo.pop_back();
    h.lastKind = EditKind::Other;
    restore(snapshot);
}

// ---- Line operations ----------------------------------------------------------------------------

void NotepadViewModel::selectedLines(int& first, int& last) const
{
    const model::Document* doc = activeDocument();
    first = doc->selectionStart().line;
    last = doc->selectionEnd().line;
    // A selection ending at the start of a line does not include that line.
    if (last > first && doc->selectionEnd().column == 0)
        --last;
}

void NotepadViewModel::transformLines(const std::function<void(std::vector<std::string>&)>& transform)
{
    model::Document* doc = activeDocument();
    int first = 0, last = 0;
    selectedLines(first, last);
    const bool hadSelection = doc->hasSelection();
    std::vector<std::string> lines;
    for (int l = first; l <= last; ++l)
        lines.push_back(doc->buffer.line(l));

    beginEdit(EditKind::Other);
    transform(lines);
    if (lines.empty())
        lines.emplace_back();
    if (first == 0 && last == doc->buffer.lineCount() - 1) {
        std::string joined;
        for (size_t k = 0; k < lines.size(); ++k)
            joined += (k ? "\n" : "") + lines[k];
        doc->buffer.setText(joined);
    } else {
        for (int l = last; l >= first; --l)
            doc->buffer.eraseLine(l);
        for (size_t k = 0; k < lines.size(); ++k)
            doc->buffer.insertLine(first + int(k), lines[k]);
    }
    const int newLast = first + int(lines.size()) - 1;
    if (hadSelection)
        setSelection({first, 0}, {newLast, int(doc->buffer.line(newLast).size())});
    else
        setCaret(doc->caret, false);
    endEdit();
}

void NotepadViewModel::transformSelection(const std::function<std::string(const std::string&)>& transform)
{
    model::Document* doc = activeDocument();
    if (!doc->hasSelection()) {
        transformLines([&](std::vector<std::string>& lines) {
            for (std::string& l : lines)
                l = transform(l);
        });
        return;
    }
    const model::TextPos start = doc->selectionStart();
    const std::string replaced = transform(selectedText());
    insertText(replaced);
    setSelection(start, doc->caret);
}

void NotepadViewModel::duplicateLines()
{
    model::Document* doc = activeDocument();
    int first = 0, last = 0;
    selectedLines(first, last);
    beginEdit(EditKind::Other);
    for (int l = last; l >= first; --l)
        doc->buffer.insertLine(last + 1, doc->buffer.line(l));
    const int count = last - first + 1;
    doc->caret.line += count;
    doc->anchor.line += count;
    endEdit();
}

void NotepadViewModel::deleteLines()
{
    model::Document* doc = activeDocument();
    int first = 0, last = 0;
    selectedLines(first, last);
    beginEdit(EditKind::Other);
    for (int l = last; l >= first; --l)
        doc->buffer.eraseLine(l);
    setCaret({first, 0}, false);
    endEdit();
}

void NotepadViewModel::moveLines(int direction)
{
    model::Document* doc = activeDocument();
    int first = 0, last = 0;
    selectedLines(first, last);
    if ((direction < 0 && first == 0) || (direction > 0 && last + 1 >= doc->buffer.lineCount()))
        return;
    beginEdit(EditKind::Other);
    if (direction < 0) {
        std::string above = doc->buffer.line(first - 1);
        doc->buffer.eraseLine(first - 1);
        doc->buffer.insertLine(last, std::move(above));
    } else {
        std::string below = doc->buffer.line(last + 1);
        doc->buffer.eraseLine(last + 1);
        doc->buffer.insertLine(first, std::move(below));
    }
    doc->caret.line += direction;
    doc->anchor.line += direction;
    endEdit();
}

void NotepadViewModel::toggleComment()
{
    model::Document* doc = activeDocument();
    const char* token = lineCommentToken(doc->language);
    if (!token) {
        statusText.set(std::string("No line comment syntax for ") + model::languageName(doc->language));
        return;
    }
    const std::string prefix = std::string(token) + " ";
    transformLines([&](std::vector<std::string>& lines) {
        bool allCommented = true;
        size_t indent = std::string::npos;
        for (const std::string& l : lines) {
            const size_t ws = leadingWhitespace(l).size();
            if (ws == l.size())
                continue; // blank lines do not count
            indent = std::min(indent, ws);
            if (l.compare(ws, std::strlen(token), token) != 0)
                allCommented = false;
        }
        if (indent == std::string::npos)
            return;
        for (std::string& l : lines) {
            const size_t ws = leadingWhitespace(l).size();
            if (ws == l.size())
                continue;
            if (allCommented) {
                const size_t len = l.compare(ws, prefix.size(), prefix) == 0 ? prefix.size() : std::strlen(token);
                l.erase(ws, len);
            } else {
                l.insert(indent, prefix);
            }
        }
    });
}

void NotepadViewModel::trimTrailing()
{
    model::Document* doc = activeDocument();
    beginEdit(EditKind::Other);
    for (int l = 0; l < doc->buffer.lineCount(); ++l) {
        std::string line = doc->buffer.line(l);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t'))
            line.pop_back();
        doc->buffer.setLine(l, std::move(line));
    }
    doc->caret = doc->buffer.clamp(doc->caret);
    doc->anchor = doc->buffer.clamp(doc->anchor);
    endEdit();
}

void NotepadViewModel::toggleBookmark()
{
    model::Document* doc = activeDocument();
    const int line = doc->caret.line;
    if (!doc->bookmarks.erase(line))
        doc->bookmarks.insert(line);
}

void NotepadViewModel::jumpToBookmark(int direction)
{
    model::Document* doc = activeDocument();
    if (doc->bookmarks.empty()) {
        statusText.set("No bookmarks in " + doc->title);
        return;
    }
    const int line = doc->caret.line;
    int target = direction > 0 ? *doc->bookmarks.begin() : *doc->bookmarks.rbegin();
    if (direction > 0) {
        auto it = doc->bookmarks.upper_bound(line);
        if (it != doc->bookmarks.end())
            target = *it;
    } else {
        auto it = doc->bookmarks.lower_bound(line);
        if (it != doc->bookmarks.begin())
            target = *std::prev(it);
    }
    goToLine(target);
}

void NotepadViewModel::compareWithSaved()
{
    const model::Document* doc = activeDocument();
    std::vector<model::DiffRow> rows = model::diffLines(model::splitLines(doc->savedText), model::splitLines(doc->buffer.text()));
    const model::DiffStats stats = model::diffStats(rows);
    review_.setDiff(doc->title + ": saved vs current", std::move(rows));
    showReviewPanel.set(true);
    reviewTab.set(ReviewTab::Diff);
    statusText.set("Compare with saved: +" + std::to_string(stats.added) + " -" + std::to_string(stats.removed));
}

void NotepadViewModel::askAgent(const std::string& prompt)
{
    if (workspace.get() == WorkspaceMode::Code)
        showAgentPanel.set(true);
    agent_.submitPrompt(prompt, selectionContextLabel());
}

void NotepadViewModel::setZoom(float value)
{
    zoom.set(std::clamp(std::round(value * 10.0f) / 10.0f, kMinZoom, kMaxZoom));
}

// ---- Find ---------------------------------------------------------------------------------------

void NotepadViewModel::refreshFind()
{
    findMatches_.clear();
    currentMatch.set(-1);
    const model::Document* doc = activeDocument();
    const std::string& query = findQuery.get();
    if (!doc || query.empty())
        return;
    auto fold = [this](std::string s) {
        if (!matchCase.get())
            for (char& c : s)
                c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string needle = fold(query);
    for (int l = 0; l < doc->buffer.lineCount(); ++l) {
        const std::string hay = fold(doc->buffer.line(l));
        for (size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + needle.size()))
            findMatches_.push_back({{l, int(pos)}, {l, int(pos + needle.size())}});
    }
}

void NotepadViewModel::findNext(int direction)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    if (findMatches_.empty()) {
        statusText.set(findQuery.get().empty() ? "Nothing to find" : "Can't find \"" + findQuery.get() + "\"");
        return;
    }
    const int n = int(findMatches_.size());
    int index = direction > 0 ? 0 : n - 1;
    if (direction > 0) {
        for (int i = 0; i < n; ++i)
            if (doc->selectionEnd() <= findMatches_[size_t(i)].start) {
                index = i;
                break;
            }
    } else {
        for (int i = n - 1; i >= 0; --i)
            if (findMatches_[size_t(i)].end <= doc->selectionStart()) {
                index = i;
                break;
            }
    }
    currentMatch.set(index);
    setSelection(findMatches_[size_t(index)].start, findMatches_[size_t(index)].end);
    caretRevealRequested.emit();
    statusText.set("Match " + std::to_string(index + 1) + " of " + std::to_string(n));
}

void NotepadViewModel::replaceCurrent()
{
    model::Document* doc = activeDocument();
    const int index = currentMatch.get();
    if (!doc || index < 0 || index >= int(findMatches_.size()) ||
        findMatches_[size_t(index)].start != doc->selectionStart() || findMatches_[size_t(index)].end != doc->selectionEnd()) {
        findNext(+1);
        return;
    }
    insertText(replaceText.get());
    findNext(+1);
}

void NotepadViewModel::replaceAll()
{
    model::Document* doc = activeDocument();
    if (!doc || findMatches_.empty())
        return;
    const size_t count = findMatches_.size();
    beginEdit(EditKind::Other);
    for (auto it = findMatches_.rbegin(); it != findMatches_.rend(); ++it) {
        const model::TextPos at = doc->buffer.erase(it->start, it->end);
        doc->buffer.insert(at, replaceText.get());
    }
    doc->caret = doc->anchor = doc->buffer.clamp(doc->caret);
    endEdit();
    statusText.set("Replaced " + std::to_string(count) + " occurrence(s)");
}

void NotepadViewModel::tick(double now)
{
    agent_.tick(now);
}

} // namespace enp::viewmodel
