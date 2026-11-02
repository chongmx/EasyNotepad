#include "enp/viewmodel/IntellisenseViewModel.h"

#include "enp/model/Highlighter.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Memory.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <algorithm>
#include <cstdio>

namespace enp::viewmodel {

namespace {

std::atomic<int> g_runningBuilds{0};

// Read after the project's own code: what a project vendors is useful, but its own names come first.
bool isVendorFolder(const std::string& name)
{
    for (const char* v : {"3rdparty", "third_party", "thirdparty", "vendor", "external", "extern", "deps", "libs"})
        if (name == v)
            return true;
    return false;
}

// Build output and package caches: generated or someone else's, and often huge.
bool isSkippedFolder(const std::string& name)
{
    for (const char* v : {"node_modules", "build", "out", "dist", "target", "bin", "obj", "__pycache__", "venv", ".venv"})
        if (name == v)
            return true;
    return name.rfind("build-", 0) == 0 || name.rfind("cmake-build", 0) == 0;
}

bool looksBinary(const std::string& text)
{
    return text.find('\0') < std::min<size_t>(text.size(), 8192);
}

// A candidate while ranking: views into the document, the index or the language table, all
// alive for the duration of refresh().
struct Candidate {
    std::string_view label;
    model::SymbolKind kind = model::SymbolKind::Word;
    int score = 0;
    long symbol = -1; // position in the index, -1 for a keyword or a word of the document
};

} // namespace

IntellisenseViewModel::IntellisenseViewModel(NotepadViewModel& owner, interfaces::IFileService* files)
    : owner_(owner), files_(files)
{
}

IntellisenseViewModel::~IntellisenseViewModel()
{
    *alive_ = false;
    if (cancel_)
        cancel_->store(true); // a build still running stops early; its result is dropped
}

int IntellisenseViewModel::runningBuilds()
{
    return g_runningBuilds.load();
}

// ---- Index ----------------------------------------------------------------------------------------

std::vector<IntellisenseViewModel::IndexSource> IntellisenseViewModel::collect() const
{
    std::vector<IndexSource> sources;
    // Open documents in their current text (unsaved edits included).
    for (const auto& doc : owner_.documents()) {
        if (doc->kind != model::FileKind::Text || doc->path.empty() || !model::canExtractSymbols(doc->language))
            continue;
        sources.push_back({doc->path, {}, doc->buffer.text(), true, doc->language});
    }
    const bool real = !owner_.workspaceRoot().empty() && files_;
    auto isOpen = [&](const std::string& path) {
        for (const auto& doc : owner_.documents())
            if (doc->path == path)
                return true;
        return false;
    };
    // The explorer's tree (hidden folders already left out): the project first, vendored code after.
    auto walk = [&](auto&& self, const ExplorerItem& folder, bool vendorPass, bool inVendor) -> void {
        for (const ExplorerItem& item : folder.children) {
            if (sources.size() >= kMaxFiles)
                return;
            if (item.isDirectory) {
                if (isSkippedFolder(item.name))
                    continue;
                const bool vendor = inVendor || isVendorFolder(item.name);
                if (vendor && !vendorPass && !inVendor)
                    continue; // the second pass reads it
                self(self, item, vendorPass, vendor);
                continue;
            }
            if (vendorPass != inVendor)
                continue;
            const model::Language language = model::languageFromPath(item.name);
            if (!model::canExtractSymbols(language) || isOpen(item.path))
                continue;
            if (real)
                sources.push_back({item.path, owner_.absolutePath(item.path), {}, false, language});
            else // the sample workspace
                sources.push_back({item.path, {}, owner_.readWorkspaceFile(item.path), true, language});
        }
    };
    walk(walk, owner_.explorer(), false, false);
    walk(walk, owner_.explorer(), true, false);
    return sources;
}

void IntellisenseViewModel::build(std::vector<IndexSource>& sources, interfaces::IFileService* files, const std::atomic<bool>* cancel,
                                  model::SymbolIndex& out)
{
    std::vector<model::Symbol> symbols; // reused for every file
    size_t readBytes = 0;
    for (IndexSource& source : sources) {
        if (cancel && cancel->load())
            break;
        std::string text;
        if (source.hasText) {
            text.swap(source.text);
        } else {
            if (!files)
                continue;
            if (const std::optional<interfaces::FileStamp> stamp = files->stamp(source.disk); stamp && stamp->size > kMaxFileBytes)
                continue;
            std::optional<std::string> read = files->read(source.disk);
            if (!read)
                continue;
            text.swap(*read);
        }
        if (text.size() > kMaxFileBytes || looksBinary(text))
            continue;
        readBytes += text.size();
        if (readBytes > kMaxReadBytes)
            break;
        model::extractSymbols(text, source.language, int(out.fileCount()), symbols);
        os::mem::release(text);
        if (symbols.empty())
            continue;
        out.addFile(std::move(source.path));
        out.add(symbols);
        if (out.size() >= kMaxSymbols)
            break;
    }
    out.finish();
}

void IntellisenseViewModel::ensureIndex()
{
    if (built_ || building_)
        return;
    if (owner_.workspaceRoot().empty() && owner_.explorer().children.empty() && owner_.documents().empty())
        return;
    os::mem::Scope scope("intellisense");
    building_ = true;
    indexing.set(true);
    auto cancel = std::make_shared<std::atomic<bool>>(false);
    cancel_ = cancel;
    std::vector<IndexSource> sources = collect();
    const int generation = generation_;
    std::weak_ptr<bool> alive = alive_;
    ++g_runningBuilds;
    os::dispatch::runAsync([this, alive, generation, cancel, sources = std::move(sources), files = files_]() mutable {
        os::mem::Scope scope("intellisense");
        model::SymbolIndex index;
        build(sources, files, cancel.get(), index);
        os::mem::release(sources); // the inputs go as soon as they are read
        cancel.reset();
        // Only `this` once on the main thread, and only while it lives and the workspace is the
        // same: otherwise the index is freed with this task.
        os::dispatch::postToMain([this, alive, generation, index = std::move(index)]() mutable {
            if (alive.expired() || generation != generation_)
                return;
            apply(std::move(index));
        });
        --g_runningBuilds;
    });
}

void IntellisenseViewModel::buildIndexNow()
{
    invalidate();
    model::SymbolIndex index;
    {
        os::mem::Scope scope("intellisense");
        std::vector<IndexSource> sources = collect();
        build(sources, files_, nullptr, index);
    }
    apply(std::move(index));
}

void IntellisenseViewModel::rebuildIndex()
{
    invalidate();
    ensureIndex();
    if (building_)
        status("IntelliSense: reading the workspace again");
}

void IntellisenseViewModel::status(const std::string& text)
{
    // The status bar's text is the main view-model's: whatever it allocates is not IntelliSense's
    // (it outlives the folder), so it is not counted in the "intellisense" scope.
    os::mem::Scope unattributed("other");
    owner_.statusText.set(text);
}

void IntellisenseViewModel::apply(model::SymbolIndex index)
{
    {
        os::mem::Scope scope("intellisense");
        index_ = std::move(index); // the previous index (empty: released before a build) goes
    }
    built_ = true;
    building_ = false;
    cancel_.reset();
    indexing.set(false);
    char message[128];
    std::snprintf(message, sizeof(message), "IntelliSense: %zu names from %zu files", index_.size(), index_.fileCount());
    status(message);
    if (active_)
        revision_ = ~uint64_t(0); // the open suggestions include the workspace now
    if (!pendingDefinition_.empty()) {
        const model::Document* doc = owner_.activeDocument();
        const bool same = doc && doc->id == pendingDocument_;
        os::mem::release(pendingDefinition_);
        pendingDocument_ = -1;
        if (same)
            goToDefinition();
    }
}

void IntellisenseViewModel::workspaceChanged()
{
    // A build about the folder that is going away: stop it, and ignore its answer.
    ++generation_;
    if (cancel_) {
        cancel_->store(true);
        cancel_.reset();
    }
    built_ = building_ = false;
    indexing.set(false);
    index_.release();
    dismiss();
    os::mem::release(pendingDefinition_);
    pendingDocument_ = -1;
    os::mem::release(back_); // places in the folder that closed
    os::mem::release(forward_);
}

void IntellisenseViewModel::invalidate()
{
    const bool large = !index_.empty();
    workspaceChanged();
    if (large)
        owner_.requestMemoryTrim(); // released outside the close path (docs/MEMORY.md rule 9)
}

void IntellisenseViewModel::documentSaved(const model::Document& doc)
{
    if (!built_ || doc.path.empty() || !model::canExtractSymbols(doc.language))
        return;
    os::mem::Scope scope("intellisense");
    std::vector<model::Symbol> symbols;
    model::extractSymbols(doc.savedText, doc.language, -1, symbols);
    index_.replaceFile(doc.path, symbols);
}

// ---- Completion -----------------------------------------------------------------------------------

bool IntellisenseViewModel::inCodeAt(const model::Document& doc, model::TextPos pos) const
{
    if (pos.column <= 0)
        return true;
    const model::LanguageSpec& spec = model::languageSpec(doc.language);
    int state = doc.highlightCache.stateAt(doc.buffer, spec, pos.line);
    std::vector<model::Token> tokens;
    model::highlightLine(doc.buffer.line(pos.line), spec, state, tokens);
    const size_t at = size_t(pos.column - 1);
    if (at >= tokens.size())
        return true;
    return tokens[at] != model::Token::Comment && tokens[at] != model::Token::String;
}

void IntellisenseViewModel::trigger()
{
    model::Document* doc = owner_.activeDocument();
    if (!doc || doc->kind != model::FileKind::Text)
        return;
    if (active_)
        dismiss();
    if (doc->multiCursor()) { // each caret has its own word: one list cannot complete them all
        status("Suggestions work with one caret (Esc goes back to one)");
        return;
    }
    start(true);
}

void IntellisenseViewModel::typed(std::string_view text)
{
    if (active_ || !autoComplete.get() || text.empty())
        return;
    const model::Document* doc = owner_.activeDocument();
    if (!doc || doc->kind != model::FileKind::Text || doc->hasSelection() || doc->multiCursor() ||
        doc->language == model::Language::PlainText)
        return;
    if (model::languageSpec(doc->language).lexer != model::Lexer::Code)
        return;
    const std::string& line = doc->buffer.line(doc->caret.line);
    const int column = doc->caret.column;
    if (column <= 0 || column > int(line.size()))
        return;
    const char c = line[size_t(column - 1)];
    if (model::isIdentifierChar(c)) {
        const auto [b, e] = model::identifierAround(line, column);
        if (b == e || column - b < kAutoTriggerLength)
            return;
    } else if (c == '.' || (c == '>' && column >= 2 && line[size_t(column - 2)] == '-') ||
               (c == ':' && column >= 2 && line[size_t(column - 2)] == ':')) {
        // Something with members before it: a name, a call or an index, not a number ("1.").
        const int before = column - (c == '.' ? 1 : 2);
        if (before <= 0)
            return;
        const char p = line[size_t(before - 1)];
        if (!(model::isIdentifierChar(p) || p == ')' || p == ']'))
            return;
        const auto [b, e] = model::identifierAround(line, before);
        if (model::isIdentifierChar(p) && b == e)
            return; // digits
    } else {
        return;
    }
    if (!inCodeAt(*doc, doc->caret))
        return;
    start(false);
}

void IntellisenseViewModel::start(bool explicitRequest)
{
    const model::Document* doc = owner_.activeDocument();
    if (!doc)
        return;
    const std::string& line = doc->buffer.line(doc->caret.line);
    const int column = std::min(doc->caret.column, int(line.size()));
    int begin = column;
    while (begin > 0 && model::isIdentifierChar(line[size_t(begin - 1)]))
        --begin;
    while (begin < column && !model::isIdentifierStart(line[size_t(begin)]))
        ++begin;
    if (begin < column && line[size_t(begin)] >= '0' && line[size_t(begin)] <= '9')
        return; // a number
    anchor_ = {doc->caret.line, begin};
    // What comes before the word: "Name::" (members of Name), "." or "->" (members).
    os::mem::release(memberOf_);
    member_ = false;
    if (begin >= 2 && line[size_t(begin - 1)] == ':' && line[size_t(begin - 2)] == ':') {
        const auto [b, e] = model::identifierAround(line, begin - 2);
        if (b < e) {
            os::mem::Scope scope("intellisense");
            memberOf_.assign(line, size_t(b), size_t(e - b));
        }
    } else if ((begin >= 1 && line[size_t(begin - 1)] == '.') || (begin >= 2 && line[size_t(begin - 1)] == '>' && line[size_t(begin - 2)] == '-')) {
        member_ = true;
    }
    active_ = true;
    explicit_ = explicitRequest;
    documentId_ = doc->id;
    ensureIndex();
    refresh();
    if (items_.empty() && !provider_) { // with a provider, its answer may still fill the list
        if (explicitRequest)
            status("No suggestions here");
        dismiss();
    } else if (!explicitRequest && items_.size() == 1 && items_[0].label == prefix_) {
        dismiss(); // the word is complete already
    }
}

void IntellisenseViewModel::sync()
{
    if (!active_)
        return;
    const model::Document* doc = owner_.activeDocument();
    if (!doc || doc->id != documentId_ || doc->kind != model::FileKind::Text) {
        dismiss();
        return;
    }
    if (doc->revision == revision_ && doc->caret == caret_)
        return;
    const model::TextPos caret = doc->caret;
    if (doc->hasSelection() || doc->multiCursor() || caret.line != anchor_.line || caret.column < anchor_.column || anchor_.line >= doc->buffer.lineCount()) {
        dismiss();
        return;
    }
    const std::string& line = doc->buffer.line(anchor_.line);
    if (caret.column > int(line.size())) {
        dismiss();
        return;
    }
    for (int i = anchor_.column; i < caret.column; ++i)
        if (!model::isIdentifierChar(line[size_t(i)])) {
            dismiss();
            return;
        }
    const std::string_view typed(line.data() + anchor_.column, size_t(caret.column - anchor_.column));
    if (typed == prefix_ && revision_ != ~uint64_t(0)) {
        revision_ = doc->revision;
        caret_ = caret;
        return;
    }
    refresh();
    if ((items_.empty() && !provider_) || (!explicit_ && items_.size() == 1 && items_[0].label == prefix_))
        dismiss();
}

void IntellisenseViewModel::refresh()
{
    const model::Document* doc = owner_.activeDocument();
    if (!doc)
        return;
    refreshItems(*doc);
    ask(*doc); // outside the scope: the provider's own work is not IntelliSense's memory
}

void IntellisenseViewModel::refreshItems(const model::Document& document)
{
    const model::Document* doc = &document;
    os::mem::Scope scope("intellisense");
    const model::TextPos caret = doc->caret;
    const std::string& caretLine = doc->buffer.line(caret.line);
    prefix_.assign(caretLine, size_t(anchor_.column), size_t(std::max(0, caret.column - anchor_.column)));
    revision_ = doc->revision;
    caret_ = caret;

    std::vector<Candidate> candidates;
    const bool members = member_ || !memberOf_.empty();
    // The language's words.
    if (!members) {
        const model::LanguageSpec& spec = model::languageSpec(doc->language);
        for (const auto* set : {&spec.keywords, &spec.types})
            for (const std::string& word : *set)
                if (const int score = model::matchScore(prefix_, word); score >= 0)
                    candidates.push_back({word, model::SymbolKind::Keyword, score, -1});
    }
    // The document's words (members: only those used after "." or "->"), except the one typed.
    {
        size_t scanned = 0;
        const int lines = doc->buffer.lineCount();
        for (int l = 0; l < lines && scanned < kMaxScanBytes; ++l) {
            const std::string& text = doc->buffer.line(l);
            scanned += text.size() + 1;
            size_t i = 0;
            while (i < text.size()) {
                if (!model::isIdentifierStart(text[i]) || (i > 0 && model::isIdentifierChar(text[i - 1]))) {
                    ++i;
                    continue;
                }
                size_t e = i;
                while (e < text.size() && model::isIdentifierChar(text[e]))
                    ++e;
                const bool typedHere = l == caret.line && int(i) == anchor_.column;
                const bool afterMember = (i >= 1 && text[i - 1] == '.') || (i >= 2 && text[i - 1] == '>' && text[i - 2] == '-');
                if (!typedHere && e - i >= 2 && (!member_ || afterMember) && memberOf_.empty()) {
                    const std::string_view word(text.data() + i, e - i);
                    if (const int score = model::matchScore(prefix_, word); score >= 0)
                        candidates.push_back({word, model::SymbolKind::Word, score + 2, -1});
                }
                i = e;
            }
        }
    }
    // The workspace's declarations.
    const int ownFile = doc->path.empty() ? -1 : index_.fileIndex(doc->path);
    for (size_t i = 0; i < index_.size(); ++i) {
        const model::SymbolView s = index_.at(i);
        if (!memberOf_.empty() && s.container != memberOf_)
            continue;
        if (member_ && (s.container.empty() || s.kind == model::SymbolKind::Type || s.kind == model::SymbolKind::Namespace))
            continue;
        if (const int score = model::matchScore(prefix_, s.name); score >= 0)
            candidates.push_back({s.name, s.kind, score + (s.file == ownFile ? 2 : 1), long(i)});
    }

    // One suggestion per name: the best score, and the declaration when there is one.
    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) { return a.label < b.label; });
    size_t kept = 0;
    for (size_t i = 0; i < candidates.size();) {
        Candidate best = candidates[i];
        size_t j = i + 1;
        for (; j < candidates.size() && candidates[j].label == best.label; ++j) {
            const Candidate& c = candidates[j];
            best.score = std::max(best.score, c.score);
            if (c.symbol >= 0 && (best.symbol < 0 || (index_.at(size_t(c.symbol)).definition && !index_.at(size_t(best.symbol)).definition))) {
                best.symbol = c.symbol;
                best.kind = c.kind;
            } else if (best.symbol < 0 && c.kind == model::SymbolKind::Keyword) {
                best.kind = c.kind;
            }
        }
        candidates[kept++] = best;
        i = j;
    }
    candidates.resize(kept);
    const size_t shown = std::min(candidates.size(), size_t(kMaxItems));
    auto better = [](const Candidate& a, const Candidate& b) {
        if (a.score != b.score)
            return a.score > b.score;
        if (a.label.size() != b.label.size())
            return a.label.size() < b.label.size();
        return a.label < b.label;
    };
    std::partial_sort(candidates.begin(), candidates.begin() + std::ptrdiff_t(shown), candidates.end(), better);

    std::vector<Item> items;
    items.reserve(shown + provided_.size());
    // The provider's answer first, while it still fits what is typed.
    for (const Item& p : provided_)
        if (model::matchScore(prefix_, p.label) >= 0)
            items.push_back(p);
    for (size_t i = 0; i < shown; ++i) {
        const Candidate& c = candidates[i];
        Item& item = items.emplace_back();
        item.label.assign(c.label.data(), c.label.size());
        item.kind = c.kind;
        item.score = c.score;
        item.source = c.symbol >= 0 ? Source::Workspace : c.kind == model::SymbolKind::Keyword ? Source::Language : Source::Document;
        if (c.symbol >= 0) {
            const model::SymbolView s = index_.at(size_t(c.symbol));
            item.detail.assign(s.detail.data(), s.detail.size());
            const std::string_view file = index_.path(s.file);
            item.location.assign(file.data(), file.size());
            item.location += ':' + std::to_string(s.line + 1);
        } else if (c.kind == model::SymbolKind::Keyword) {
            item.detail = "keyword";
        }
    }
    items_.swap(items); // the previous suggestions are freed with `items`
    selected_ = 0;
    ++selectionSerial_;
}

