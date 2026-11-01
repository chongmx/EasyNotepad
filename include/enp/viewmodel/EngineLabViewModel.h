// The developer "Engine lab" (plan 23): downloads the local model and its drafter, and measures how
// fast they run on this device, with and without speculative decoding, on the CPU or the GPU.
// Hidden unless developer mode is on.
#pragma once

#include "enp/interfaces/ILocalEngineService.h"
#include "enp/model/Json.h"
#include "enp/viewmodel/Observable.h"

#include <string>
#include <vector>

namespace enp::viewmodel {

class EngineLabViewModel {
public:
    // A file the lab can fetch: pinned to a repository commit and checked by SHA-256.
    struct CatalogFile {
        const char* label;
        const char* fileName;
        const char* url;
        const char* sha256;
        uint64_t size;
        const char* origin;
    };
    static const CatalogFile& modelFile();
    static const CatalogFile& drafterFile();

    enum class FileState { Missing, Partial, Downloading, Verifying, Unverified, Ready, Mismatch, Custom };
    struct FileStatus {
        FileState state = FileState::Missing;
        std::string path;
        uint64_t have = 0;
        uint64_t total = 0;
        double verifyFraction = 0;
        std::string note; // e.g. "waiting for Wi-Fi"
    };
    enum class Which { Model, Drafter };

    struct Record {
        std::string when; // local time, "2026-11-01 14:05"
        std::string device;
        interfaces::EngineRunSettings settings;
        interfaces::EngineRunResult result;
    };

    explicit EngineLabViewModel(interfaces::ILocalEngineService* engine = nullptr);

    bool hasEngine() const { return engine_ != nullptr; }
    const interfaces::EngineInfo& info();
    // Call while the lab is shown: follows downloads and starts the checksum when one finishes.
    void refresh();

    FileStatus fileStatus(Which which) const;
    std::string pathFor(Which which) const; // the file a run would use
    bool fileUsable(Which which) const;
    // On mobile data the download waits for confirmMobileDownload() (the view asks); on Wi-Fi it
    // starts at once. A download paused waiting for Wi-Fi can be moved to mobile data the same way.
    void download(Which which);
    void useMobileData(); // for the download in progress (it starts again)
    // Phones: copy a file picked in the system file picker (e.g. downloaded in the browser).
    bool canImport() const { return engine_ && engine_->canImport(); }
    void importFile(Which which);
    bool importing() const { return downloading_ >= 0 && importing_; }
    bool confirmationPending() const { return confirm_ >= 0; }
    Which pendingDownload() const { return Which(confirm_ < 0 ? 0 : confirm_); }
    void confirmMobileDownload();
    void cancelConfirmation() { confirm_ = -1; }
    bool waitingForWifi() const;
    void cancelDownload();
    void verify(Which which);
    bool downloading() const { return downloading_ >= 0; }

    // Optional: run a file that is already on the device instead of the catalogue's.
    Property<std::string> customModelPath;
    Property<std::string> customDrafterPath;

    Property<int> threads{4};
    Property<bool> useGpu{false};
    Property<bool> allowMobileData{false};
    Property<int> promptTokens{512};
    Property<int> generateTokens{128};

    // One run with the current settings; the comparison runs without and then with the drafter.
    void run(bool withDrafter);
    void runComparison();
    void cancelRun();
    bool running() const { return running_; }
    const std::string& stage() const { return stage_; }

    const std::vector<Record>& history() const { return history_; }
    void clearHistory();
    std::string report() const; // the history as text, newest first, to copy and share

    // Saved with the settings.
    model::Json save() const;
    void load(const model::Json& j);

    // The lab's own message line (a phone has no status bar): what just happened, or went wrong.
    const std::string& notice() const { return notice_; }
    bool noticeIsError() const { return noticeError_; }
    void clearNotice() { notice_.clear(); }
    // Why the last download (or check) of this file failed; shown on its row until it is retried.
    const std::string& downloadError(Which which) const;

    Event<> changed;                      // something saved changed
    Event<std::string> statusMessage;

private:
    void startRun(bool withDrafter, bool thenWithDrafter);
    void startDownload(Which which, bool mobileData);
    int confirm_ = -1; // Which, waiting for "download on mobile data?"
    bool importing_ = false; // the transfer is a copy from the file picker
    void say(const std::string& text, bool error = false);
    std::string notice_;
    bool noticeError_ = false;
    std::string downloadErrors_[2];
    std::string deviceLabel();

    interfaces::ILocalEngineService* engine_ = nullptr;
    interfaces::EngineInfo info_;
    bool infoLoaded_ = false;
    int downloading_ = -1; // Which, while a catalogue download runs
    interfaces::DownloadStatus lastDownload_;
    int verifying_ = -1;
    double verifyFraction_ = 0;
    std::vector<std::pair<std::string, std::string>> verified_; // path -> sha256 that matched
    std::vector<std::string> mismatched_;
    bool running_ = false;
    std::string stage_;
    std::vector<Record> history_;
    bool threadsChosen_ = false;
    std::vector<Subscription> subscriptions_;
};

} // namespace enp::viewmodel
