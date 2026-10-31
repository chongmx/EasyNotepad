// SHA-256 (FIPS 180-4), for the Hash tool.
#pragma once

#include <string>
#include <string_view>

namespace enp::model {

std::string sha256Hex(std::string_view data); // 64 lower-case hex digits

} // namespace enp::model