// ---- AI completion --------------------------------------------------------------------------------

void IntellisenseViewModel::setProvider(Provider provider)
{
    provider_ = std::move(provider);
    ++requestSerial_; // answers to requests of the previous provider are dropped
}

void IntellisenseViewModel::ask(const model::Document& doc)
{
    if (!provider_)
        return;
    Request request; // built in IntelliSense's scope, freed when the provider returns
    request.serial = ++requestSerial_;
    request.documentId = doc.id;
    request.path = doc.path;
    request.language = doc.language;
    request.prefix = prefix_;
    const model::TextPos caret = doc.caret;
    const int lines = doc.buffer.lineCount();
    request.before = doc.buffer.text({std::max(0, caret.line - 120), 0}, caret);
    if (request.before.size() > kRequestBefore) {
        size_t cut = request.before.size() - kRequestBefore;
        while (cut < request.before.size() && (static_cast<unsigned char>(request.before[cut]) & 0xC0) == 0x80)
            ++cut; // on a character boundary
        request.before.erase(0, cut);
    }
    const int lastLine = std::min(lines - 1, caret.line + 40);
    request.after = doc.buffer.text(caret, {lastLine, int(doc.buffer.line(lastLine).size())});
    if (request.after.size() > kRequestAfter) {
        size_t cut = kRequestAfter;
        while (cut > 0 && (static_cast<unsigned char>(request.after[cut]) & 0xC0) == 0x80)
            --cut;
        request.after.resize(cut);
    }
    {
        os::mem::Scope scope("intellisense");
        request.context = contextFor(prefix_, doc.path, request.before);
    }
    provider_(request);
}

