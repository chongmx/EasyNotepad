// IAgentCliService over os::process. The command comes from $EASYNOTEPAD_AGENT_CLI, default
// "claude -p" (Claude Code in print mode: the prompt arrives on stdin, the answer on stdout).
// With Claude Code, conversations stream (stream-json) and continue with --resume.
#pragma once

#include "enp/interfaces/IAgentCliService.h"
#include "enp/os/Process.h"

#include <atomic>
#include <memory>
#include <vector>

namespace enp::services {

class AgentCliService final : public interfaces::IAgentCliService {
public:
    AgentCliService();
    ~AgentCliService() override;

    std::string command() const override;
    bool available() const override;
    void run(const std::string& prompt, const std::string& workingDirectory,
             std::function<void(bool ok, const std::string& output)> done) override;
    void cancel() override;
    void setCommand(const std::string& command) override;
    void recheck() override;
    bool checking() const override { return checking_; }
    bool isClaude() const override { return claudeCommand_; }
    std::string executable() const override { return executable_; }
    bool canStream() const override { return claudeCommand_ && available(); }
    void stream(const interfaces::AgentStreamRequest& request, std::function<void(const std::string& line)> onLine,
                std::function<void(bool ok, const std::string& error)> done) override;

    // Where Claude Code is: PATH, its installer's folders, or the copy inside the VS Code extension.
    static std::string findClaude();

private:
    std::string command_;
    bool claudeCommand_ = false; // the command is Claude Code (so it can stream)
    // Searching PATH is slow when it holds network folders: it runs in the background, and the
    // answer is kept for a while.
    mutable bool available_ = true;
    mutable bool checking_ = false;
    mutable int checkGeneration_ = 0; // a check started before the command changed is ignored
    mutable double checkedAt_ = -1.0e9;
    mutable std::string executable_; // the program's full path, once found (Claude Code runs from it)
    std::vector<os::process::CancelFlag> running_; // main thread only
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);
    os::process::CancelFlag track();
    void untrack(const os::process::CancelFlag& flag);
};

} // namespace enp::services
