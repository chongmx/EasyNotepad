// Ciphers and digests for reading encrypted files (PDF's standard security handler): MD5, RC4,
// AES in CBC mode (128, 192 and 256-bit keys), and SHA-256/384/512. For reading what others
// protected: nothing EasyNotepad writes is encrypted with these, and MD5 and RC4 are long broken
// (PDF still uses them). Pure functions over byte strings; nothing is kept between calls.
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace enp::model::crypto {

std::string md5(std::string_view data);    // 16 bytes
std::string sha256(std::string_view data); // 32 bytes
std::string sha384(std::string_view data); // 48 bytes
std::string sha512(std::string_view data); // 64 bytes

// RC4 with `key` (1 to 256 bytes) over `data`: encrypting and decrypting are the same.
std::string rc4(std::string_view key, std::string_view data);

// AES in CBC mode, without padding: `key` 16, 24 or 32 bytes, `iv` 16 bytes, `data` a whole
// number of 16-byte blocks. Nothing (nullopt) when a size is wrong.
std::optional<std::string> aesCbcEncrypt(std::string_view key, std::string_view iv, std::string_view data);
std::optional<std::string> aesCbcDecrypt(std::string_view key, std::string_view iv, std::string_view data);

} // namespace enp::model::crypto