void IntellisenseViewModel::provide(int serial, std::vector<Item> items)
{
    if (!active_ || serial != requestSerial_)
        return; // a late answer: the list closed or moved on (`items` is freed here)
    os::mem::Scope scope("intellisense");
    if (items.size() > size_t(kMaxProvided))
        items.resize(size_t(kMaxProvided));
    for (Item& item : items) {
        item.source = Source::Provider;
        if (item.label.empty()) // the first line of what it inserts
            item.label = item.insertText.substr(0, item.insertText.find('\n'));
    }
    items.erase(std::remove_if(items.begin(), items.end(), [](const Item& i) { return i.label.empty(); }), items.end());
    if (items.empty() && items_.empty()) {
        dismiss();
        return;
    }
    // Shown first, before the names (any earlier answer is replaced).
    std::vector<Item> merged;
    merged.reserve(items.size() + items_.size());
    for (const Item& item : items)
        merged.push_back(item);
    for (Item& item : items_)
        if (item.source != Source::Provider)
            merged.push_back(std::move(item));
    items_.swap(merged);
    provided_.swap(items); // the previous answer goes with `items`
    selected_ = 0;
    ++selectionSerial_;
}

const char* IntellisenseViewModel::contextReasonName(ContextReason reason)
{
    switch (reason) {
    case ContextReason::Typed: return "typed";
    case ContextReason::UsedNearby: return "used nearby";
    case ContextReason::SameFile: return "same file";
    }
    return "";
}

