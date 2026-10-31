// Contract for running command-line tools (linters, analyzers, formatters) and getting their output.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace enp::interfaces {

struct ProcessResult {
    bool started = false;
    int exitCode = -1;
    std::string output;      // stdout (and stderr, for a shell command line)
    std::string errorOutput; // stderr, for a ProcessRequest
    std::string error;       // why it could not run
};

// A program and its exact arguments, run without a shell (see os::process::Command).
struct ProcessRequest {
    std::vector<std::string> arguments;
    std::vector<std::pair<std::string, std::string>> environment;
    std::string input;
    std::string workingDirectory;
};

class IProcessService {
public:
    virtual ~IProcessService() = default;
    // Runs `commandLine` through the shell in `workingDirectory`; `done` is called on the main thread.
    virtual void run(const std::string& commandLine, const std::string& workingDirectory,
                     std::function<void(const ProcessResult&)> done) = 0;
    // Runs a program with exact arguments (no shell); `done` is called on the main thread.
    virtual void run(const ProcessRequest& request, std::function<void(const ProcessResult&)> done)
    {
        ProcessResult result;
        result.error = "not supported";
        if (done)
            done(result);
        (void)request;
    }
    // A configuration value from the environment (e.g. EASYNOTEPAD_LINT), if set.
    virtual std::optional<std::string> setting(const std::string& name) const = 0;
};

} // namespace enp::interfaces
