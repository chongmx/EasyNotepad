// OS abstraction layer: umbrella header and process-level services.
#pragma once

#include "enp/os/Platform.h"
#include "enp/os/Dialog.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Fs.h"
#include "enp/os/Log.h"
#include "enp/os/Shell.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace enp::os {

struct InitParams {
    int argc = 0;
    char** argv = nullptr;
    // Platform handle required by some backends: `android_app*` on Android, unused elsewhere.
    void* nativeHandle = nullptr;
};

bool init(const InitParams& params);
void shutdown();

// Launch arguments as UTF-8 (argv[0] excluded). On the web the `?doc=` query parameter
// is exposed as the first argument.
const std::vector<std::string>& arguments();

std::optional<std::string> getEnv(const std::string& name);

// Monotonic clock in seconds.
double now();

// What the process holds right now, for tracking down leaks on a user's machine: a count that
// climbs while the app idles names the culprit (handles: processes, sockets, files; GDI/USER
// objects: windows, cursors, fonts; threads: work never joined). 0 where the platform cannot tell.
struct ProcessStats {
    std::uint64_t residentBytes = 0; // working set (Windows) / resident set (POSIX)
    std::uint64_t privateBytes = 0;  // committed private memory (Windows), anonymous memory (Linux)
    int handles = 0;                 // kernel handles (Windows) / open descriptors (Linux, Android)
    int gdiObjects = 0;              // Windows only
    int userObjects = 0;             // Windows only
    int threads = 0;
};
ProcessStats processStats();

// Gives memory the program has freed back to the operating system. Freed heap blocks otherwise
// stay with the process (the allocator keeps them for reuse), so closing a folder would not show
// in a task manager. Call it after releasing something large; on Windows it also trims the
// working set, so the pages show as released at once (they are paged back in on demand).
void trimMemory();

} // namespace enp::os