std::vector<IntellisenseViewModel::ContextSnippet> IntellisenseViewModel::contextFor(std::string_view prefix, std::string_view path,
                                                                                     std::string_view nearby,
                                                                                     const ContextBudget& budget) const
{
    std::vector<ContextSnippet> out;
    if (index_.empty() || budget.maxSnippets <= 0)
        return out;
    struct Pick {
        int score;
        ContextReason reason;
        size_t symbol; // position in the index
    };
    std::vector<Pick> picks;
    const int ownFile = path.empty() ? -1 : index_.fileIndex(path);
    // 1. What is being typed: the best matches (exact-case prefixes first).
    if (!prefix.empty()) {
        for (size_t i = 0; i < index_.size(); ++i)
            if (const int score = model::matchScore(prefix, index_.at(i).name); score >= 0)
                picks.push_back({2000 + score, ContextReason::Typed, i});
        const size_t keep = std::min<size_t>(picks.size(), 16);
        std::partial_sort(picks.begin(), picks.begin() + std::ptrdiff_t(keep), picks.end(),
                          [](const Pick& a, const Pick& b) { return a.score > b.score; });
        picks.resize(keep);
    }
    // 2. What the code before the caret uses and declares elsewhere: the nearer, the better.
    {
        std::vector<std::pair<std::string_view, size_t>> names; // name, last offset
        for (size_t i = 0; i < nearby.size();) {
            if (!model::isIdentifierStart(nearby[i]) || (i > 0 && model::isIdentifierChar(nearby[i - 1]))) {
                ++i;
                continue;
            }
            size_t e = i;
            while (e < nearby.size() && model::isIdentifierChar(nearby[e]))
                ++e;
            if (e - i >= 2)
                names.push_back({nearby.substr(i, e - i), i});
            i = e;
        }
        std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.first < b.first || (a.first == b.first && a.second > b.second); });
        names.erase(std::unique(names.begin(), names.end(), [](const auto& a, const auto& b) { return a.first == b.first; }), names.end());
        for (const auto& [name, offset] : names) {
            if (name == prefix)
                continue;
            const long s = best(name, {}, {});
            if (s < 0 || index_.at(size_t(s)).file == ownFile)
                continue; // the model reads this file's code itself
            const int nearness = nearby.empty() ? 0 : int(100.0 * double(offset) / double(nearby.size()));
            picks.push_back({1000 + nearness, ContextReason::UsedNearby, size_t(s)});
        }
    }
    // 3. The file's own types and functions, in file order.
    if (ownFile >= 0) {
        int order = 0;
        for (size_t i = 0; i < index_.size(); ++i) {
            const model::SymbolView s = index_.at(i);
            if (s.file == ownFile && (s.kind == model::SymbolKind::Type || s.kind == model::SymbolKind::Function))
                picks.push_back({500 - std::min(order++, 400), ContextReason::SameFile, i});
        }
    }
    // One snippet per declaration (its best reason), best first, within the budget.
    std::sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) {
        return a.symbol != b.symbol ? a.symbol < b.symbol : a.score > b.score;
    });
    picks.erase(std::unique(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.symbol == b.symbol; }), picks.end());
    std::sort(picks.begin(), picks.end(), [this](const Pick& a, const Pick& b) {
        return a.score != b.score ? a.score > b.score : index_.at(a.symbol).name < index_.at(b.symbol).name;
    });
    size_t bytes = 0;
    for (const Pick& p : picks) {
        if (int(out.size()) >= budget.maxSnippets)
            break;
        const model::SymbolView s = index_.at(p.symbol);
        const std::string_view file = index_.path(s.file);
        const size_t size = s.name.size() + s.container.size() + file.size() + s.detail.size() + 48;
        if (bytes + size > budget.maxBytes)
            continue; // a shorter one may still fit
        bytes += size;
        out.push_back({p.reason, s.kind, std::string(s.name), std::string(s.container), std::string(file), s.line, std::string(s.detail), p.score});
    }
    return out;
}

