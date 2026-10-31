// Contract for asking a command-line AI agent (e.g. `claude -p`) to do a one-shot task.
#pragma once

#include <functional>
#include <string>

namespace enp::interfaces {

class IAgentCliService {
public:
    virtual ~IAgentCliService() = default;
    // The configured command line, e.g. "claude -p".
    virtual std::string command() const = 0;
    // False when the command's executable cannot be found.
    virtual bool available() const = 0;
    // Runs the agent with `prompt` on stdin in `workingDirectory`; `done` is called on the main
    // thread with success and the agent's output (or the error).
    virtual void run(const std::string& prompt, const std::string& workingDirectory,
                     std::function<void(bool ok, const std::string& output)> done) = 0;
};

} // namespace enp::interfaces
