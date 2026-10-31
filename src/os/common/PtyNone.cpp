// Platforms without pseudo-terminals (Android NativeActivity, the web).
#include "enp/os/Pty.h"

namespace enp::os::pty {

bool supported()
{
    return false;
}

std::vector<Shell> availableShells()
{
    return {};
}

std::unique_ptr<Process> spawn(const Options&, OutputCallback, ExitCallback, std::string* error)
{
    if (error)
        *error = "Terminals are not available on this platform";
    return nullptr;
}

} // namespace enp::os::pty