std::vector<IntellisenseViewModel::ContextSnippet> IntellisenseViewModel::contextFor(std::string_view prefix, std::string_view path,
                                                                                     std::string_view nearby) const
{
    return contextFor(prefix, path, nearby, ContextBudget{});
}

std::string IntellisenseViewModel::formatContext(const std::vector<ContextSnippet>& snippets)
{
    std::string out;
    for (const ContextSnippet& c : snippets) {
        out += model::symbolKindName(c.kind);
        out += ' ';
        out += c.name;
        if (!c.container.empty()) {
            out += " in ";
            out += c.container;
        }
        out += " (" + c.path + ":" + std::to_string(c.line + 1) + ", " + contextReasonName(c.reason) + "): ";
        out += c.text;
        out += '\n';
    }
    return out;
}

void IntellisenseViewModel::move(int delta)
{
    if (items_.empty())
        return;
    selected_ = std::clamp(selected_ + delta, 0, int(items_.size()) - 1);
    ++selectionSerial_;
}

void IntellisenseViewModel::select(int index)
{
    if (items_.empty())
        return;
    selected_ = std::clamp(index, 0, int(items_.size()) - 1);
    ++selectionSerial_;
}

bool IntellisenseViewModel::accept()
{
    sync();
    if (!active_ || items_.empty())
        return false;
    const model::Document* doc = owner_.activeDocument();
    if (!doc)
        return false;
    Item& chosen = items_[size_t(selected_)];
    std::string text = std::move(chosen.insertText.empty() ? chosen.label : chosen.insertText);
    const model::TextPos from = anchor_;
    const model::TextPos to = doc->caret;
    const std::string typed = prefix_;
    dismiss();
    if (text.compare(0, typed.size(), typed) == 0) {
        // Only the rest is typed: undo and macros see an ordinary insertion at the caret.
        if (text.size() > typed.size())
            owner_.insertText(std::string_view(text).substr(typed.size()));
    } else {
        owner_.setSelection(from, to);
        owner_.insertText(text);
    }
    return true;
}

