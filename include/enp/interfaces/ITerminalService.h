// Contract for running shells on pseudo-terminals.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace enp::interfaces {

struct ShellProfile {
    std::string name;    // shown in the UI: "PowerShell", "bash"
    std::string command; // what the service runs
};

class ITerminalSession {
public:
    virtual ~ITerminalSession() = default; // ends the process; no callbacks arrive afterwards
    virtual void write(std::string_view bytes) = 0;
    virtual void resize(int columns, int rows) = 0;
    virtual void terminate() = 0;
};

class ITerminalService {
public:
    struct Callbacks {
        std::function<void(std::string_view bytes)> output; // main thread
        std::function<void(int exitCode)> exited;           // main thread, once, after the last output
    };

    virtual ~ITerminalService() = default;
    virtual bool isSupported() const = 0;
    // Installed shells, preferred first.
    virtual const std::vector<ShellProfile>& shells() const = 0;
    // Returns null and sets `error` on failure. An empty working directory means the app's.
    virtual std::unique_ptr<ITerminalSession> start(const ShellProfile& shell, const std::string& workingDirectory,
                                                    int columns, int rows, Callbacks callbacks, std::string* error) = 0;
};

} // namespace enp::interfaces
