// Downloading large files (models): resumable, one at a time, progress polled by the caller.
// Desktop runs curl (part of Windows 10 and later, macOS and Linux); Android hands the transfer to
// the system's DownloadManager, which keeps going while the app is in the background.
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

} // namespace enp::os::download
