// Downloading large files (models): resumable, one at a time, progress polled by the caller.
// Desktop runs curl (part of Windows 10 and later, macOS and Linux); Android downloads in the app
// (HTTP with Range resume and retries), and can also copy in a file picked in the system picker.
#pragma once

#include <cstdint>
#include <string>

namespace enp::os::download {

struct Status {
    enum class State { Idle, Running, Done, Failed };
    State state = State::Idle;
    uint64_t received = 0;
    uint64_t total = 0; // 0 while unknown
    std::string error;
};

// Where large downloaded files belong on this device (created if missing). Android: the app's
// external files folder, the only place DownloadManager may write for the app.
std::string directory();

// Starts fetching `url` into `path`; a partial file there is resumed (desktop). On Android the
// transfer waits for Wi-Fi unless `allowMetered`.
bool start(const std::string& url, const std::string& path, bool allowMetered, std::string* error);
Status status();
void cancel();
// True on a metered network (mobile data, a hotspot): large downloads ask first. Desktop: false.
bool meteredNetwork();
// Android: the system file picker, then the chosen file is copied to `path` (followed by status()
// like a download). Elsewhere a path can simply be typed, so there is no import.
bool canImport();
bool importFile(const std::string& path, std::string* error);

} // namespace enp::os::download
