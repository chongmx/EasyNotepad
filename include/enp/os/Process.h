// Running command-line tools: the basis of the CLI-based services (agent, analyzers, ...).
#pragma once

#include <string>

namespace enp::os::process {

struct Result {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string output; // stdout and stderr, interleaved
    std::string error;  // why it could not run
};

// Runs `commandLine` through the system shell (cmd.exe /c on Windows, /bin/sh -c elsewhere),
// writes `input` to its stdin, and collects its output. Blocks: call from a background thread.
Result run(const std::string& commandLine, const std::string& input, const std::string& workingDirectory,
           int timeoutSeconds);

// Full path of an executable found on PATH (".exe", ".cmd", ".bat" are tried on Windows), or empty.
std::string findExecutable(const std::string& name);

} // namespace enp::os::process
