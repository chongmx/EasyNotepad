// Pictures as the page reader hands them on: QOI for pixels it decoded itself, and a picture with
// a soft mask it cannot apply (a JPEG's alpha, a mask of another size) packed with that mask for
// the reader service to put together when it decodes the page's pictures.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::viewer {

// RGBA8 pixels (`w` * `h` * 4) as a QOI file.
std::string encodeQoi(const std::vector<std::uint8_t>& rgba, int w, int h);

// RGBA8 pixels as they are, with their size: "enpr", width and height (big-endian 32-bit), then
// `w` * `h` * 4 bytes. For pixels handed from one part of the program to another (the page reader
// to the reader service to the view): no encoding, and unpacking is a copy.
std::string packRgba(const std::uint8_t* rgba, int w, int h);
inline std::string packRgba(const std::vector<std::uint8_t>& rgba, int w, int h) { return packRgba(rgba.data(), w, h); }
// The pixels of a packed picture, viewing `bytes`; false when it is not one.
bool unpackRgba(std::string_view bytes, int& w, int& h, std::string_view& pixels);

struct MaskedImage {
    std::string_view image; // an image file: JPEG, QOI, ...
    std::string_view mask;  // `raw`: maskWidth * maskHeight bytes of alpha; otherwise an image file (its first channel)
    int maskWidth = 0, maskHeight = 0;
    bool raw = true;
};

// "enpm", then the image and the mask, each with its length (big-endian 32-bit).
std::string packMasked(std::string_view image, std::string_view mask, int maskWidth, int maskHeight, bool raw);
// The parts of a packed picture, viewing `bytes`; false when it is not one.
bool unpackMasked(std::string_view bytes, MaskedImage& out);

} // namespace enp::model::viewer