void IntellisenseViewModel::dismiss()
{
    active_ = false;
    explicit_ = false;
    member_ = false;
    documentId_ = -1;
    selected_ = 0;
    os::mem::release(items_);
    os::mem::release(provided_);
    os::mem::release(prefix_);
    os::mem::release(memberOf_);
    ++requestSerial_; // a provider's answer still on its way is dropped
}

// ---- Navigation -----------------------------------------------------------------------------------

long IntellisenseViewModel::best(std::string_view name, std::string_view preferPath, std::string_view skipPath) const
{
    const auto [first, last] = index_.named(name);
    long found = -1;
    int foundScore = -1;
    for (size_t i = first; i < last; ++i) {
        const model::SymbolView s = index_.at(i);
        const std::string_view file = index_.path(s.file);
        if (!skipPath.empty() && file == skipPath)
            continue;
        const int score = (s.definition ? 2 : 0) + (!preferPath.empty() && file == preferPath ? 1 : 0);
        if (score > foundScore) {
            found = long(i);
            foundScore = score;
        }
    }
    return found;
}

std::optional<model::SymbolView> IntellisenseViewModel::lookup(std::string_view name, std::string_view preferPath) const
{
    const long i = best(name, preferPath, {});
    if (i < 0)
        return std::nullopt;
    return index_.at(size_t(i));
}

