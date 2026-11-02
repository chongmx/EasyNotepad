#include "enp/viewmodel/NotepadViewModel.h"

#include "enp/model/BinaryInfo.h"
#include "enp/model/Json.h"
#include "enp/model/WorkspaceFile.h"
#include "enp/viewmodel/DummyData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "enp/os/Memory.h"

#include <deque>

namespace enp::viewmodel {

namespace {

constexpr int kIndentWidth = 4;
constexpr float kMinZoom = 0.5f;
constexpr float kMaxZoom = 3.0f;
constexpr size_t kMaxUndo = 500;

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

NotepadViewModel::NotepadViewModel(interfaces::ITerminalService* terminalService, interfaces::IFileService* fileService,
                                   interfaces::IAgentCliService* agentCli, interfaces::IProcessService* processService,
                                   interfaces::ILocalEngineService* localEngine)
    : commands_(size_t(CommandId::Count)), explorer_(dummy::workspace()), files_(fileService), engineLab_(localEngine),
      terminal_(terminalService),
      logicalMap_(*this, agentCli), fileDialog_(*this, fileService),
      findInFiles_(*this, fileService), settings_(fileService)
{
    agentCli_ = agentCli;
    terminal_.setWorkingDirectory(files_ ? files_->homeDirectory() : std::string()); // no folder yet: home
    processes_ = processService;
    git_.setServices(processService, fileService, agentCli);
    initCommands();
    pinned_ = {CommandId::DetailOverview, CommandId::ToggleLayersMap, CommandId::RunAllReviews, CommandId::ToggleTerminal};
    bindSettings();

    subscriptions_.push_back(review_.statusMessage.subscribe([this](const std::string& s) { statusText.set(s); }));
    subscriptions_.push_back(agent_.statusMessage.subscribe([this](const std::string& s) { statusText.set(s); }));
    subscriptions_.push_back(engineLab_.statusMessage.subscribe([this](const std::string& s) { statusText.set(s); }));
    subscriptions_.push_back(showHiddenFiles.subscribe([this](const bool&) { refreshExplorer(); }));
    subscriptions_.push_back(engineLab_.changed.subscribe([this]() { settings_.markDirty(); }));
    // The local agent uses the Engine Lab's model files and settings.
    agent_.setLocal(
        localEngine,
        [this]() {
            AgentViewModel::LocalSetup s;
            using Which = EngineLabViewModel::Which;
            if (!engineLab_.hasEngine()) {
                s.why = "this build has no local model.";
                return s;
            }
            if (const interfaces::EngineInfo& info = engineLab_.info(); !info.available) {
                s.why = info.unavailableReason;
                return s;
            }
            if (!engineLab_.fileUsable(Which::Model)) {
                s.why = "the model is not downloaded yet (agent set-up, the gear).";
                return s;
            }
            s.ready = true;
            s.modelPath = engineLab_.pathFor(Which::Model);
            if (engineLab_.fileUsable(Which::Drafter))
                s.drafterPath = engineLab_.pathFor(Which::Drafter);
            s.threads = engineLab_.threads.get();
            s.gpu = engineLab_.useGpu.get();
            return s;
        },
        [this]() { return agentContextText(); });
    subscriptions_.push_back(terminal_.statusMessage.subscribe([this](const std::string& s) { statusText.set(s); }));
    subscriptions_.push_back(agent_.replied.subscribe([this](const std::string& prompt) { pointer_.guideFor(prompt); }));
    if (agentCli_) {
        agentCli_->setCommand(agent_.command.get());
        subscriptions_.push_back(agent_.command.subscribe([this](const std::string& c) { agentCli_->setCommand(c); }));
    }
    subscriptions_.push_back(logicalMap_.statusMessage.subscribe([this](const std::string& s) { statusText.set(s); }));
    subscriptions_.push_back(foldingMode.subscribe([this](const model::OutlineMode& mode) {
        for (auto& doc : documents_)
            doc->detailLevel = -1; // regions changed: re-apply the detail level
        if (model::Document* doc = activeDocument())
            applyDetailLevel(*doc);
        statusText.set(mode == model::OutlineMode::Logical
                           ? "Logical folding: function bodies fold into their steps"
                           : "Syntax folding: regions follow the code's structure");
    }));
    subscriptions_.push_back(editorMode.subscribe([this](const EditorMode& mode) {
        if (mode == EditorMode::Map)
            logicalMap_.revealCaret(); // context aware: start where the user is reading
    }));
    subscriptions_.push_back(review_.navigateRequested.subscribe(
        [this](const std::string& path, const int& line) { navigateTo(path, line); }));
    subscriptions_.push_back(agent_.changeAccepted.subscribe(
        [this](const std::string& path, const std::string& text) { applyText(path, text); }));
    subscriptions_.push_back(findQuery.subscribe([this](const std::string&) { refreshFind(); }));
    subscriptions_.push_back(matchCase.subscribe([this](const bool&) { refreshFind(); }));
    subscriptions_.push_back(activeIndex.subscribe([this](const int&) {
        refreshFind();
        if (model::Document* doc = activeDocument(); doc && doc->detailLevel != detailLevel.get())
            applyDetailLevel(*doc);
    }));
    subscriptions_.push_back(detailLevel.subscribe([this](const int& level) {
        if (model::Document* doc = activeDocument())
            applyDetailLevel(*doc);
        statusText.set(std::string("Detail: ") + detailLevelName(level) + " - " + detailLevelDescription(level));
    }));
}

NotepadViewModel::~NotepadViewModel()
{
    settings_.markDirty(); // the session (carets, the active tab) is always written on exit
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
    os::mem::Scope scope("documents");
    auto doc = std::make_unique<model::Document>();
    doc->id = nextDocumentId_++;
    doc->title = std::move(title);
    doc->path = std::move(path);
    doc->buffer.setText(text);
    doc->savedText = doc->buffer.text();
    doc->language = model::languageFromPath(doc->path.empty() ? doc->title : doc->path);
    // Opened files are read first: Markdown formatted, CSV as a table (JSON and YAML open as text,
    // usually to be edited; their tree and accordion are a click away).
    doc->rendered = (doc->language == model::Language::Markdown || doc->language == model::Language::Csv) && !doc->path.empty();
    if (model::languageSpec(doc->language).lfByDefault)
        doc->eol = model::EolMode::Lf;
    applyDetectedIndent(*doc);
    documents_.push_back(std::move(doc));
    activate(int(documents_.size()) - 1);
    return *documents_.back();
}

void NotepadViewModel::activate(int index)
{
    if (index < 0 || index >= int(documents_.size()))
        return;
    activeIndex.set(index);
    ++revealSerial_;
    caretRevealRequested.emit();
}

int NotepadViewModel::documentIndexById(int id) const
{
    for (int i = 0; i < int(documents_.size()); ++i)
        if (documents_[size_t(i)]->id == id)
            return i;
    return -1;
}

bool NotepadViewModel::canSaveInPlace(const model::Document& doc) const
{
    return !doc.path.empty();
}

void NotepadViewModel::requestCloseDocument(int index)
{
    if (index < 0 || index >= int(documents_.size()))
        return;
    const model::Document& doc = *documents_[size_t(index)];
    if (!doc.modified) {
        closeDocument(index);
        return;
    }
    activate(index); // show what is about to be lost
    pendingClose.set(doc.id);
}

void NotepadViewModel::resolvePendingClose(CloseChoice choice)
{
    const int index = documentIndexById(pendingClose.get());
    pendingClose.set(-1);
    if (index < 0 || choice == CloseChoice::Cancel)
        return;
    model::Document& doc = *documents_[size_t(index)];
    if (choice == CloseChoice::Save) {
        if (!canSaveInPlace(doc)) {
            if (files_) { // untitled: Save As, closing it once saved
                execute(CommandId::SaveAs);
                fileDialog_.closeAfterSave = doc.id;
            }
            return;
        }
        if (!saveDocument(doc))
            return; // not saved: keep it open
    }
    closeDocument(index);
}

std::vector<const model::Document*> NotepadViewModel::unsavedDocuments() const
{
    std::vector<const model::Document*> out;
    for (const auto& doc : documents_)
        if (doc->modified)
            out.push_back(doc.get());
    return out;
}

void NotepadViewModel::requestQuit()
{
    if (unsavedDocuments().empty()) {
        quitRequested.emit();
        return;
    }
    quitConfirmPending.set(true);
}

void NotepadViewModel::resolveQuit(CloseChoice choice)
{
    quitConfirmPending.set(false);
    if (choice == CloseChoice::Cancel)
        return;
    if (choice == CloseChoice::Save) {
        for (auto& doc : documents_)
            if (doc->modified && (!canSaveInPlace(*doc) || !saveDocument(*doc))) {
                activate(documentIndexById(doc->id));
                statusText.set("Not quitting: \"" + doc->title + "\" could not be saved");
                return;
            }
    }
    quitRequested.emit();
}

void NotepadViewModel::requestCloseFolder()
{
    if (workspaceRoot_.empty())
        return;
    if (unsavedDocuments().empty())
        closeFolder();
    else
        closeFolderPending.set(true);
}

void NotepadViewModel::resolveCloseFolder(CloseChoice choice)
{
    closeFolderPending.set(false);
    if (choice == CloseChoice::Cancel)
        return;
    if (choice == CloseChoice::Save)
        for (auto& doc : documents_)
            if (doc->modified && (!canSaveInPlace(*doc) || !saveDocument(*doc))) {
                activate(documentIndexById(doc->id));
                statusText.set("Not closing the folder: \"" + doc->title + "\" could not be saved");
                return;
            }
    closeFolder();
}

void NotepadViewModel::closeFolder()
{
    const std::string closed = workspaceRoot_;
    for (auto& doc : documents_)
        doc->modified = false; // answered already: closing discards
    while (!documents_.empty())
        closeDocument(0);
    os::mem::release(workspaceRoot_);
    workspaceIndent_.reset();
    terminal_.setWorkingDirectory(files_ ? files_->homeDirectory() : std::string()); // no folder: home
    os::mem::release(explorer_.children);
    os::mem::release(explorer_.name);
    os::mem::release(explorer_.path);
    explorer_ = ExplorerItem{"", "", true, {}}; // empty: "No folder open", not the samples
    findInFiles_.folder.clear();
    findInFiles_.clear(); // the hits (and their lines) were the closed folder's
    layers_.workspaceChanged();
    logicalMap_.workspaceChanged();
    git_.workspaceChanged();
    statusText.set(closed.empty() ? "No folder open" : "Closed the folder " + closed);
    memoryTrimRequested_ = true;
}

bool NotepadViewModel::takeMemoryTrimRequest()
{
    const bool requested = memoryTrimRequested_;
    memoryTrimRequested_ = false;
    return requested;
}

void NotepadViewModel::closeWhere(const std::function<bool(int index, const model::Document&)>& close)
{
    const model::Document* active = activeDocument();
    std::vector<std::unique_ptr<model::Document>> kept;
    int unsaved = 0;
    int keptActive = -1;
    for (int i = 0; i < int(documents_.size()); ++i) {
        std::unique_ptr<model::Document>& doc = documents_[size_t(i)];
        if (close(i, *doc) && !doc->modified) {
            history_.forget(doc->id);
            if (doc->kind != model::FileKind::Text)
                memoryTrimRequested_ = true; // its view held a decoded image or a library's contents
            continue;
        }
        if (close(i, *doc))
            ++unsaved;
        if (doc.get() == active)
            keptActive = int(kept.size());
        kept.push_back(std::move(doc));
    }
    documents_ = std::move(kept);
    if (documents_.empty())
        memoryTrimRequested_ = true;
    if (unsaved > 0)
        statusText.set("Kept " + std::to_string(unsaved) + " document(s) open: unsaved changes");
    activeIndex.set(-1); // force notification
    activeIndex.set(documents_.empty() ? -1 : keptActive >= 0 ? keptActive : 0);
    ++revealSerial_;
}

void NotepadViewModel::closeOthers(int index)
{
    if (index < 0 || index >= int(documents_.size()))
        return;
    activate(index);
    closeWhere([index](int i, const model::Document&) { return i != index; });
}

void NotepadViewModel::closeToTheRight(int index)
{
    closeWhere([index](int i, const model::Document&) { return i > index; });
}

void NotepadViewModel::closeSaved()
{
    closeWhere([](int, const model::Document&) { return true; }); // modified ones are kept anyway
    statusText.set(documents_.empty() ? "Closed all documents" : "Closed the saved documents");
}

void NotepadViewModel::closeAll()
{
    closeWhere([](int, const model::Document&) { return true; });
}

void NotepadViewModel::moveDocument(int from, int to)
{
    const int count = int(documents_.size());
    if (from < 0 || from >= count || to < 0 || to >= count || from == to)
        return;
    const model::Document* active = activeDocument();
    std::unique_ptr<model::Document> doc = std::move(documents_[size_t(from)]);
    documents_.erase(documents_.begin() + from);
    documents_.insert(documents_.begin() + to, std::move(doc));
    for (int i = 0; i < count; ++i)
        if (documents_[size_t(i)].get() == active)
            activeIndex.set(i);
}

int NotepadViewModel::documentIndex(const std::string& path) const
{
    for (int i = 0; i < int(documents_.size()); ++i)
        if (documents_[size_t(i)]->path == path && !path.empty())
            return i;
    return -1;
}

std::string NotepadViewModel::documentHint(int index) const
{
    if (index < 0 || index >= int(documents_.size()))
        return {};
    const model::Document& doc = *documents_[size_t(index)];
    auto folders = [](const std::string& path) {
        std::vector<std::string> parts;
        size_t start = 0;
        for (size_t slash = path.find('/'); slash != std::string::npos; slash = path.find('/', start)) {
            parts.push_back(path.substr(start, slash - start));
            start = slash + 1;
        }
        return parts; // without the file name
    };
    const std::vector<std::string> mine = folders(doc.path);
    size_t depth = 0; // how many trailing folders tell this one apart from every namesake
    for (int i = 0; i < int(documents_.size()); ++i) {
        const model::Document& other = *documents_[size_t(i)];
        if (i == index || other.title != doc.title)
            continue;
        const std::vector<std::string> theirs = folders(other.path);
        size_t k = 1;
        while (k <= mine.size() && k <= theirs.size() && mine[mine.size() - k] == theirs[theirs.size() - k])
            ++k;
        depth = std::max(depth, std::min(k, mine.size()));
    }
    std::string hint;
    for (size_t k = mine.size() - depth; k < mine.size(); ++k)
        hint += (hint.empty() ? "" : "/") + mine[k];
    return hint;
}

void NotepadViewModel::closeDocument(int index)
{
    settings_.markDirty(); // the session
    if (index < 0 || index >= int(documents_.size()))
        return;
    history_.forget(documents_[size_t(index)]->id);
    // An image or a library: what its view decoded is large, so the pages go back to the system now.
    const bool large = documents_[size_t(index)]->kind != model::FileKind::Text;
    documents_.erase(documents_.begin() + index);
    if (documents_.empty() || large)
        memoryTrimRequested_ = true;
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
            if (documents_[size_t(i)]->onDisk)
                remember(recentFiles_, absolutePath(path), kMaxRecentFiles); // opened again: most recent
            return;
        }
    const bool absolute = FileDialogViewModel::isAbsolute(path);
    if (absolute && !workspaceRelative(path).empty() && workspaceRelative(path) != path) {
        openFile(workspaceRelative(path)); // inside the workspace: one document per file, relative path
        return;
    }
    if ((workspaceRoot_.empty() && !absolute) || !files_) {
        addDocument(fileName(path), path, dummy::fileContent(path));
        statusText.set("Opened " + path + " (sample content)");
        return;
    }
    std::string error;
    os::mem::Scope scope("documents");
    std::optional<std::string> text = files_->read(absolutePath(path), &error);
    if (!text) {
        statusText.set("Cannot open " + path + ": " + error);
        return;
    }
    // Images are shown, binary files are not poured into a text buffer: both get a document
    // whose buffer stays empty (the editor shows the image, or a notice with "Open as text").
    if (const model::FileType type = model::detectFileType(path, *text); type.kind != model::FileKind::Text) {
        const bool library = type.kind == model::FileKind::Binary && model::bin::recognises(*text);
        os::mem::release(*text);
        model::Document& doc = addDocument(fileName(path), path, {});
        doc.kind = type.kind;
        doc.format = type.format;
        doc.rendered = false;
        doc.onDisk = true;
        recordDiskStamp(doc);
        remember(recentFiles_, absolutePath(path), kMaxRecentFiles);
        statusText.set(type.kind == model::FileKind::Image ? "Opened " + path + " (" + type.format + " image)"
                       : library ? "Opened " + path + " (" + type.format + "): its functions, imports and sections are listed"
                                 : path + " is a binary file (" + type.format + "): not opened as text");
        return;
    }
    // Keep the file's conventions: BOM and line endings are restored on save.
    bool bom = text->rfind("\xEF\xBB\xBF", 0) == 0;
    if (bom)
        text->erase(0, 3);
    const bool crlf = text->find("\r\n") != std::string::npos;
    model::Document& doc = addDocument(fileName(path), path, *text);
    doc.onDisk = true;
    recordDiskStamp(doc);
    remember(recentFiles_, absolutePath(path), kMaxRecentFiles);
    doc.eol = crlf ? model::EolMode::CrLf : model::EolMode::Lf;
    doc.encoding = bom ? model::Encoding::Utf8Bom : model::Encoding::Utf8;
    statusText.set("Opened " + path);
}

void NotepadViewModel::setDataView(model::DataView view)
{
    model::Document* doc = activeDocument();
    if (!doc || (doc->language != model::Language::Json && doc->language != model::Language::Yaml) || doc->dataView == view)
        return;
    doc->dataView = view;
    if (view == model::DataView::Text)
        revealLine(doc->caret.line);
}

bool NotepadViewModel::openAsText(int index)
{
    if (index < 0 || index >= int(documents_.size()) || !files_)
        return false;
    model::Document& doc = *documents_[size_t(index)];
    if (doc.kind == model::FileKind::Text)
        return true;
    std::string error;
    os::mem::Scope scope("documents");
    std::optional<std::string> text = files_->read(absolutePath(doc.path), &error);
    if (!text) {
        statusText.set("Cannot open " + doc.path + ": " + error);
        return false;
    }
    const bool bom = text->rfind("\xEF\xBB\xBF", 0) == 0;
    if (bom)
        text->erase(0, 3);
    doc.eol = text->find("\r\n") != std::string::npos ? model::EolMode::CrLf : model::EolMode::Lf;
    doc.encoding = bom ? model::Encoding::Utf8Bom : model::Encoding::Utf8;
    doc.buffer.setText(*text);
    doc.savedText = doc.buffer.text();
    doc.kind = model::FileKind::Text;
    doc.modified = false;
    ++doc.revision;
    applyDetectedIndent(doc);
    recordDiskStamp(doc);
    statusText.set("Opened " + doc.path + " as text (" + doc.format + "): saving would rewrite the file as text");
    return true;
}

std::string NotepadViewModel::absolutePath(const std::string& path) const
{
    const bool absolute = !path.empty() && (path[0] == '/' || (path.size() > 1 && path[1] == ':'));
    return absolute || workspaceRoot_.empty() ? path : workspaceRoot_ + "/" + path;
}

bool NotepadViewModel::saveDocument(model::Document& doc)
{
    if (doc.kind != model::FileKind::Text) { // nothing was loaded: writing the empty buffer would destroy the file
        statusText.set(doc.title + " is shown as " + (doc.kind == model::FileKind::Image ? "an image" : "a binary file") +
                       ": there is nothing to save");
        return false;
    }
    if (!doc.onDisk || !files_) {
        doc.savedText = doc.buffer.text();
        doc.modified = false;
        statusText.set("Saved " + doc.title + " (sample document: nothing was written to disk)");
        return true;
    }
    if (doc.hasDiskStamp) {
        const std::optional<interfaces::FileStamp> now = files_->stamp(absolutePath(doc.path));
        if (now && now->exists && (now->size != doc.diskSize || now->modified != doc.diskModified)) {
            doc.diskState = model::Document::DiskState::Changed;
            statusText.set(doc.title + " changed on disk since it was opened: Reload, Keep mine (then save), or Compare");
            return false;
        }
    }
    const char* eol = doc.eol == model::EolMode::CrLf ? "\r\n" : doc.eol == model::EolMode::Cr ? "\r" : "\n";
    std::string data = doc.buffer.text(eol);
    if (doc.encoding == model::Encoding::Utf8Bom)
        data.insert(0, "\xEF\xBB\xBF");
    std::string error;
    if (!files_->write(absolutePath(doc.path), data, &error)) {
        statusText.set("Cannot save " + doc.path + ": " + error);
        return false;
    }
    doc.savedText = doc.buffer.text();
    doc.modified = false;
    recordDiskStamp(doc);
    statusText.set("Saved " + doc.path);
    git_.documentSaved(doc.path, doc.savedText);
    return true;
}

void NotepadViewModel::refreshExplorer()
{
    if (workspaceRoot_.empty() || !files_)
        return;
    os::mem::Scope scope("explorer");
    // Breadth-first, so that a big folder early in the order cannot crowd out the others (a
    // depth-first scan with a budget used to show a repository as a part of its first folder and
    // nothing else): every top-level entry shows, and the budget runs out in the depths, where
    // folders are read when expanded instead.
    ExplorerItem tree{workspaceRoot_.substr(workspaceRoot_.find_last_of('/') + 1), "", true, {}};
    tree.scanned = false;
    std::deque<ExplorerItem*> pending{&tree};
    int budget = explorerLimits_.total;
    while (!pending.empty() && budget > 0) {
        ExplorerItem& folder = *pending.front();
        pending.pop_front();
        scanExplorerFolder(folder);
        budget -= int(folder.children.size());
        const int depth = folder.path.empty() ? 0 : int(std::count(folder.path.begin(), folder.path.end(), '/')) + 1;
        if (depth + 1 > explorerLimits_.depth)
            continue;
        for (ExplorerItem& child : folder.children) // this vector is final now: the pointers stay valid
            if (child.isDirectory)
                pending.push_back(&child);
    }
    explorer_ = std::move(tree);
}

// Reads one folder's entries, up to the per-folder limit. Version control, build output and other
// bulk folders at the top level are left out; hidden entries unless showHiddenFiles.
void NotepadViewModel::scanExplorerFolder(ExplorerItem& folder)
{
    static const char* const skipped[] = {".git", "build", "bin", "out", "ref", "node_modules", "_deps"};
    const bool hidden = showHiddenFiles.get();
    bool more = false;
    const std::string absolute = folder.path.empty() ? workspaceRoot_ : workspaceRoot_ + "/" + folder.path;
    folder.children.clear();
    for (const interfaces::DirectoryEntry& e : files_->list(absolute, size_t(std::max(1, explorerLimits_.perFolder)), &more)) {
        if (e.name.empty() || (e.hidden && !hidden))
            continue;
        if (e.isDirectory && folder.path.empty() && std::find(std::begin(skipped), std::end(skipped), e.name) != std::end(skipped))
            continue;
        ExplorerItem item{e.name, folder.path.empty() ? e.name : folder.path + "/" + e.name, e.isDirectory, {}};
        item.scanned = !e.isDirectory; // folders: read later (breadth-first, or when expanded)
        folder.children.push_back(std::move(item));
    }
    folder.truncated = more;
    folder.scanned = true;
}

void NotepadViewModel::expandExplorer(const std::string& path)
{
    if (workspaceRoot_.empty() || !files_)
        return;
    auto find = [](auto&& self, ExplorerItem& item, const std::string& p) -> ExplorerItem* {
        if (item.path == p)
            return &item;
        for (ExplorerItem& child : item.children)
            if (child.isDirectory && (child.path == p || p.compare(0, child.path.size() + 1, child.path + "/") == 0))
                return self(self, child, p);
        return nullptr;
    };
    ExplorerItem* folder = find(find, explorer_, path);
    if (folder && folder->isDirectory && !folder->scanned) {
        os::mem::Scope scope("explorer");
        scanExplorerFolder(*folder);
    }
}

void NotepadViewModel::remember(std::vector<std::string>& list, const std::string& path, size_t max)
{
    std::string p = path;
    std::replace(p.begin(), p.end(), '\\', '/');
    list.erase(std::remove(list.begin(), list.end(), p), list.end());
    list.insert(list.begin(), p);
    if (list.size() > max)
        list.resize(max);
    settings_.markDirty();
}

void NotepadViewModel::openRecent(const std::string& path)
{
    const bool folder = std::find(recentFolders_.begin(), recentFolders_.end(), path) != recentFolders_.end();
    if (folder ? openFolder(path) : (files_ && files_->read(path).has_value())) {
        if (!folder)
            openFile(path);
        return;
    }
    // Gone: forget it.
    for (std::vector<std::string>* list : {&recentFiles_, &recentFolders_})
        list->erase(std::remove(list->begin(), list->end(), path), list->end());
    settings_.markDirty();
    statusText.set(path + " no longer exists; removed from the recent list");
}

void NotepadViewModel::clearRecent()
{
    recentFiles_.clear();
    recentFolders_.clear();
    settings_.markDirty();
}

bool NotepadViewModel::restoreSession()
{
    if (!files_ || !savedSession_.isObject())
        return false;
    const std::string workspace = savedSession_.getString("workspace");
    const model::Json* docs = savedSession_.find("documents");
    const bool hasDocs = docs && docs->isArray() && !docs->items().empty();
    if (workspace.empty() && !hasDocs)
        return false;
    if (!workspace.empty() && !openFolder(workspace))
        return false;
    if (hasDocs) {
        while (!documents_.empty())
            closeDocument(0);
        for (const model::Json& d : docs->items()) {
            const std::string path = d.getString("path");
            if (path.empty() || !files_->read(path))
                continue; // deleted since
            openFile(path);
            if (model::Document* doc = activeDocument())
                doc->caret = doc->anchor = doc->buffer.clamp({int(d.getNumber("line", 0)), 0});
        }
        const int active = int(savedSession_.getNumber("active", 0));
        if (active >= 0 && active < int(documents_.size()))
            activate(active);
    }
    statusText.set("Restored the last session" + (workspace.empty() ? std::string() : ": " + workspace));
    return !documents_.empty() || !workspace.empty();
}

void NotepadViewModel::recordDiskStamp(model::Document& doc)
{
    const std::optional<interfaces::FileStamp> stamp = files_ && doc.onDisk ? files_->stamp(absolutePath(doc.path)) : std::nullopt;
    doc.hasDiskStamp = stamp && stamp->exists;
    doc.diskSize = stamp ? stamp->size : 0;
    doc.diskModified = stamp ? stamp->modified : 0;
    doc.diskState = model::Document::DiskState::InSync;
}

std::optional<std::string> NotepadViewModel::readDiskText(const model::Document& doc) const
{
    std::optional<std::string> text = files_ ? files_->read(absolutePath(doc.path)) : std::nullopt;
    if (text && text->rfind("\xEF\xBB\xBF", 0) == 0)
        text->erase(0, 3);
    return text;
}

void NotepadViewModel::checkDiskChanges()
{
    if (!files_)
        return;
    for (auto& docPtr : documents_) {
        model::Document& doc = *docPtr;
        if (!doc.onDisk || !doc.hasDiskStamp)
            continue;
        const std::optional<interfaces::FileStamp> now = files_->stamp(absolutePath(doc.path));
        if (!now)
            continue;
        if (!now->exists) {
            if (doc.diskState != model::Document::DiskState::Deleted) {
                doc.diskState = model::Document::DiskState::Deleted;
                statusText.set(doc.title + " was deleted on disk (saving recreates it)");
            }
            continue;
        }
        if (now->size == doc.diskSize && now->modified == doc.diskModified) {
            if (doc.diskState == model::Document::DiskState::Deleted)
                doc.diskState = model::Document::DiskState::InSync; // it came back
            continue;
        }
        if (!doc.modified) {
            reloadFromDisk(doc); // nothing of yours to lose
            statusText.set("Reloaded " + doc.title + ": it changed on disk");
        } else if (doc.diskState != model::Document::DiskState::Changed) {
            doc.diskState = model::Document::DiskState::Changed;
            statusText.set(doc.title + " changed on disk, and you have unsaved changes: Reload, Keep mine or Compare");
        }
    }
}

void NotepadViewModel::reloadFromDisk(model::Document& doc)
{
    const std::optional<std::string> text = readDiskText(doc);
    if (!text)
        return;
    const int line = doc.caret.line;
    doc.buffer.setText(*text);
    doc.savedText = doc.buffer.text();
    doc.modified = false;
    doc.caret = doc.anchor = doc.buffer.clamp({line, 0});
    doc.cursors.clear();
    history_.forget(doc.id); // the old steps no longer apply to this text
    ++doc.revision;
    recordDiskStamp(doc);
    refreshFind();
}

void NotepadViewModel::keepMine(model::Document& doc)
{
    recordDiskStamp(doc); // accept the file as it is now: the next save replaces it
    doc.diskState = model::Document::DiskState::InSync;
    statusText.set("Keeping your version of " + doc.title + " (save to write it)");
}

void NotepadViewModel::compareWithDisk(model::Document& doc)
{
    const std::optional<std::string> disk = readDiskText(doc);
    std::vector<model::DiffRow> rows = model::diffLines(model::splitLines(disk.value_or(std::string())), model::splitLines(doc.buffer.text()));
    review_.setDiff(doc.title + ": on disk vs yours", std::move(rows));
    showPanel.set(true);
    panelTab.set(PanelTab::Diff);
}

void NotepadViewModel::recordMacro(MacroStep step) const
{
    if (!recording.get() || playingMacro_)
        return;
    // Typing coalesces into one text step.
    if (step.kind == MacroStep::Kind::Text && !macroRecording_.empty() && macroRecording_.back().kind == MacroStep::Kind::Text)
        macroRecording_.back().text += step.text;
    else
        macroRecording_.push_back(std::move(step));
}

void NotepadViewModel::playMacro(int times)
{
    model::Document* doc = activeDocument();
    if (!doc || macro_.empty() || recording.get())
        return;
    beginEdit(EditKind::Other); // every step joins this one undo step
    playingMacro_ = true;
    for (int t = 0; t < times; ++t)
        for (const MacroStep& step : macro_) {
            switch (step.kind) {
            case MacroStep::Kind::Text: insertText(step.text); break;
            case MacroStep::Kind::Newline: newline(); break;
            case MacroStep::Kind::Backspace: backspace(); break;
            case MacroStep::Kind::Delete: deleteForward(); break;
            case MacroStep::Kind::Indent: insertIndent(); break;
            case MacroStep::Kind::Move:
                setCaret({doc->caret.line + step.line, std::max(0, doc->caret.column + step.column)}, step.extend);
                break;
            case MacroStep::Kind::Command: execute(step.command); break;
            }
        }
    playingMacro_ = false;
    endEdit();
    statusText.set("Played the macro" + (times > 1 ? " " + std::to_string(times) + " times" : std::string()));
}

std::string NotepadViewModel::workspaceRelative(const std::string& path) const
{
    if (workspaceRoot_.empty() || !FileDialogViewModel::isAbsolute(path))
        return path;
    std::string root = workspaceRoot_, p = path;
    std::replace(root.begin(), root.end(), '\\', '/');
    std::replace(p.begin(), p.end(), '\\', '/');
    auto lower = [](std::string s) {
#if defined(_WIN32)
        for (char& c : s)
            c = char(std::tolower(static_cast<unsigned char>(c)));
#endif
        return s;
    };
    if (lower(p).rfind(lower(root) + "/", 0) == 0)
        return p.substr(root.size() + 1);
    return path;
}

bool NotepadViewModel::saveDocumentAs(model::Document& doc, const std::string& path)
{
    if (doc.kind != model::FileKind::Text) {
        statusText.set(doc.title + " is not open as text: there is nothing to save");
        return false;
    }
    if (!files_)
        return false;
    const std::string previousPath = doc.path;
    const bool previousOnDisk = doc.onDisk;
    doc.path = workspaceRelative(path);
    doc.onDisk = true;
    if (!saveDocument(doc)) {
        doc.path = previousPath;
        doc.onDisk = previousOnDisk;
        return false;
    }
    doc.title = fileName(doc.path);
    doc.language = model::languageFromPath(doc.path);
    remember(recentFiles_, absolutePath(doc.path), kMaxRecentFiles);
    ++revealSerial_;
    refreshExplorer();
    return true;
}

std::string NotepadViewModel::readWorkspaceFile(const std::string& path) const
{
    for (const auto& doc : documents_)
        if (doc->kind == model::FileKind::Text && (doc->path.empty() ? doc->title : doc->path) == path)
            return doc->buffer.text();
    if (!workspaceRoot_.empty() && files_)
        return files_->read(absolutePath(path)).value_or(std::string());
    return dummy::fileContent(path);
}

std::optional<std::string> NotepadViewModel::readFileRange(const std::string& path, std::uint64_t offset, size_t size, std::uint64_t* total) const
{
    if (!files_)
        return std::nullopt;
    return files_->readRange(absolutePath(path), offset, size, total);
}

bool NotepadViewModel::writeWorkspaceFile(const std::string& path, const std::string& text)
{
    if (workspaceRoot_.empty() || !files_)
        return false;
    const std::string full = absolutePath(path);
    const size_t slash = full.find_last_of('/');
    if (slash != std::string::npos)
        files_->createDirectories(full.substr(0, slash));
    return files_->write(full, text);
}

bool NotepadViewModel::openFolder(const std::string& path)
{
    if (!files_ || !files_->isDirectory(path)) {
        statusText.set("Cannot open folder " + path);
        return false;
    }
    std::string root = files_->absolute(path);
    while (root.size() > 1 && root.back() == '/')
        root.pop_back();
    workspaceRoot_ = root;
    workspaceIndent_.reset(); // a workspace file sets it again after opening its folder
    terminal_.setWorkingDirectory(root); // new terminals start in the open folder
    refreshExplorer();
    findInFiles_.folder.clear();
    findInFiles_.clear();
    layers_.workspaceChanged();
    logicalMap_.workspaceChanged();
    remember(recentFolders_, root, kMaxRecentFolders);
    while (!documents_.empty())
        closeDocument(0);
    // A real folder: no samples. The agent chat goes to the agent CLI, the review tools run the
    // configured commands.
    agent_.goLive(agentCli_, root);
    review_.goLive(processes_, agentCli_, root, [this]() {
        const model::Document* doc = activeDocument();
        return doc && doc->onDisk ? absolutePath(doc->path) : std::string();
    });
    git_.workspaceChanged();
    for (const ExplorerItem& item : explorer_.children)
        if (!item.isDirectory && (item.name == "README.md" || item.name == "readme.md")) {
            openFile(item.path);
            break;
        }
    statusText.set("Opened folder " + root);
    return true;
}

bool NotepadViewModel::isWorkspaceFile(const std::string& path)
{
    return model::isWorkspaceFile(path);
}

bool NotepadViewModel::openWorkspaceFile(const std::string& path)
{
    if (!files_) {
        statusText.set("Cannot open workspace files here (no file access)");
        return false;
    }
    const std::string file = files_->absolute(path);
    std::string error;
    const std::optional<std::string> text = files_->read(file, &error);
    if (!text) {
        statusText.set("Cannot read " + file + (error.empty() ? std::string() : ": " + error));
        return false;
    }
    const std::optional<model::WorkspaceImport> w = model::parseWorkspaceFile(file, *text, &error);
    if (!w) {
        statusText.set(error);
        return false;
    }
    if (!openFolder(w->root)) {
        statusText.set("The folder of " + w->name + " does not exist: " + w->root);
        return false;
    }
    // The other folders of a multi-root workspace: one Ctrl+R away (oldest first, so the list
    // keeps the workspace's order below the open one).
    for (size_t i = w->folders.size(); i-- > 1;)
        if (files_->isDirectory(w->folders[i]))
            remember(recentFolders_, w->folders[i], kMaxRecentFolders);
    remember(recentFolders_, workspaceRoot_, kMaxRecentFolders);
    if (w->tabSize || w->insertSpaces) {
        model::IndentStyle style = indentFor(model::Language::PlainText);
        if (w->tabSize)
            style.width = *w->tabSize;
        if (w->insertSpaces)
            style.tabs = !*w->insertSpaces;
        workspaceIndent_ = style;
    }
    std::string message;
    if (w->kind == model::WorkspaceImport::Kind::Solution) {
        message = "Imported the solution " + w->name + ": " + std::to_string(w->projects.size()) + " project(s) in " + workspaceRoot_;
    } else {
        message = "Imported the workspace " + w->name + ": " + workspaceRoot_;
        if (w->folders.size() > 1)
            message += " (" + std::to_string(w->folders.size() - 1) + " more folder(s) under Recent, Ctrl+R)";
    }
    if (workspaceIndent_)
        message += std::string(", indent ") + (workspaceIndent_->tabs ? "tabs" : "spaces") + " " + std::to_string(workspaceIndent_->width);
    statusText.set(message);
    return true;
}

void NotepadViewModel::openStartupDocuments(const std::vector<std::string>& arguments)
{
    if (workspaceRoot_.empty()) {
        openFile(dummy::kWelcomePath);
        openFile("src/main.cpp");
    }
    for (const std::string& argument : arguments) {
        if (isWorkspaceFile(argument) && openWorkspaceFile(argument))
            continue;
        openFile(argument);
    }
    if (workspaceRoot_.empty())
        statusText.set("Ready - running on sample data (open a folder: EasyNotepad <folder>)");
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

void NotepadViewModel::setCaret(model::TextPos pos, bool extendSelection, bool keepCursors)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    if (!keepCursors) {
        os::mem::release(doc->cursors); // thousands after Ctrl+Shift+L: give the room back
        occurrence_ = {}; // Ctrl+D starts over
    }
    MacroScope scope(*this);
    const model::TextPos before = doc->caret;
    doc->caret = doc->buffer.clamp(pos);
    if (scope.outer && doc->caret != before)
        recordMacro({MacroStep::Kind::Move, {}, doc->caret.line - before.line, doc->caret.column - before.column, extendSelection, CommandId::Count});
    if (!extendSelection)
        doc->anchor = doc->caret;
    if (keepCursors)
        model::normalizeCursors(*doc);
}

void NotepadViewModel::setSelection(model::TextPos anchor, model::TextPos caret)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    os::mem::release(doc->cursors);
    occurrence_ = {};
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
    if (!doc)
        return {};
    if (doc->multiCursor()) {
        std::vector<model::Selection> all = doc->selections();
        std::sort(all.begin(), all.end(), [](const model::Selection& a, const model::Selection& b) { return a.start() < b.start(); });
        std::string text;
        bool first = true;
        for (const model::Selection& sel : all) {
            if (sel.empty())
                continue;
            if (!first)
                text += '\n';
            text += doc->buffer.text(sel.start(), sel.end());
            first = false;
        }
        return text;
    }
    if (!doc->hasSelection())
        return {};
    return doc->buffer.text(doc->selectionStart(), doc->selectionEnd());
}

