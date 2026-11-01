// ILocalEngineService over llama.cpp (when the build has it: ENP_LOCAL_ENGINE), the OS downloader
// and a streaming SHA-256. Runs in the app's own process for now; plan 23 moves it into a helper
// process with the safety net.
#pragma once

#include "enp/interfaces/ILocalEngineService.h"

#include <atomic>
#include <memory>
#include <optional>

namespace enp::services {

class LocalEngineService final : public interfaces::ILocalEngineService {
public:
    LocalEngineService();
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
    // Cleared on destruction: results of work still running are then dropped, not posted.
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace enp::services
