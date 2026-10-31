// IAgentCliService over os::process. The command comes from $EASYNOTEPAD_AGENT_CLI, default
// "claude -p" (Claude Code in print mode: the prompt arrives on stdin, the answer on stdout).
#pragma once

#include "enp/interfaces/IAgentCliService.h"

#include <atomic>
#include <memory>

namespace enp::services {

class AgentCliService final : public interfaces::IAgentCliService {
public:
    AgentCliService();
    ~AgentCliService() override;

    std::string command() const override { return command_; }
    bool available() const override;
    void run(const std::string& prompt, const std::string& workingDirectory,
             std::function<void(bool ok, const std::string& output)> done) override;

private:
    std::string command_;
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
};

} // namespace enp::services