bool NotepadViewModel::anySelection() const
{
    const model::Document* doc = activeDocument();
    if (!doc)
        return false;
    if (doc->hasSelection())
        return true;
    for (const model::Selection& sel : doc->cursors)
        if (!sel.empty())
            return true;
    return false;
}

void NotepadViewModel::insertText(std::string_view text)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    MacroScope scope(*this);
    if (scope.outer)
        recordMacro({MacroStep::Kind::Text, std::string(text), 0, 0, false, CommandId::Count});
    const EditKind kind = text.size() == 1 && text[0] != '\n' ? EditKind::Typing : EditKind::Other;
    if (forEachCursor(kind, [&](size_t) { insertText(text); }))
        return;
    beginEdit(kind);
    model::TextPos at = doc->buffer.erase(doc->selectionStart(), doc->selectionEnd());
    doc->caret = doc->anchor = doc->buffer.insert(at, text);
    // Typing a closer ("}", "</", "endif(", "fi ", "else:") moves its line to the opener's indent.
    if (smartIndent.get() && text.size() == 1 && model::closesIndent(text[0], model::languageSpec(doc->language)))
        reindentClosingLine(*doc, doc->caret.line);
    // Closing a continued block comment: " * */" becomes " */".
    if (smartIndent.get() && text == "/" && model::languageSpec(doc->language).blockCommentStart == "/*") {
        const std::string& line = doc->buffer.line(doc->caret.line);
        const size_t ws = model::leadingWhitespace(line).size();
        if (line.compare(ws, std::string::npos, "* */") == 0 && doc->caret.column == int(line.size())) {
            doc->buffer.erase({doc->caret.line, int(ws)}, {doc->caret.line, int(ws) + 2});
            doc->caret.column -= 2;
            doc->anchor = doc->caret;
        }
    }
    endEdit();
}

