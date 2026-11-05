// Find in Files: searches the workspace (open documents in their current, unsaved text) on a
// background thread, groups the results by file, opens them, and replaces across files.
#pragma once

#include "enp/interfaces/IFileService.h"
#include "enp/model/Search.h"
#include "enp/viewmodel/Observable.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace enp::viewmodel {

class NotepadViewModel;

class FindInFilesViewModel {
public:
    struct Hit {
        int line = 0;
        int column = 0;
        int length = 0;
        std::string text; // the line (trimmed to a reasonable length)
    };
    struct FileResult {
        std::string path; // workspace-relative (or as opened)
        std::vector<Hit> hits;
    };

    static constexpr size_t kMaxFileBytes = 4 * 1024 * 1024; // larger files are skipped
    static constexpr int kMaxHits = 20000;                    // stop collecting beyond this

    FindInFilesViewModel(NotepadViewModel& owner, interfaces::IFileService* files);
    ~FindInFilesViewModel();

    std::string query;
    std::string replacement;
    std::string include; // "*.cpp;*.h"; empty: all files
    std::string folder;  // workspace-relative: only files under it (Explorer > Find in Folder); empty: all
    model::SearchOptions options;

    Property<bool> running{false};
    Property<std::string> error;

    void search();               // asynchronous; results arrive on the main thread
    void searchNow();            // synchronous (tests, small workspaces)
    void open(const std::string& path, const Hit& hit);
    int replaceAll();            // files changed; open documents are edited (undoable), others written
    void clear();

    const std::vector<FileResult>& results() const { return results_; }
    int hitCount() const { return hitCount_; }
    int filesSearched() const { return filesSearched_; }
    bool truncated() const { return truncated_; }
    int generation() const { return generation_; }
    // The running search's progress (files read of files to read), updated by its thread.
    int progressDone() const { return progress_ ? progress_->done.load(std::memory_order_relaxed) : 0; }
    int progressTotal() const { return progress_ ? progress_->total : 0; }

private:
    // Shared with the search's thread: it counts, and stops when cancelled (a newer search or a
    // clear), instead of reading the rest of the workspace for nothing.
    struct Progress {
        std::atomic<int> done{0};
        int total = 0; // set before the thread starts
        std::atomic<bool> cancel{false};
    };
    struct Source {
        std::string path;    // as shown
        std::string disk;    // absolute, for reading
        std::string text;    // filled for open documents
        bool hasText = false;
    };
    struct Outcome {
        std::vector<FileResult> results;
        int hits = 0;
        int files = 0;
        bool truncated = false;
        std::string error;
    };
    std::vector<Source> collect() const;
    static Outcome run(const std::vector<Source>& sources, const std::string& query, const model::SearchOptions& options,
                       interfaces::IFileService* files, Progress* progress = nullptr);
    void apply(Outcome outcome);
    void cancelRunning(); // stops the running search's thread and forgets its progress

    NotepadViewModel& owner_;
    interfaces::IFileService* files_;
    std::vector<FileResult> results_;
    int hitCount_ = 0;
    int filesSearched_ = 0;
    bool truncated_ = false;
    int generation_ = 0;
    std::shared_ptr<Progress> progress_; // only while a search runs
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace enp::viewmodel
