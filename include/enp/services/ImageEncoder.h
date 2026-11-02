// Encodes RGBA8 pixels as a PNG file in memory (stb_image_write; screenshots of the app).
#pragma once

#include <string>

namespace enp::services {

// `rgba`: width * height * 4 bytes, top row first. Empty on failure.
std::string encodePng(const unsigned char* rgba, int width, int height);

} // namespace enp::services