bool NotepadViewModel::reindentClosingLine(model::Document& doc, int line)
{
    const std::optional<std::string> target = model::closingIndent(doc.buffer, line, model::languageSpec(doc.language), doc.indent);
    const std::string current = model::leadingWhitespace(doc.buffer.line(line));
    if (!target || *target == current)
        return false;
    doc.buffer.erase({line, 0}, {line, int(current.size())});
    doc.buffer.insert({line, 0}, *target);
    const int delta = int(target->size()) - int(current.size());
    for (model::TextPos* p : {&doc.caret, &doc.anchor})
        if (p->line == line)
            p->column = std::max(int(target->size()), p->column + delta);
    return true;
}

void NotepadViewModel::newline()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    MacroScope scope(*this);
    if (scope.outer)
        recordMacro({MacroStep::Kind::Newline, {}, 0, 0, false, CommandId::Count});
    if (forEachCursor(EditKind::Other, [&](size_t) { newline(); }))
        return;
    if (!smartIndent.get()) {
        const std::string indent = leadingWhitespace(doc->buffer.line(doc->selectionStart().line));
        insertText("\n" + indent);
        return;
    }
    beginEdit(EditKind::Other);
    doc->caret = doc->anchor = doc->buffer.erase(doc->selectionStart(), doc->selectionEnd());
    const model::LanguageSpec& spec = model::languageSpec(doc->language);
    reindentClosingLine(*doc, doc->caret.line); // "end" / "fi" typed without a space after it
    const model::NewlineEdit e = model::smartNewline(doc->buffer, doc->caret, spec, doc->indent);
    model::TextPos from{doc->caret.line, doc->caret.column - e.eraseBefore};
    model::TextPos to{doc->caret.line, doc->caret.column + e.eraseAfter};
    model::TextPos at = doc->buffer.erase(from, to);
    // Caret after the first part (the new line's indent); the rest (a split-off closer) follows it.
    doc->caret = doc->buffer.insert(at, e.text.substr(0, size_t(e.caretOffset)));
    if (size_t(e.caretOffset) < e.text.size())
        doc->buffer.insert(doc->caret, e.text.substr(size_t(e.caretOffset)));
    doc->anchor = doc->caret;
    endEdit();
}

