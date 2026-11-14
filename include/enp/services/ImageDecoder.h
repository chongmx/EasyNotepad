// Decodes image files into RGBA8 pixels: PNG, JPEG, GIF, BMP, TGA, PSD, PNM, Radiance HDR (tone
// mapped) through stb_image; QOI and ICO/CUR with small decoders of their own. Says why when it
// cannot (unknown or unsupported format such as WebP, TIFF, SVG, AVIF).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::services {

struct DecodedImage {
    std::vector<unsigned char> rgba; // width * height * 4
    int width = 0, height = 0;
    int sourceWidth = 0, sourceHeight = 0; // before any downscale
    std::string format;                    // "PNG", "QOI", ...
};

// Images wider or taller than `maxSide` are box-downscaled by an integer factor. On failure
// returns false with `note` ("not an image", "WebP is not supported yet", ...).
bool decodeImage(const std::string& bytes, DecodedImage& out, std::string& note, int maxSide = 2048);

// A picture a page shows at most `maxSide` pixels across, made ready for the view off the main
// thread: decoded (a packed picture with its soft mask, model::viewer::packMasked, put together),
// shrunk (area-averaged) when larger, and handed on as QOI. Bytes that are not a picture stay as
// they are (the view says why it cannot show them).
void fitImage(std::string& bytes, int maxSide);

// What a JPEG's header says, read without decoding it: its size, its colour components (1 grey,
// 3 colour, 4 CMYK / YCCK), whether it is Adobe's (APP14: CMYK stored inverted), and its EXIF
// orientation (1: as stored). False when it is not a JPEG or its header is cut short.
struct JpegInfo {
    int width = 0, height = 0, components = 0;
    int orientation = 1;
    bool adobe = false;
};
bool readJpegInfo(std::string_view bytes, JpegInfo& out);

// Raw deflate data (a zip entry stored with method 8) inflated; nothing when it is not valid.
std::optional<std::string> inflateRaw(std::string_view data, size_t expectedSize);

} // namespace enp::services
