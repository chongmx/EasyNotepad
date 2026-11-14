// Pictures decoded by the platform's own decoder, at about the size they are shown: a JPEG is
// decoded straight at 1/2, 1/4 or 1/8 of its size where the decoder can (only the coefficients
// that size needs), then scaled to fit. Android: AImageDecoder (Android 11 and later, the system's
// libjpeg-turbo, NEON); Windows: WIC; macOS: ImageIO. Elsewhere (Linux, the web, older Android)
// there is none and the caller decodes the picture itself (services::decodeImage, stb_image).
#pragma once

#include <string_view>
#include <vector>

namespace enp::os::image {

// `jpeg` (a whole JPEG file, grey or colour: the caller keeps CMYK and EXIF-turned ones) as RGBA8
// pixels, about `maxSide` pixels across: at most twice that where the decoder stops at a scale
// it decodes directly (the caller shrinks the rest), never larger than the picture itself. As
// stored: no EXIF turn. False when the platform has no decoder or it cannot decode this one.
// Safe on any thread; `rgba` is the only memory kept.
bool decodeJpeg(std::string_view jpeg, int maxSide, std::vector<unsigned char>& rgba, int& width, int& height);

} // namespace enp::os::image