void NotepadViewModel::insertIndent()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    MacroScope scope(*this);
    if (scope.outer)
        recordMacro({MacroStep::Kind::Indent, {}, 0, 0, false, CommandId::Count});
    if (forEachCursor(EditKind::Other, [&](size_t) { insertIndent(); }))
        return;
    if (doc->indent.tabs) {
        insertText("\t");
        return;
    }
    // Spaces up to the next stop, measured in visual columns (tabs expand, UTF-8 counts once).
    const model::TextPos start = doc->selectionStart();
    const std::string& line = doc->buffer.line(start.line);
    const int width = std::max(1, doc->indent.width);
    int column = 0;
    for (int i = 0; i < start.column && i < int(line.size()); ++i) {
        if (line[size_t(i)] == '\t')
            column = (column / width + 1) * width;
        else if (!model::isUtf8Continuation(line[size_t(i)]))
            ++column;
    }
    insertText(std::string(size_t(width - column % width), ' '));
}

void NotepadViewModel::setIndentStyle(model::IndentStyle style)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    style.width = std::clamp(style.width, 1, 16);
    doc->indent = style;
    doc->indentDetected = false; // chosen by the user from now on
    statusText.set(std::string("Indent: ") + (style.tabs ? "tabs" : "spaces") + ", width " + std::to_string(style.width) +
                   " (Convert re-writes existing lines)");
}

