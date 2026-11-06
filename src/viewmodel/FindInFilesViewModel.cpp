#include "enp/viewmodel/FindInFilesViewModel.h"
#include "enp/os/Memory.h"

#include "enp/os/Dispatch.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <algorithm>

namespace enp::viewmodel {

namespace {

constexpr size_t kMaxLineShown = 240;

std::string fileNameOf(const std::string& path)
{
    const size_t slash = path.find_last_of("/\\");
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// Binary files (a NUL byte early on) are not searched.
bool looksBinary(const std::string& text)
{
    return text.find('\0', 0) < std::min<size_t>(text.size(), 8192);
}

std::string lineAt(const std::string& text, int line)
{
    size_t start = 0;
    for (int l = 0; l < line && start != std::string::npos; ++l) {
        start = text.find_first_of("\r\n", start);
        if (start == std::string::npos)
            break;
        start += (text[start] == '\r' && start + 1 < text.size() && text[start + 1] == '\n') ? 2 : 1;
    }
    if (start == std::string::npos || start > text.size())
        return {};
    const size_t end = text.find_first_of("\r\n", start);
    return text.substr(start, (end == std::string::npos ? text.size() : end) - start);
}

} // namespace

FindInFilesViewModel::FindInFilesViewModel(NotepadViewModel& owner, interfaces::IFileService* files)
    : owner_(owner), files_(files)
{
}

FindInFilesViewModel::~FindInFilesViewModel()
{
    *alive_ = false;
    cancelRunning();
}

std::vector<FindInFilesViewModel::Source> FindInFilesViewModel::collect() const
{
    std::vector<Source> sources;
    // Open documents first, in their current text (unsaved edits included).
    for (const auto& doc : owner_.documents()) {
        const std::string path = doc->path.empty() ? doc->title : doc->path;
        if (!model::matchesFilter(fileNameOf(path), include) || (!folder.empty() && path.rfind(folder + "/", 0) != 0))
            continue;
        sources.push_back({path, {}, doc->buffer.text(), true});
    }
    if (owner_.workspaceRoot().empty() || !files_)
        return sources;
    // Then the workspace's files (the explorer's tree: hidden and build folders already left out).
    auto walk = [&](auto&& self, const ExplorerItem& item) -> void {
        if (item.isDirectory) {
            for (const ExplorerItem& child : item.children)
                self(self, child);
            return;
        }
        if (!model::matchesFilter(item.name, include) || (!folder.empty() && item.path.rfind(folder + "/", 0) != 0))
            return;
        for (const Source& s : sources)
            if (s.path == item.path)
                return; // open: already searched
        sources.push_back({item.path, owner_.absolutePath(item.path), {}, false});
    };
    walk(walk, owner_.explorer());
    return sources;
}

FindInFilesViewModel::Outcome FindInFilesViewModel::run(const std::vector<Source>& sources, const std::string& query,
                                                        const model::SearchOptions& options, interfaces::IFileService* files,
                                                        Progress* progress)
{
    Outcome out;
    for (const Source& source : sources) {
        if (progress) {
            if (progress->cancel.load(std::memory_order_relaxed))
                return out; // superseded: nobody takes this outcome
            progress->done.fetch_add(1, std::memory_order_relaxed);
        }
        std::string text;
        if (source.hasText) {
            text = source.text;
        } else {
            std::optional<std::string> read = files ? files->read(source.disk) : std::nullopt;
            if (!read || read->size() > kMaxFileBytes || looksBinary(*read))
                continue;
            text = std::move(*read);
        }
        ++out.files;
        std::string error;
        const std::vector<model::TextMatch> matches = model::searchText(text, query, options, &error);
        if (!error.empty()) {
            out.error = error;
            return out;
        }
        if (matches.empty())
            continue;
        FileResult result{source.path, {}};
        std::string currentText;
        int currentLine = -1;
        for (const model::TextMatch& m : matches) {
            if (m.line != currentLine) {
                currentLine = m.line;
                currentText = lineAt(text, m.line);
                if (currentText.size() > kMaxLineShown)
                    currentText = currentText.substr(0, kMaxLineShown);
            }
            result.hits.push_back({m.line, m.column, m.length, currentText});
            if (++out.hits >= kMaxHits) {
                out.truncated = true;
                break;
            }
        }
        out.results.push_back(std::move(result));
        if (out.truncated)
            break;
    }
    return out;
}

void FindInFilesViewModel::apply(Outcome outcome)
{
    results_ = std::move(outcome.results);
    hitCount_ = outcome.hits;
    filesSearched_ = outcome.files;
    truncated_ = outcome.truncated;
    error.set(outcome.error);
    progress_.reset();
    running.set(false);
}

void FindInFilesViewModel::cancelRunning()
{
    if (progress_)
        progress_->cancel = true;
    progress_.reset();
}

void FindInFilesViewModel::search()
{
    if (query.empty()) {
        clear();
        return;
    }
    const int generation = ++generation_;
    cancelRunning();
    running.set(true);
    std::vector<Source> sources = collect();
    progress_ = std::make_shared<Progress>();
    progress_->total = int(sources.size());
    std::weak_ptr<bool> alive = alive_;
    os::dispatch::runAsync([this, alive, generation, progress = progress_, sources = std::move(sources), q = query, o = options,
                            files = files_]() {
        Outcome outcome = run(sources, q, o, files, progress.get());
        os::dispatch::postToMain([this, alive, generation, outcome = std::move(outcome)]() mutable {
            if (alive.expired() || generation != generation_)
                return; // superseded by a newer search
            apply(std::move(outcome));
        });
    });
}

void FindInFilesViewModel::searchNow()
{
    os::mem::Scope scope("find");
    ++generation_;
    cancelRunning();
    if (query.empty()) {
        clear();
        return;
    }
    apply(run(collect(), query, options, files_));
}

void FindInFilesViewModel::clear()
{
    ++generation_;
    cancelRunning();
    os::mem::release(results_); // not clear(): the storage goes too (docs/MEMORY.md)
    hitCount_ = filesSearched_ = 0;
    truncated_ = false;
    error.set({});
    running.set(false);
}

void FindInFilesViewModel::open(const std::string& path, const Hit& hit)
{
    owner_.navigateTo(path, hit.line);
    if (owner_.activeDocument())
        owner_.setSelection({hit.line, hit.column}, {hit.line, hit.column + hit.length});
}

int FindInFilesViewModel::replaceAll()
{
    if (query.empty())
        return 0;
    int changedFiles = 0, replacements = 0;
    for (const FileResult& result : results_) {
        std::string err;
        int count = 0;
        const int open = [&]() {
            for (int i = 0; i < int(owner_.documents().size()); ++i) {
                const auto& doc = owner_.documents()[size_t(i)];
                if ((doc->path.empty() ? doc->title : doc->path) == result.path)
                    return i;
            }
            return -1;
        }();
        if (open >= 0) {
            // Open documents are edited (one undo step each), not written behind the editor's back.
            const std::string before = owner_.documents()[size_t(open)]->buffer.text();
            const std::string after = model::replaceText(before, query, replacement, options, &count, &err);
            if (count > 0)
                owner_.applyText(result.path, after);
        } else if (files_) {
            const std::string disk = owner_.absolutePath(result.path);
            const std::optional<std::string> before = files_->read(disk);
            if (!before)
                continue;
            const std::string after = model::replaceText(*before, query, replacement, options, &count, &err);
            if (count > 0 && !files_->write(disk, after))
                continue;
        }
        if (!err.empty()) {
            error.set(err);
            break;
        }
        if (count > 0) {
            ++changedFiles;
            replacements += count;
        }
    }
    owner_.statusText.set("Replaced " + std::to_string(replacements) + " match(es) in " + std::to_string(changedFiles) + " file(s)");
    searchNow();
    return changedFiles;
}

} // namespace enp::viewmodel