bool IntellisenseViewModel::jumpTo(const std::string& path, int line, int column)
{
    owner_.navigateTo(path, line);
    const model::Document* doc = owner_.activeDocument();
    if (!doc || doc->path != path) {
        status("Cannot open " + path);
        return false;
    }
    owner_.setCaret({line, column}, false);
    owner_.caretRevealRequested.emit();
    return true;
}

bool IntellisenseViewModel::goToDefinition()
{
    model::Document* doc = owner_.activeDocument();
    if (!doc || doc->kind != model::FileKind::Text)
        return false;
    dismiss();
    const std::string& line = doc->buffer.line(doc->caret.line);
    const auto [b, e] = model::identifierAround(line, doc->caret.column);
    if (b == e) {
        status("Put the caret on a name to go to its definition");
        return false;
    }
    const std::string name = line.substr(size_t(b), size_t(e - b));
    const int caretLine = doc->caret.line;

    // This document, as it is now. (Only this search is IntelliSense's memory: opening a file and
    // moving the caret below belong to the documents and are counted there.)
    bool found = false, foundDefinition = false, onDeclaration = false;
    int localLine = 0, localColumn = 0;
    if (model::canExtractSymbols(doc->language)) {
        os::mem::Scope scope("intellisense");
        std::vector<model::Symbol> symbols;
        model::extractSymbols(doc->buffer.text(), doc->language, -1, symbols);
        for (const model::Symbol& s : symbols) {
            if (s.name != name)
                continue;
            if (s.line == caretLine) {
                onDeclaration = true;
                continue;
            }
            if (!found || (s.definition && !foundDefinition)) {
                found = true;
                foundDefinition = s.definition;
                localLine = s.line;
                localColumn = s.column;
            }
        }
    }
    auto goLocal = [&]() {
        remember();
        owner_.goToLine(localLine);
        owner_.setCaret({localLine, localColumn}, false);
        owner_.caretRevealRequested.emit();
        status("Definition of " + name + ": line " + std::to_string(localLine + 1));
        return true;
    };
    if (found && (!onDeclaration || foundDefinition))
        return goLocal();

    // The workspace.
    if (!built_) {
        ensureIndex();
        if (!built_) {
            if (!building_) {
                status("No definition found for " + name);
                return false;
            }
            {
                os::mem::Scope scope("intellisense");
                pendingDefinition_ = name;
            }
            pendingDocument_ = doc->id;
            status("IntelliSense: reading the workspace to find " + name + "...");
            return false;
        }
    }
    // Elsewhere in the workspace (this document was searched above, in its current text).
    if (const long i = best(name, {}, doc->path); i >= 0) {
        const model::SymbolView target = index_.at(size_t(i));
        const std::string path(index_.path(target.file)); // a copy: opening the file may change the index
        const int targetLine = target.line, targetColumn = target.column;
        remember();
        if (!jumpTo(path, targetLine, targetColumn)) {
            back_.pop_back(); // nowhere was reached
            return false;
        }
        status("Definition of " + name + ": " + path + ":" + std::to_string(targetLine + 1));
        return true;
    }
    if (found)
        return goLocal();
    status(onDeclaration ? name + " is declared here" : "No definition found for " + name);
    return false;
}