void NotepadViewModel::applyDetectedIndent(model::Document& doc)
{
    const std::optional<model::IndentStyle> found = model::detectIndent(doc.buffer);
    doc.indent = found ? *found : indentFor(doc.language);
    doc.indentDetected = found.has_value();
}

void NotepadViewModel::backspace()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    MacroScope scope(*this);
    if (scope.outer)
        recordMacro({MacroStep::Kind::Backspace, {}, 0, 0, false, CommandId::Count});
    if (forEachCursor(EditKind::Deleting, [&](size_t) { backspace(); }))
        return;
    if (doc->hasSelection()) {
        deleteSelection();
        return;
    }
    if (doc->caret == model::TextPos{0, 0})
        return;
    beginEdit(EditKind::Deleting);
    // In leading spaces, one indent level at a time (back to the previous stop).
    const std::string& line = doc->buffer.line(doc->caret.line);
    const int column = doc->caret.column;
    model::TextPos from = doc->buffer.prev(doc->caret);
    if (smartIndent.get() && !doc->indent.tabs && column > 0 && int(model::leadingWhitespace(line).size()) >= column &&
        line.find('\t') >= size_t(column)) {
        const int width = std::max(1, doc->indent.width);
        const int remove = column % width == 0 ? width : column % width;
        from = {doc->caret.line, column - std::min(remove, column)};
    }
    doc->caret = doc->anchor = doc->buffer.erase(from, doc->caret);
    endEdit();
}

void NotepadViewModel::deleteForward()
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    MacroScope scope(*this);
    if (scope.outer)
        recordMacro({MacroStep::Kind::Delete, {}, 0, 0, false, CommandId::Count});
    if (forEachCursor(EditKind::Deleting, [&](size_t) { deleteForward(); }))
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
    if (!doc || !anySelection())
        return;
    if (forEachCursor(EditKind::Other, [&](size_t) { deleteSelection(); }))
        return;
    beginEdit(EditKind::Other);
    doc->caret = doc->anchor = doc->buffer.erase(doc->selectionStart(), doc->selectionEnd());
    endEdit();
}

void NotepadViewModel::goToLine(int line)
{
    if (!activeDocument())
        return;
    revealLine(line);
    setCaret({line, 0}, false);
    caretRevealRequested.emit();
}

// The code the local model gets with a question (it cannot open files itself): the selection, or
// the lines around the caret, at most about 2,500 characters (some 600 tokens: the model reads about
// 100 tokens a second, so every one is felt before the first word).
std::string NotepadViewModel::agentContextText() const
{
    const model::Document* doc = activeDocument();
    if (!doc || doc->buffer.lineCount() == 0)
        return {};
    std::string code;
    if (doc->hasSelection()) {
        code = doc->buffer.text(doc->selectionStart(), doc->selectionEnd());
    } else {
        const int first = std::max(0, doc->caret.line - 20);
        const int last = std::min(doc->buffer.lineCount() - 1, doc->caret.line + 20);
        code = doc->buffer.text({first, 0}, {last, int(doc->buffer.line(last).size())});
    }
    constexpr size_t kMaxChars = 2500;
    if (code.size() > kMaxChars)
        code = code.substr(0, kMaxChars) + "\n...";
    if (code.find_first_not_of(" \t\r\n") == std::string::npos)
        return {};
    return "```\n" + code + "\n```";
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
    revealLine(doc->caret.line); // never edit text hidden behind a summary
    history_.begin(*doc, kind);
}

