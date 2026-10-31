#include "enp/services/ProcessService.h"

#include "enp/os/Dispatch.h"
#include "enp/os/Os.h"
#include "enp/os/Process.h"

namespace enp::services {

namespace {

constexpr int kTimeoutSeconds = 5 * 60;

} // namespace

ProcessService::~ProcessService()
{
    *alive_ = false; // results arriving later are dropped
}

void ProcessService::run(const std::string& commandLine, const std::string& workingDirectory,
                         std::function<void(const interfaces::ProcessResult&)> done)
{
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    os::dispatch::runAsync([alive, commandLine, workingDirectory, done = std::move(done)]() {
        const os::process::Result r = os::process::run(commandLine, {}, workingDirectory, kTimeoutSeconds);
        interfaces::ProcessResult result{r.started && !r.timedOut, r.exitCode, r.output, {},
                                         r.timedOut ? "did not finish within " + std::to_string(kTimeoutSeconds / 60) + " minutes" : r.error};
        os::dispatch::postToMain([alive, result, done]() {
            if (auto token = alive.lock(); token && *token)
                done(result);
        });
    });
}

void ProcessService::run(const interfaces::ProcessRequest& request, std::function<void(const interfaces::ProcessResult&)> done)
{
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    os::dispatch::runAsync([alive, request, done = std::move(done)]() {
        os::process::Command command;
        command.arguments = request.arguments;
        command.environment = request.environment;
        command.input = request.input;
        command.workingDirectory = request.workingDirectory;
        command.timeoutSeconds = kTimeoutSeconds;
        const os::process::Result r = os::process::run(command);
        interfaces::ProcessResult result{r.started && !r.timedOut, r.exitCode, r.output, r.errorOutput,
                                         r.timedOut ? "did not finish within " + std::to_string(kTimeoutSeconds / 60) + " minutes" : r.error};
        os::dispatch::postToMain([alive, result, done]() {
            if (auto token = alive.lock(); token && *token)
                done(result);
        });
    });
}

std::optional<std::string> ProcessService::setting(const std::string& name) const
{
    std::optional<std::string> value = os::getEnv(name);
    if (value && value->empty())
        return std::nullopt;
    return value;
}

} // namespace enp::services
