// Pseudo-terminal processes: a child process (normally a shell) attached to a terminal device, so
// it behaves as if it ran in a real terminal (colours, line editing, full-screen programs).
// Windows: ConPTY (Windows 10 1809+). Linux/macOS: forkpty(). Android/web: unsupported.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace enp::os::pty {

struct Shell {
    std::string name;    // "PowerShell", "bash"
    std::string command; // executable path or command line
};

struct Options {
    // Program to run; empty: the first of availableShells(). On Windows this may be a full command
    // line (Shell::command is one); on POSIX it is the executable path.
    std::string command;
    std::vector<std::string> arguments;
    std::string workingDirectory; // empty: current directory
    int columns = 80;
    int rows = 24;
};

// Both callbacks run on a background thread.
using OutputCallback = std::function<void(std::string data)>;
using ExitCallback = std::function<void(int exitCode)>;

class Process {
public:
    virtual ~Process() = default; // terminates the process if still running
    virtual bool write(std::string_view data) = 0;
    virtual void resize(int columns, int rows) = 0;
    virtual void terminate() = 0;
};

bool supported();
// Shells installed on this machine, preferred first.
std::vector<Shell> availableShells();

// Starts `options.command` on a new pseudo-terminal. Returns null and sets `error` on failure.
// `onExit` is called once, after the last `onOutput`.
std::unique_ptr<Process> spawn(const Options& options, OutputCallback onOutput, ExitCallback onExit,
                               std::string* error = nullptr);

} // namespace enp::os::pty