void NotepadViewModel::endEdit()
{
    model::Document* doc = activeDocument();
    if (!history_.end(*doc))
        return; // a nested edit: the outer one finishes it
    model::normalizeCursors(*doc);
    ++doc->revision;
    doc->modified = !doc->buffer.equals(doc->savedText);
    refreshFind();
    caretRevealRequested.emit();
}

bool NotepadViewModel::canUndo() const
{
    const model::Document* doc = activeDocument();
    return doc && history_.canUndo(doc->id);
}

size_t NotepadViewModel::historyBytes() const
{
    const model::Document* doc = activeDocument();
    return doc ? history_.bytes(doc->id) : 0;
}

bool NotepadViewModel::canRedo() const
{
    const model::Document* doc = activeDocument();
    return doc && history_.canRedo(doc->id);
}

void NotepadViewModel::afterHistoryMove(model::Document& doc)
{
    model::normalizeCursors(doc); // clamps every caret
    doc.modified = !doc.buffer.equals(doc.savedText);
    ++doc.revision;
    refreshFind();
    caretRevealRequested.emit();
}

void NotepadViewModel::undo()
{
    model::Document* doc = activeDocument();
    history_.undo(*doc);
    afterHistoryMove(*doc);
}

void NotepadViewModel::redo()
{
    model::Document* doc = activeDocument();
    history_.redo(*doc);
    afterHistoryMove(*doc);
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
    doc->cursors.clear();
    endEdit();
    statusText.set("Replaced " + std::to_string(count) + " occurrence(s)");
}

// ---- Ribbon customization -----------------------------------------------------------------------

bool NotepadViewModel::isPinned(CommandId id) const
{
    return std::find(pinned_.begin(), pinned_.end(), id) != pinned_.end();
}

void NotepadViewModel::setPinned(CommandId id, bool pinned)
{
    const auto it = std::find(pinned_.begin(), pinned_.end(), id);
    if (pinned && it == pinned_.end())
        pinned_.push_back(id);
    else if (!pinned && it != pinned_.end())
        pinned_.erase(it);
    ++ribbonRevision_;
    settings_.markDirty();
}

void NotepadViewModel::movePinned(CommandId id, int direction)
{
    const auto it = std::find(pinned_.begin(), pinned_.end(), id);
    if (it == pinned_.end())
        return;
    const long from = long(it - pinned_.begin());
    const long to = from + (direction < 0 ? -1 : 1);
    if (to < 0 || to >= long(pinned_.size()))
        return;
    std::swap(pinned_[size_t(from)], pinned_[size_t(to)]);
    ++ribbonRevision_;
    settings_.markDirty();
}

void NotepadViewModel::setRibbonTabs(WorkspaceMode mode, std::vector<RibbonTabSetting> tabs)
{
    std::vector<RibbonTabSetting>& current = ribbonTabs_[mode == WorkspaceMode::Agent ? 1 : 0];
    if (tabs == current)
        return;
    current = std::move(tabs);
    ++ribbonRevision_;
    settings_.markDirty();
}

void NotepadViewModel::restoreRibbonTabs(WorkspaceMode mode)
{
    std::vector<RibbonTabSetting>& current = ribbonTabs_[mode == WorkspaceMode::Agent ? 1 : 0];
    if (current.empty())
        return;
    os::mem::release(current);
    ++ribbonRevision_;
    settings_.markDirty();
    statusText.set("Ribbon tabs restored to the default");
}

void NotepadViewModel::addCustomAction(std::string name, std::string command)
{
    if (name.empty() || command.empty())
        return;
    customActions_.push_back({std::move(name), std::move(command)});
    statusText.set("Added \"" + customActions_.back().name + "\" to My Tools");
    settings_.markDirty();
    ++ribbonRevision_;
}

void NotepadViewModel::removeCustomAction(int index)
{
    if (index >= 0 && index < int(customActions_.size()))
        customActions_.erase(customActions_.begin() + index);
    settings_.markDirty();
    ++ribbonRevision_;
}

void NotepadViewModel::runCustomAction(int index)
{
    if (index < 0 || index >= int(customActions_.size()))
        return;
    if (!terminal_.supported()) {
        statusText.set("Custom actions need a terminal, which this platform does not have");
        return;
    }
    runInTerminal(customActions_[size_t(index)].command);
    statusText.set("Running " + customActions_[size_t(index)].name + ": " + customActions_[size_t(index)].command);
}

void NotepadViewModel::runInTerminal(const std::string& command)
{
    if (!terminal_.supported()) {
        statusText.set("This platform has no terminal: run it yourself: " + command);
        return;
    }
    showPanelTab(PanelTab::Terminal);
    if (!terminal_.active() || terminal_.active()->exited)
        terminal_.newTerminal(0);
    if (terminal_.active())
        terminal_.sendInput(command + "\r");
}

void NotepadViewModel::replaceLines(int first, int last, const std::vector<std::string>& lines)
{
    model::Document* doc = activeDocument();
    if (!doc || first < 0 || first > doc->buffer.lineCount())
        return;
    const int count = doc->buffer.lineCount();
    std::string text;
    for (const std::string& l : lines)
        text += l + "\n";
    model::TextPos from{first, 0}, to{first, 0};
    if (last >= first) {
        to = last + 1 < count ? model::TextPos{last + 1, 0} : model::TextPos{count - 1, int(doc->buffer.line(count - 1).size())};
        if (last + 1 >= count && !text.empty())
            text.pop_back(); // the last line has no newline after it
    }
    if (first >= count) { // appending after the last line
        from = to = {count - 1, int(doc->buffer.line(count - 1).size())};
        text = "\n" + text.substr(0, text.empty() ? 0 : text.size() - 1);
    }
    setSelection(from, to);
    insertText(text);
}

namespace {
bool validName(const std::string& name)
{
    return !name.empty() && name != "." && name != ".." && name.find_first_of("/\\:*?\"<>|") == std::string::npos;
}
} // namespace

bool NotepadViewModel::createFile(const std::string& folder, const std::string& name)
{
    if (!canEditFiles() || !validName(name)) {
        statusText.set("Not a valid file name: " + name);
        return false;
    }
    const std::string path = folder.empty() ? name : folder + "/" + name;
    const std::optional<interfaces::FileStamp> stamp = files_->stamp(absolutePath(path));
    if (stamp && stamp->exists) {
        statusText.set(path + " already exists");
        return false;
    }
    std::string error;
    if (!files_->write(absolutePath(path), "", &error)) {
        statusText.set("Cannot create " + path + ": " + error);
        return false;
    }
    refreshExplorer();
    openFile(path);
    statusText.set("Created " + path);
    git_.requestRefresh();
    return true;
}

bool NotepadViewModel::createFolder(const std::string& folder, const std::string& name)
{
    if (!canEditFiles() || !validName(name)) {
        statusText.set("Not a valid folder name: " + name);
        return false;
    }
    const std::string path = folder.empty() ? name : folder + "/" + name;
    if (files_->isDirectory(absolutePath(path)) || !files_->createDirectories(absolutePath(path))) {
        statusText.set("Cannot create the folder " + path);
        return false;
    }
    refreshExplorer();
    statusText.set("Created the folder " + path);
    return true;
}

bool NotepadViewModel::renamePath(const std::string& path, const std::string& newName)
{
    if (!canEditFiles() || !validName(newName)) {
        statusText.set("Not a valid name: " + newName);
        return false;
    }
    const size_t slash = path.find_last_of('/');
    const std::string target = (slash == std::string::npos ? std::string() : path.substr(0, slash + 1)) + newName;
    if (target == path)
        return true;
    std::string error;
    if (!files_->rename(absolutePath(path), absolutePath(target), &error)) {
        statusText.set("Cannot rename " + path + ": " + error);
        return false;
    }
    // Open documents (the file, or files inside the folder) follow.
    for (auto& doc : documents_) {
        if (doc->path == path) {
            doc->path = target;
            doc->title = newName;
            doc->language = model::languageFromPath(target);
        } else if (doc->path.rfind(path + "/", 0) == 0)
            doc->path = target + doc->path.substr(path.size());
        else
            continue;
        recordDiskStamp(*doc);
    }
    refreshExplorer();
    statusText.set("Renamed " + path + " to " + newName);
    git_.requestRefresh();
    return true;
}

bool NotepadViewModel::deletePath(const std::string& path)
{
    if (!canEditFiles() || path.empty())
        return false;
    // Its open documents close first (the view asked about unsaved ones).
    for (int i = int(documents_.size()) - 1; i >= 0; --i) {
        const std::string& p = documents_[size_t(i)]->path;
        if (p == path || p.rfind(path + "/", 0) == 0)
            closeDocument(i);
    }
    bool recycled = false;
    std::string error;
    if (!files_->remove(absolutePath(path), &recycled, &error)) {
        statusText.set("Cannot delete " + path + ": " + error);
        return false;
    }
    refreshExplorer();
    statusText.set(recycled ? "Moved " + path + " to the Recycle Bin" : "Deleted " + path);
    git_.requestRefresh();
    return true;
}

