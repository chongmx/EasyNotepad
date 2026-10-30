// Private interface between the portable OS-layer code and the per-platform backends.
#pragma once

#include "enp/os/Os.h"

#include <string>
#include <string_view>
#include <vector>

namespace enp::os::detail {

// Implemented by every platform backend.
bool platformInit(const InitParams& params, std::vector<std::string>& outArguments);
void platformShutdown();
void writeLogSink(log::Level level, const char* message);

// Shared helpers.
std::string toLowerAscii(std::string s);
std::string shellQuote(const std::string& s); // single-quote for /bin/sh
// "scheme://..." or "mailto:"
bool isUrl(std::string_view s);
std::string percentDecode(std::string_view s);

} // namespace enp::os::detail
