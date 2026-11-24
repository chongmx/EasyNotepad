#include "enp/model/Base64.h"

#include <cstdint>

namespace enp::model {

namespace {
const char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int digitValue(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '+')
        return 62;
    if (c == '/')
        return 63;
    return -1;
}
} // namespace

std::string base64Encode(std::string_view data)
{
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t n = std::uint32_t(static_cast<unsigned char>(data[i])) << 16 |
                                (i + 1 < data.size() ? std::uint32_t(static_cast<unsigned char>(data[i + 1])) << 8 : 0u) |
                                (i + 2 < data.size() ? std::uint32_t(static_cast<unsigned char>(data[i + 2])) : 0u);
        out += kDigits[n >> 18 & 63];
        out += kDigits[n >> 12 & 63];
        out += i + 1 < data.size() ? kDigits[n >> 6 & 63] : '=';
        out += i + 2 < data.size() ? kDigits[n & 63] : '=';
    }
    return out;
}

std::optional<std::string> base64Decode(std::string_view text)
{
    if (text.size() % 4 != 0)
        return std::nullopt;
    std::string out;
    out.reserve(text.size() / 4 * 3);
    for (size_t i = 0; i < text.size(); i += 4) {
        const bool last = i + 4 == text.size();
        const int pad = last ? (text[i + 3] == '=') + (text[i + 2] == '=' && text[i + 3] == '=') : 0;
        int v[4];
        for (int k = 0; k < 4 - pad; ++k)
            if ((v[k] = digitValue(text[i + k])) < 0)
                return std::nullopt;
        for (int k = 4 - pad; k < 4; ++k)
            v[k] = 0;
        const std::uint32_t n = std::uint32_t(v[0]) << 18 | std::uint32_t(v[1]) << 12 | std::uint32_t(v[2]) << 6 | std::uint32_t(v[3]);
        out += char(n >> 16 & 255);
        if (pad < 2)
            out += char(n >> 8 & 255);
        if (pad < 1)
            out += char(n & 255);
    }
    return out;
}

} // namespace enp::model
