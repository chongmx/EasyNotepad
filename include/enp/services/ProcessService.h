// IProcessService over os::process: commands run on a background thread, results on the main thread.
#pragma once

#include "enp/interfaces/IProcessService.h"

#include <atomic>
#include <memory>

namespace enp::services {

class ProcessService : public interfaces::IProcessService {
public:
    ~ProcessService() override;
    void run(const std::string& commandLine, const std::string& workingDirectory,
             std::function<void(const interfaces::ProcessResult&)> done) override;
    std::optional<std::string> setting(const std::string& name) const override;

private:
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace enp::services
