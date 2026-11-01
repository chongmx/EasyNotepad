// Running command-line tools: the basis of the CLI-based services (agent, analyzers, ...).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace enp::os::process {

struct Result {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string output;      // stdout (and stderr, for the shell form)
    std::string errorOutput; // stderr, for the argument-list form
    std::string error;       // why it could not run
    bool cancelled = false;  // stopped through `cancel`
};

// Set to true (from any thread) to stop a running program: the program and everything it started
// end within a fraction of a second.
using CancelFlag = std::shared_ptr<std::atomic<bool>>;

// Runs `commandLine` through the system shell (cmd.exe /c on Windows, /bin/sh -c elsewhere),
// writes `input` to its stdin, and collects its output. Blocks: call from a background thread.
Result run(const std::string& commandLine, const std::string& input, const std::string& workingDirectory,
           int timeoutSeconds, const CancelFlag& cancel = nullptr);

// A program and its exact arguments, run without a shell: nothing in the arguments is interpreted.
struct Command {
    std::vector<std::string> arguments; // [0] is the program, found on PATH unless it is a path
    std::vector<std::pair<std::string, std::string>> environment; // added to (or replacing) ours
    std::string input;                  // written to stdin
    std::string workingDirectory;
    int timeoutSeconds = 300;
    CancelFlag cancel;
    // Called on a reader thread with each piece of stdout as it arrives (it is still collected in
    // `output`): for programs that report progress while they run.
    std::function<void(const char* data, size_t size)> onOutput;
};
// Blocks: call from a background thread. stdout and stderr are kept apart.
Result run(const Command& command);

// Full path of an executable found on PATH (".exe", ".cmd", ".bat" are tried on Windows), or empty.
std::string findExecutable(const std::string& name);

} // namespace enp::os::process
