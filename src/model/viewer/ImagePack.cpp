#include "enp/model/viewer/ImagePack.h"

#include <algorithm>
#include <cstring>

namespace enp::model::viewer {

std::string encodeQoi(const std::vector<std::uint8_t>& rgba, int w, int h)
{
    std::string out;
    out.reserve(14 + rgba.size() / 2 + 8);
    out += "qoif";
    auto be32 = [&](std::uint32_t v) {
        for (int s = 24; s >= 0; s -= 8)
            out += char((v >> s) & 0xFF);
    };
    be32(std::uint32_t(w));
    be32(std::uint32_t(h));
    out += char(4);
    out += char(0);
    std::uint8_t index[64][4] = {};
    std::uint8_t prev[4] = {0, 0, 0, 255};
    int run = 0;
    const std::size_t n = std::size_t(w) * std::size_t(h);
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* px = &rgba[i * 4];
        if (std::memcmp(px, prev, 4) == 0) {
            if (++run == 62 || i + 1 == n) {
                out += char(0xC0 | (run - 1));
                run = 0;
            }
            continue;
        }
        if (run > 0) {
            out += char(0xC0 | (run - 1));
            run = 0;
        }
        const int slot = (px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) % 64;
        if (std::memcmp(index[slot], px, 4) == 0) {
            out += char(slot);
        } else {
            std::memcpy(index[slot], px, 4);
            out += char(0xFF);
            out.append(reinterpret_cast<const char*>(px), 4);
        }
        std::memcpy(prev, px, 4);
    }
    out.append(7, '\0');
    out += char(1);
    return out;
}

namespace {

void be32(std::string& out, std::uint32_t v)
{
    for (int s = 24; s >= 0; s -= 8)
        out += char((v >> s) & 0xFF);
}

std::uint32_t readBe32(std::string_view b, std::size_t at)
{
    return (std::uint32_t(std::uint8_t(b[at])) << 24) | (std::uint32_t(std::uint8_t(b[at + 1])) << 16) |
           (std::uint32_t(std::uint8_t(b[at + 2])) << 8) | std::uint32_t(std::uint8_t(b[at + 3]));
}

} // namespace

std::string packRgba(const std::uint8_t* rgba, int w, int h)
{
    const std::size_t size = std::size_t(std::max(0, w)) * std::size_t(std::max(0, h)) * 4;
    std::string out;
    out.reserve(12 + size);
    out += "enpr";
    be32(out, std::uint32_t(w));
    be32(out, std::uint32_t(h));
    out.append(reinterpret_cast<const char*>(rgba), size);
    return out;
}

bool unpackRgba(std::string_view bytes, int& w, int& h, std::string_view& pixels)
{
    if (bytes.size() < 12 || bytes.substr(0, 4) != "enpr")
        return false;
    const std::uint32_t pw = readBe32(bytes, 4), ph = readBe32(bytes, 8);
    if (pw == 0 || ph == 0 || pw > (1u << 15) || ph > (1u << 15) || std::uint64_t(pw) * ph * 4 != bytes.size() - 12)
        return false;
    w = int(pw);
    h = int(ph);
    pixels = bytes.substr(12);
    return true;
}

std::string packMasked(std::string_view image, std::string_view mask, int maskWidth, int maskHeight, bool raw)
{
    std::string out;
    out.reserve(4 + 4 + image.size() + 13 + mask.size());
    out += "enpm";
    be32(out, std::uint32_t(image.size()));
    out.append(image);
    be32(out, std::uint32_t(maskWidth));
    be32(out, std::uint32_t(maskHeight));
    out += char(raw ? 0 : 1);
    be32(out, std::uint32_t(mask.size()));
    out.append(mask);
    return out;
}

bool unpackMasked(std::string_view bytes, MaskedImage& out)
{
    if (bytes.size() < 8 || bytes.substr(0, 4) != "enpm")
        return false;
    const std::size_t imageSize = readBe32(bytes, 4);
    if (imageSize > bytes.size() - 8 || bytes.size() - 8 - imageSize < 13)
        return false;
    std::size_t at = 8 + imageSize;
    out.image = bytes.substr(8, imageSize);
    out.maskWidth = int(readBe32(bytes, at));
    out.maskHeight = int(readBe32(bytes, at + 4));
    out.raw = bytes[at + 8] == 0;
    const std::size_t maskSize = readBe32(bytes, at + 9);
    at += 13;
    if (maskSize > bytes.size() - at || out.maskWidth <= 0 || out.maskHeight <= 0 || out.maskWidth > (1 << 14) || out.maskHeight > (1 << 14))
        return false;
    out.mask = bytes.substr(at, maskSize);
    return !out.raw || out.mask.size() >= std::size_t(out.maskWidth) * std::size_t(out.maskHeight);
}

} // namespace enp::model::viewer