NotepadViewModel::AgentSetup NotepadViewModel::agentSetup() const
{
    AgentSetup setup;
    if (!agentCli_)
        return setup;
    setup.available = agentCli_->available();
    setup.checking = agentCli_->checking();
    setup.claude = agentCli_->isClaude();
    setup.command = agentCli_->command();
    setup.executable = agentCli_->executable();
    return setup;
}

void NotepadViewModel::recheckAgent()
{
    if (agentCli_)
        agentCli_->recheck();
}

std::string NotepadViewModel::agentInstallCommand()
{
#if defined(_WIN32)
    return "irm https://claude.ai/install.ps1 | iex";
#else
    return "curl -fsSL https://claude.ai/install.sh | bash";
#endif
}

void NotepadViewModel::installAgent()
{
#if defined(_WIN32)
    // Through PowerShell explicitly: the terminal may be running Command Prompt.
    runInTerminal("powershell -NoProfile -ExecutionPolicy Bypass -Command "" + agentInstallCommand() + """);
#else
    runInTerminal(agentInstallCommand());
#endif
    statusText.set("Installing Claude Code in the terminal; press Check again when it has finished");
}

void NotepadViewModel::loginAgent()
{
    const std::string found = agentCli_ ? agentCli_->executable() : std::string();
    std::string command = "claude";
    if (!found.empty() && found.find_first_of(" \t") == std::string::npos)
        command = found;
    else if (!found.empty())
#if defined(_WIN32)
        command = "cmd /c "" + found + """; // works from PowerShell and Command Prompt alike
#else
        command = """ + found + """;
#endif
    runInTerminal(command);
    statusText.set("Log in to Claude Code in the terminal (it opens your browser), then type /exit");
}

void NotepadViewModel::openLink(const std::string& url)
{
    if (!files_ || !files_->openExternal(url))
        statusText.set("Cannot open " + url);
}

void NotepadViewModel::revealInFileManager(const std::string& path)
{
    if (files_ && !workspaceRoot_.empty() && !files_->reveal(absolutePath(path)))
        statusText.set("Cannot show " + path + " in the file manager");
}

void NotepadViewModel::openTerminalIn(const std::string& folder)
{
    const std::string dir = absolutePath(folder.empty() ? "." : folder);
#if defined(_WIN32)
    runInTerminal("cd /d \"" + dir + "\"");
#else
    runInTerminal("cd '" + dir + "'");
#endif
}

void NotepadViewModel::findInFolder(const std::string& folder)
{
    findInFiles_.folder = folder;
    showPanelTab(PanelTab::Search);
    searchFocusRequested.emit();
}

void NotepadViewModel::openTextDocument(const std::string& title, const std::string& pathForLanguage, const std::string& text)
{
    model::Document& doc = addDocument(title, {}, text);
    doc.language = model::languageFromPath(pathForLanguage);
}

// ---- Reading ------------------------------------------------------------------------------------

const char* NotepadViewModel::detailLevelName(int level)
{
    static const char* const names[kDetailLevels] = {"Full", "Balanced", "Overview", "Outline"};
    return names[std::clamp(level, 0, kDetailLevels - 1)];
}

const char* NotepadViewModel::detailLevelDescription(int level)
{
    static const char* const descriptions[kDetailLevels] = {
        "every line of code",
        "long functions and blocks collapse into AI summaries",
        "every function body collapses into its AI summary",
        "only the top-level structure, each part summarized",
    };
    return descriptions[std::clamp(level, 0, kDetailLevels - 1)];
}

const std::vector<model::OutlineNode>& NotepadViewModel::outline() const
{
    static const std::vector<model::OutlineNode> none;
    const model::Document* doc = activeDocument();
    if (!doc)
        return none;
    if (outline_.documentId != doc->id || outline_.revision != doc->revision || outline_.language != doc->language ||
        outline_.mode != foldingMode.get()) {
        if (outline_.mode != foldingMode.get())
            outline_.summaries.clear();
        outline_.mode = foldingMode.get();
        const bool sameDocument = outline_.documentId == doc->id;
        outline_.documentId = doc->id;
        outline_.revision = doc->revision;
        outline_.language = doc->language;
        outline_.nodes = model::buildOutline(doc->buffer, doc->language, foldingMode.get());
        if (!sameDocument)
            outline_.summaries.clear();
    }
    return outline_.nodes;
}

const model::OutlineNode* NotepadViewModel::regionAt(int line) const
{
    const model::OutlineNode* found = nullptr;
    for (const model::OutlineNode& n : outline())
        if (n.startLine == line)
            found = &n; // children come after parents: keep the innermost
    return found;
}

const std::string& NotepadViewModel::summary(const model::OutlineNode& node) const
{
    auto it = outline_.summaries.find(node.startLine);
    if (it == outline_.summaries.end()) {
        const model::Document* doc = activeDocument();
        std::vector<std::string> parts;
        const auto& nodes = outline();
        const int self = int(&node - nodes.data());
        for (const model::OutlineNode& n : nodes)
            if (n.parent == self)
                parts.push_back(n.name);
        it = outline_.summaries.emplace(node.startLine, dummy::summarize(doc->path.empty() ? doc->title : doc->path, node,
                                                                         doc->buffer, parts)).first;
    }
    return it->second;
}

bool NotepadViewModel::isFolded(const model::OutlineNode& node) const
{
    const model::Document* doc = activeDocument();
    return doc && doc->folded.count(node.startLine) > 0;
}

void NotepadViewModel::setFolded(int startLine, bool folded)
{
    model::Document* doc = activeDocument();
    if (!doc)
        return;
    if (folded)
        doc->folded.insert(startLine);
    else
        doc->folded.erase(startLine);
    // Keep the caret visible: a caret inside a newly folded region moves to its header.
    if (const model::OutlineNode* n = regionAt(startLine); folded && n && n->contains(doc->caret.line))
        setCaret({n->startLine, 0}, false);
}

void NotepadViewModel::toggleFold(int startLine)
{
    if (const model::Document* doc = activeDocument())
        setFolded(startLine, doc->folded.count(startLine) == 0);
}

void NotepadViewModel::foldAll()
{
    for (const model::OutlineNode& n : outline())
        setFolded(n.startLine, true);
}

void NotepadViewModel::unfoldAll()
{
    if (model::Document* doc = activeDocument())
        doc->folded.clear();
}

void NotepadViewModel::revealLine(int line)
{
    model::Document* doc = activeDocument();
    if (!doc || doc->folded.empty())
        return;
    for (const model::OutlineNode& n : outline())
        if (n.contains(line))
            doc->folded.erase(n.startLine);
}

void NotepadViewModel::applyDetailLevel(model::Document& doc)
{
    doc.detailLevel = detailLevel.get();
    doc.folded.clear();
    if (&doc != activeDocument())
        return;
    const int level = detailLevel.get();
    const bool logical = foldingMode.get() == model::OutlineMode::Logical;
    for (const model::OutlineNode& n : outline()) {
        bool fold = false;
        if (logical && n.kind != model::OutlineKind::Section) {
            // Logical: Balanced folds long steps, Overview every step (functions read as their step
            // list), Outline whole functions and types.
            switch (level) {
            case 1: fold = n.kind == model::OutlineKind::Step && n.lineCount() > 5; break;
            case 2: fold = n.kind == model::OutlineKind::Step; break;
            case 3: fold = n.kind == model::OutlineKind::Function || n.kind == model::OutlineKind::Type; break;
            default: break;
            }
            if (fold)
                doc.folded.insert(n.startLine);
            continue;
        }
        switch (level) {
        case 1: // Balanced: long functions, and long nested blocks or sub-sections
            fold = n.lineCount() > 12 && (n.kind == model::OutlineKind::Function || n.depth > 0);
            break;
        case 2: // Overview: every function body, and sub-sections
            fold = n.kind == model::OutlineKind::Function || (n.kind == model::OutlineKind::Section && n.depth > 0) ||
                   (n.kind == model::OutlineKind::Block && n.depth > 0 && n.lineCount() > 6);
            break;
        case 3: // Outline: top-level structure only
            fold = n.depth == 0 ? n.kind != model::OutlineKind::Section || n.lineCount() > 3 : false;
            if (n.depth == 1 && n.kind == model::OutlineKind::Section)
                fold = true;
            break;
        default: break;
        }
        if (fold)
            doc.folded.insert(n.startLine);
    }
    // Markdown in Outline mode keeps the document title open and folds its sections.
    if (level == 3 && doc.language == model::Language::Markdown)
        for (const model::OutlineNode& n : outline())
            if (n.depth == 0)
                doc.folded.erase(n.startLine);
    if (doc.caret.line > 0)
        for (const model::OutlineNode& n : outline())
            if (doc.folded.count(n.startLine) && n.contains(doc.caret.line))
                doc.caret = doc.anchor = {n.startLine, 0};
}

// ---- Agent cursor ---------------------------------------------------------------------------------

void NotepadViewModel::pointAt(std::string target, std::string message, bool click, double seconds)
{
    pointer_.pointAt(std::move(target), std::move(message), click, seconds);
}

void NotepadViewModel::hidePointer()
{
    pointer_.hidePointer();
}

void NotepadViewModel::startTour()
{
    pointer_.startTour();
}

void NotepadViewModel::stopTour()
{
    pointer_.stopTour();
}

void NotepadViewModel::tick(double now)
{
    settings_.flush(now);
    engineLab_.refresh(); // a model download, started from the lab or the agent set-up
    if (now >= nextDiskCheck_) {
        nextDiskCheck_ = now + 2.0;
        checkDiskChanges();
    }
    pointer_.tick(now);
    agent_.tick(now);
    git_.tick(now);
}

