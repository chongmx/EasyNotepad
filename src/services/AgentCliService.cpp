#include "enp/services/AgentCliService.h"

#include "enp/os/Os.h"
#include "enp/os/Process.h"

#include <chrono>

namespace enp::services {

namespace {

constexpr int kTimeoutSeconds = 15 * 60; // a whole-project dissection can take a while

} // namespace

AgentCliService::AgentCliService()
{
    const std::optional<std::string> configured = os::getEnv("EASYNOTEPAD_AGENT_CLI");
    command_ = configured && !configured->empty() ? *configured : "claude -p";
}

AgentCliService::~AgentCliService()
{
    *alive_ = false; // results arriving later are dropped
}

bool AgentCliService::available() const
{
    const double now = double(std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
    if (now - checkedAt_ < 30.0)
        return available_;
    checkedAt_ = now;
    std::string executable = command_.substr(0, command_.find(' '));
    if (executable.size() > 1 && executable.front() == '"')
        executable = command_.substr(1, command_.find('"', 1) - 1);
    available_ = !os::process::findExecutable(executable).empty() || os::fs::exists(executable);
    return available_;
}

void AgentCliService::run(const std::string& prompt, const std::string& workingDirectory,
                          std::function<void(bool ok, const std::string& output)> done)
{
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    const std::string command = command_;
    os::dispatch::runAsync([alive, command, prompt, workingDirectory, done = std::move(done)]() {
        const os::process::Result result = os::process::run(command, prompt, workingDirectory, kTimeoutSeconds);
        const bool ok = result.started && !result.timedOut && result.exitCode == 0;
        std::string text = result.output;
        if (!result.started)
            text = result.error;
        else if (result.timedOut)
            text = "the agent did not finish within " + std::to_string(kTimeoutSeconds / 60) + " minutes";
        else if (result.exitCode != 0)
            text = "the agent exited with code " + std::to_string(result.exitCode) + ": " + result.output.substr(0, 400);
        os::dispatch::postToMain([alive, ok, text, done]() {
            if (auto token = alive.lock(); token && *token)
                done(ok, text);
        });
    });
}

} // namespace enp::services
