// Contract for running command-line tools (linters, analyzers, formatters) and getting their output.
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace enp::interfaces {

struct ProcessResult {
    bool started = false;
    int exitCode = -1;
    std::string output; // stdout and stderr
    std::string error;  // why it could not run
};

class IProcessService {
public:
    virtual ~IProcessService() = default;
    // Runs `commandLine` through the shell in `workingDirectory`; `done` is called on the main thread.
    virtual void run(const std::string& commandLine, const std::string& workingDirectory,
                     std::function<void(const ProcessResult&)> done) = 0;
    // A configuration value from the environment (e.g. EASYNOTEPAD_LINT), if set.
    virtual std::optional<std::string> setting(const std::string& name) const = 0;
};

} // namespace enp::interfaces