// ---- Settings -----------------------------------------------------------------------------------

void NotepadViewModel::setDocumentLanguage(model::Language language)
{
    if (model::Document* doc = activeDocument()) {
        doc->language = language;
        if (!doc->indentDetected)
            doc->indent = indentFor(language);
        statusText.set(std::string("Language: ") + model::languageSpec(language).name);
    }
}

void NotepadViewModel::setSettingsPath(const std::string& path)
{
    settings_.setPath(path);
}

// What is remembered between sessions: view options as bound properties, structured data as
// sections of the settings file.
void NotepadViewModel::bindSettings()
{
    settings_.bind("view.darkTheme", darkTheme);
    settings_.bind("view.workspace", workspace);
    settings_.bind("view.showExplorer", showExplorer);
    settings_.bind("view.slimRibbon", slimRibbon);
    settings_.bind("developer.enabled", developerMode);
    settings_.section(
        "engineLab", [this](const model::Json& j) { engineLab_.load(j); },
        [this]() { return engineLab_.save(); });
    settings_.bind("view.showPanel", showPanel);
    settings_.bind("view.panelTab", panelTab);
    settings_.bind("view.showAgentPanel", showAgentPanel);
    settings_.bind("explorer.showHiddenFiles", showHiddenFiles);
    settings_.bind("view.showSideBar", showLayersMap);
    settings_.bind("view.sideBar", sideBar);
    settings_.bind("view.detailLevel", detailLevel);
    settings_.bind("view.foldingMode", foldingMode);
    settings_.bind("view.zoom", zoom);
    settings_.bind("editor.wordWrap", wordWrap);
    settings_.bind("agent.model", agent_.modelIndex);
    settings_.bind("agent.mode", agent_.autonomyIndex);
    settings_.bind("agent.command", agent_.command);
    settings_.bind("agent.appControl", agent_.appControl);
    settings_.bind("agent.choice", agent_.agentChoice);
    settings_.bind("agent.localFallback", agent_.localFallback);
    settings_.bind("editor.showWhitespace", showWhitespace);
    settings_.bind("editor.showLineNumbers", showLineNumbers);
    settings_.bind("editor.showIndentGuides", showIndentGuides);
    settings_.bind("editor.smartIndent", smartIndent);
    settings_.bind("performance.maxFps", maxFramesPerSecond);
    settings_.bind("performance.blinkCaret", blinkCaret);

    // {"indent": {"tabs": false, "width": 2}}
    settings_.section(
        "indent",
        [this](const model::Json& j) {
            if (j.isObject())
                preferredIndent_ = model::IndentStyle{j.getBool("tabs"), std::clamp(int(j.getNumber("width", 4)), 1, 16)};
        },
        [this]() {
            if (!preferredIndent_)
                return model::Json();
            return model::Json::object().set("tabs", preferredIndent_->tabs).set("width", preferredIndent_->width);
        });

    // {"macro": [{"kind": 0, "text": "...", "line": 0, "column": 0, "extend": false, "command": "DuplicateLine"}]}
    settings_.section(
        "macro",
        [this](const model::Json& j) {
            macro_.clear();
            for (const model::Json& step : j.items()) {
                MacroStep m;
                m.kind = MacroStep::Kind(std::clamp(int(step.getNumber("kind")), 0, int(MacroStep::Kind::Command)));
                m.text = step.getString("text");
                m.line = int(step.getNumber("line"));
                m.column = int(step.getNumber("column"));
                m.extend = step.getBool("extend");
                if (m.kind == MacroStep::Kind::Command && !commandFromKey(step.getString("command"), m.command))
                    continue;
                macro_.push_back(std::move(m));
            }
        },
        [this]() {
            model::Json steps = model::Json::array();
            for (const MacroStep& m : macro_) {
                model::Json step = model::Json::object().set("kind", int(m.kind));
                if (!m.text.empty())
                    step.set("text", m.text);
                if (m.kind == MacroStep::Kind::Move)
                    step.set("line", m.line).set("column", m.column).set("extend", m.extend);
                if (m.kind == MacroStep::Kind::Command)
                    step.set("command", commandKey(m.command));
                steps.push(std::move(step));
            }
            return steps;
        });

    // {"recent": {"files": [...], "folders": [...]}}
    settings_.section(
        "recent",
        [this](const model::Json& j) {
            auto read = [&](const char* key, std::vector<std::string>& into) {
                into.clear();
                if (const model::Json* list = j.find(key); list && list->isArray())
                    for (const model::Json& p : list->items())
                        if (!p.asString().empty())
                            into.push_back(p.asString());
            };
            read("files", recentFiles_);
            read("folders", recentFolders_);
        },
        [this]() {
            model::Json files = model::Json::array(), folders = model::Json::array();
            for (const std::string& p : recentFiles_)
                files.push(p);
            for (const std::string& p : recentFolders_)
                folders.push(p);
            return model::Json::object().set("files", std::move(files)).set("folders", std::move(folders));
        });

    // {"session": {"workspace": "C:/x", "documents": [{"path": "C:/x/a.cpp", "line": 12}], "active": 0}}
    settings_.section(
        "session",
        [this](const model::Json& j) { savedSession_ = j; },
        [this]() {
            model::Json docs = model::Json::array();
            int active = -1;
            for (int i = 0; i < int(documents_.size()); ++i) {
                const model::Document& doc = *documents_[size_t(i)];
                if (!doc.onDisk)
                    continue; // untitled and sample documents are not part of the session
                if (i == activeIndex.get())
                    active = int(docs.items().size());
                docs.push(model::Json::object().set("path", absolutePath(doc.path)).set("line", doc.caret.line));
            }
            return model::Json::object()
                .set("workspace", workspaceRoot_)
                .set("documents", std::move(docs))
                .set("active", active);
        });

    // {"ribbon": {"pinned": ["DetailOverview", ...], "customActions": [{"name": ..., "command": ...}],
    //             "tabs": {"code": [{"name": "Home", "visible": true}, ...], "agent": [...]}}}
    settings_.section(
        "ribbon",
        [this](const model::Json& j) {
            if (const model::Json* pinned = j.find("pinned"); pinned && pinned->isArray()) {
                pinned_.clear();
                for (const model::Json& key : pinned->items())
                    if (CommandId id; commandFromKey(key.asString(), id))
                        pinned_.push_back(id);
            }
            if (const model::Json* actions = j.find("customActions"); actions && actions->isArray()) {
                customActions_.clear();
                for (const model::Json& a : actions->items())
                    if (!a.getString("name").empty())
                        customActions_.push_back({a.getString("name"), a.getString("command")});
            }
            if (const model::Json* tabs = j.find("tabs"); tabs && tabs->isObject()) {
                const char* const keys[] = {"code", "agent"};
                for (int m = 0; m < 2; ++m) {
                    ribbonTabs_[m].clear();
                    if (const model::Json* list = tabs->find(keys[m]); list && list->isArray())
                        for (const model::Json& t : list->items())
                            if (!t.getString("name").empty())
                                ribbonTabs_[m].push_back({t.getString("name"), t.getBool("visible", true)});
                }
            }
            ++ribbonRevision_;
        },
        [this]() {
            model::Json pinned = model::Json::array();
            for (CommandId id : pinned_)
                pinned.push(commandKey(id));
            model::Json actions = model::Json::array();
            for (const CustomAction& a : customActions_)
                actions.push(model::Json::object().set("name", a.name).set("command", a.command));
            model::Json result = model::Json::object().set("pinned", std::move(pinned)).set("customActions", std::move(actions));
            if (!ribbonTabs_[0].empty() || !ribbonTabs_[1].empty()) { // only when the user changed something
                model::Json tabs = model::Json::object();
                const char* const keys[] = {"code", "agent"};
                for (int m = 0; m < 2; ++m) {
                    model::Json list = model::Json::array();
                    for (const RibbonTabSetting& t : ribbonTabs_[m])
                        list.push(model::Json::object().set("name", t.name).set("visible", t.visible));
                    tabs.set(keys[m], std::move(list));
                }
                result.set("tabs", std::move(tabs));
            }
            return result;
        });
}

void NotepadViewModel::setPreferredIndent(std::optional<model::IndentStyle> style)
{
    if (style)
        style->width = std::clamp(style->width, 1, 16);
    preferredIndent_ = style;
    // Open documents without a style of their own follow the new default.
    for (auto& doc : documents_)
        if (!doc->indentDetected)
            doc->indent = indentFor(doc->language);
    settings_.markDirty();
}

model::IndentStyle NotepadViewModel::indentFor(model::Language language) const
{
    if (workspaceIndent_)
        return *workspaceIndent_;
    return preferredIndent_ ? *preferredIndent_ : model::defaultIndent(model::languageSpec(language));
}

std::optional<uint32_t> NotepadViewModel::syntaxColor(bool dark, const std::string& languageKey, model::Token token) const
{
    return syntaxColors_.color(dark, languageKey, token);
}

void NotepadViewModel::setSyntaxColor(bool dark, const std::string& languageKey, model::Token token, std::optional<uint32_t> rgb)
{
    syntaxColors_.setColor(dark, languageKey, token, rgb);
}

void NotepadViewModel::resetSyntaxColors(bool dark, const std::string& languageKey)
{
    syntaxColors_.reset(dark, languageKey);
}

bool NotepadViewModel::hasSyntaxColors(bool dark, const std::string& languageKey) const
{
    return syntaxColors_.hasColors(dark, languageKey);
}

} // namespace enp::viewmodel
