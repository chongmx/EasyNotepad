// Running command-line tools: the basis of the CLI-based services (agent, analyzers, ...).
#pragma once

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
};

// Runs `commandLine` through the system shell (cmd.exe /c on Windows, /bin/sh -c elsewhere),
// writes `input` to its stdin, and collects its output. Blocks: call from a background thread.
Result run(const std::string& commandLine, const std::string& input, const std::string& workingDirectory,
           int timeoutSeconds);

// A program and its exact arguments, run without a shell: nothing in the arguments is interpreted.
struct Command {
    std::vector<std::string> arguments; // [0] is the program, found on PATH unless it is a path
    std::vector<std::pair<std::string, std::string>> environment; // added to (or replacing) ours
    std::string input;                  // written to stdin
    std::string workingDirectory;
    int timeoutSeconds = 300;
};
// Blocks: call from a background thread. stdout and stderr are kept apart.
Result run(const Command& command);

// Full path of an executable found on PATH (".exe", ".cmd", ".bat" are tried on Windows), or empty.
std::string findExecutable(const std::string& name);

} // namespace enp::os::process
