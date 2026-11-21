// ILocalEngineService over llama.cpp (when the build has it: ENP_LOCAL_ENGINE), the OS downloader
// and a streaming SHA-256. The CPU engine runs in the app's own process. A faster engine library
// next to the app (enp-engine-cuda) runs in a helper process (EngineHelper); when it is absent,
// cannot start or fails, the CPU answers, as if it were not there (plan 21).
#pragma once

#include "enp/interfaces/ILocalEngineService.h"

#include <atomic>
#include <memory>
#include <optional>
#include <vector>

namespace enp::services {

// The helper process's side: `EasyNotepad --local-engine <library>` loads the engine library and
// serves the app's requests over stdin and stdout until its input ends. Returns the exit code.
int runLocalEngineHelper(const std::string& library);

class LocalEngineService final : public interfaces::ILocalEngineService {
public:
    // Where the faster engine library is, and the command that starts its helper.
    struct Accelerator {
        std::string library;
        std::vector<std::string> helper;
        int startSeconds = 60;
        int silenceSeconds = 180;
    };
    // enp-engine-cuda next to the executable, served by the executable itself; none on phones and the web.
    static std::optional<Accelerator> defaultAccelerator();

    LocalEngineService(); // with defaultAccelerator()
    explicit LocalEngineService(std::optional<Accelerator> accelerator);
    ~LocalEngineService() override;

    interfaces::EngineInfo info() override;
    void run(const interfaces::EngineRunSettings& settings, std::function<void(const std::string& stage)> progress,
             std::function<void(const interfaces::EngineRunResult& result)> done) override;
    void cancel() override;
    bool running() const override { return running_->load(); }
    void chat(const interfaces::LocalChatRequest& request, std::function<void(const std::string& stage)> stage,
              std::function<void(const std::string& text)> text,
              std::function<void(const interfaces::LocalChatResult& result)> done) override;
    bool chatting() const override { return chatting_->load(); }
    void unloadIfIdle(double idleSeconds) override;

    std::string modelsDirectory() override;
    uint64_t freeSpace(const std::string& path) override;
    uint64_t fileSize(const std::string& path) override;
    bool startDownload(const std::string& url, const std::string& path, bool allowMetered, std::string* error) override;
    interfaces::DownloadStatus downloadStatus() override;
    void cancelDownload() override;
    bool meteredNetwork() override;
    bool canImport() override;
    bool importFile(const std::string& path, std::string* error) override;
    void hashFile(const std::string& path, std::function<void(double fraction)> progress,
                  std::function<void(const std::string& sha256)> done) override;

private:
    struct ChatModel; // the agent's model, kept loaded between questions
    std::optional<interfaces::EngineInfo> info_;
    std::shared_ptr<ChatModel> chat_;
    std::shared_ptr<std::atomic<bool>> chatting_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> running_ = std::make_shared<std::atomic<bool>>(false);
    std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);
    // A question asked while a stopped one still winds down (a long prompt is stopped between
    // batches): started when that one ends. Main thread only.
    struct WaitingChat {
        interfaces::LocalChatRequest request;
        std::function<void(const std::string& stage)> stage;
        std::function<void(const std::string& text)> text;
        std::function<void(const interfaces::LocalChatResult& result)> done;
    };
    std::unique_ptr<WaitingChat> waiting_;
    void startWaiting();
    void dropWaiting();
    // Cleared on destruction: results of work still running are then dropped, not posted.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace enp::services
