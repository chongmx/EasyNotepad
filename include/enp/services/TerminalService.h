// ITerminalService on top of os::pty; output and exit are marshalled to the main thread.
#pragma once

#include "enp/interfaces/ITerminalService.h"

namespace enp::services {

class TerminalService final : public interfaces::ITerminalService {
public:
    TerminalService();

    bool isSupported() const override { return supported_; }
    const std::vector<interfaces::ShellProfile>& shells() const override { return shells_; }
    std::unique_ptr<interfaces::ITerminalSession> start(const interfaces::ShellProfile& shell,
                                                        const std::string& workingDirectory, int columns, int rows,
                                                        Callbacks callbacks, std::string* error) override;

private:
    bool supported_;
    std::vector<interfaces::ShellProfile> shells_;
};

} // namespace enp::services
