// DEFLATE decompression for the document readers: raw (ZIP entries) or zlib-wrapped (PDF
// FlateDecode, PNG). Bounded: a stream that would inflate past `limit` bytes fails instead of
// filling memory (zip bombs).
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace enp::model::package {

// `expected`: the size the container states, as a first guess for the output (0: unknown).
// nullopt when the data is broken or the output would exceed `limit`.
std::optional<std::string> inflate(std::string_view data, bool zlibHeader, std::size_t limit, std::size_t expected = 0);

} // namespace enp::model::package
