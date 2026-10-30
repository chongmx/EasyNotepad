// OS abstraction layer: umbrella header and process-level services.
#pragma once

#include "enp/os/Platform.h"
#include "enp/os/Dialog.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Fs.h"
#include "enp/os/Log.h"
#include "enp/os/Shell.h"

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

} // namespace enp::os
