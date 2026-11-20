// Base64 (RFC 4648, with padding): screenshots for automation clients, and pictures and sound sent
// to the engine library's helper process inside its JSON requests.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace enp::model {

std::string base64Encode(std::string_view data);

// The bytes, or nothing when `text` is not base64 (whitespace is not skipped).
std::optional<std::string> base64Decode(std::string_view text);

} // namespace enp::model