// ---- Jump history ---------------------------------------------------------------------------------

IntellisenseViewModel::Place IntellisenseViewModel::here() const
{
    Place place;
    if (const model::Document* doc = owner_.activeDocument()) {
        place.documentId = doc->id;
        place.path = doc->path;
        place.caret = doc->caret;
    }
    return place;
}

void IntellisenseViewModel::remember()
{
    if (!owner_.activeDocument())
        return;
    os::mem::Scope scope("intellisense");
    back_.push_back(here());
    if (back_.size() > kMaxHistory)
        back_.erase(back_.begin());
    os::mem::release(forward_); // a new jump starts a new way forward
}

bool IntellisenseViewModel::show(const Place& place)
{
    const int index = owner_.documentIndexById(place.documentId);
    if (index >= 0 && owner_.documents()[size_t(index)]->path == place.path) {
        owner_.activate(index);
    } else if (!place.path.empty()) {
        owner_.openFile(place.path); // closed since: opened again
        const model::Document* doc = owner_.activeDocument();
        if (!doc || doc->path != place.path)
            return false;
    } else {
        return false; // an untitled document that closed
    }
    owner_.goToLine(place.caret.line);
    owner_.setCaret(place.caret, false); // clamped to the text as it is now
    owner_.caretRevealRequested.emit();
    return true;
}

bool IntellisenseViewModel::step(std::vector<Place>& from, std::vector<Place>& to, const char* done, const char* none)
{
    dismiss();
    while (!from.empty()) {
        Place target;
        Place current;
        {
            os::mem::Scope scope("intellisense");
            target = std::move(from.back());
            from.pop_back();
            current = here();
        }
        if (!show(target))
            continue; // gone: the next one
        if (current.documentId >= 0) {
            os::mem::Scope scope("intellisense");
            to.push_back(std::move(current));
            if (to.size() > kMaxHistory)
                to.erase(to.begin());
        }
        status(done);
        return true;
    }
    status(none);
    return false;
}

bool IntellisenseViewModel::goBack()
{
    return step(back_, forward_, "Back to where Go to Definition started", "Nothing to go back to");
}

bool IntellisenseViewModel::goForward()
{
    return step(forward_, back_, "Forward again", "Nothing to go forward to");
}

} // namespace enp::viewmodel
