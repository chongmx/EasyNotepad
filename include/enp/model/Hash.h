// SHA-256 (FIPS 180-4), for the Hash tool and for checking downloads.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace enp::model {

// Incremental: feed any number of pieces, then read the digest once (large files are hashed in
// chunks, never held whole).
class Sha256 {
public:
    void update(const void* data, size_t size);
    std::string hexDigest(); // 64 lower-case hex digits; the object is finished afterwards

private:
    void block(const uint8_t* p);
    uint32_t h_[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint8_t buffer_[64] = {};
    size_t buffered_ = 0;
    uint64_t length_ = 0;
};

std::string sha256Hex(std::string_view data); // 64 lower-case hex digits

} // namespace enp::model
