// TIFF pictures, decoded here (no libtiff): the first page, in strips or tiles, uncompressed,
// PackBits, LZW, Deflate or CCITT fax (Modified Huffman, Group 3 one- and two-dimensional,
// Group 4); bilevel and grey of 1 to 16 bits, palette, RGB and RGBA, CMYK; the horizontal predictor.
// Not yet: JPEG inside TIFF, floating-point samples, YCbCr, BigTIFF (the note says which).
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace enp::services::tiff {

// RGBA, 4 bytes a pixel, rows top to bottom. False with a note saying why.
bool decode(std::string_view bytes, std::vector<unsigned char>& rgba, int& width, int& height, std::string& note);

} // namespace enp::services::tiff
