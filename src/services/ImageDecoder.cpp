#include "enp/services/ImageDecoder.h"

#include "enp/model/FileKind.h"
#include "enp/model/viewer/ImagePack.h"
#include "enp/os/Image.h"
#include "enp/os/Memory.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>

// stb_image allocates through the global operator new (the accounting sees it) under the
// "images" scope the callers set; these are the only raw allocation calls outside src/os, and
// they exist to feed a C library.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_NO_PIC
#define STBI_MAX_DIMENSIONS (1 << 14)
#define STBI_MALLOC(size) ::operator new(size, std::nothrow)
#define STBI_REALLOC_SIZED(ptr, oldSize, newSize) enp::services::stbRealloc(ptr, oldSize, newSize)
#define STBI_FREE(ptr) ::operator delete(ptr)
namespace enp::services {
inline void* stbRealloc(void* ptr, size_t oldSize, size_t newSize)
{
    void* fresh = ::operator new(newSize, std::nothrow);
    if (ptr && fresh)
        std::memcpy(fresh, ptr, std::min(oldSize, newSize));
    ::operator delete(ptr);
    return fresh;
}
} // namespace enp::services
#if defined(__GNUC__)
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wunused-function"
#   pragma GCC diagnostic ignored "-Wsign-compare"
#   pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#   if !defined(__clang__) && __GNUC__ >= 12
#       pragma GCC diagnostic ignored "-Wuse-after-free" // stb's realloc pattern, through the macro above
#   endif
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#   pragma GCC diagnostic pop
#endif

namespace enp::services {

namespace {

bool decodeBytes(std::string_view bytes, DecodedImage& out, std::string& note, int maxSide);

// A JPEG decoded by the platform at about `maxSide` across (os::image); false: decode it here.
// Only one the platforms all show alike: grey or colour (CMYK is treated differently by each:
// stb_image does Adobe's the way PDF readers do), and not turned by an EXIF orientation (a PDF's
// pictures are drawn as stored; Android's decoder always applies it).
bool decodeScaledJpeg(std::string_view bytes, int maxSide, DecodedImage& out)
{
    JpegInfo info;
    if (!readJpegInfo(bytes, info) || (info.components != 1 && info.components != 3) || info.orientation != 1)
        return false;
    out = DecodedImage{};
    if (!os::image::decodeJpeg(bytes, maxSide, out.rgba, out.width, out.height))
        return false;
    out.format = "JPEG";
    out.sourceWidth = info.width;
    out.sourceHeight = info.height;
    return true;
}

std::uint32_t be32(const unsigned char* p) { return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3]; }
std::uint32_t le32(const unsigned char* p) { return (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[1]) << 8) | p[0]; }
std::uint16_t le16(const unsigned char* p) { return std::uint16_t((std::uint16_t(p[1]) << 8) | p[0]); }

// QOI (the "Quite OK Image" format): https://qoiformat.org/qoi-specification.pdf
bool decodeQoi(const unsigned char* data, size_t size, std::vector<unsigned char>& rgba, int& w, int& h, std::string& note)
{
    if (size < 22 || std::memcmp(data, "qoif", 4) != 0) {
        note = "not a QOI file";
        return false;
    }
    const std::uint32_t width = be32(data + 4), height = be32(data + 8);
    if (width == 0 || height == 0 || width > (1u << 14) || height > (1u << 14)) {
        note = "QOI dimensions out of range";
        return false;
    }
    w = int(width);
    h = int(height);
    rgba.resize(size_t(width) * size_t(height) * 4);
    unsigned char index[64][4] = {};
    unsigned char px[4] = {0, 0, 0, 255};
    size_t p = 14;
    const size_t end = size - 8; // the 8-byte end marker
    size_t run = 0;
    for (size_t o = 0; o < rgba.size(); o += 4) {
        if (run > 0) {
            --run;
        } else if (p < end) {
            const unsigned char b1 = data[p++];
            if (b1 == 0xFE) { // RGB
                if (p + 3 > end) break;
                px[0] = data[p++]; px[1] = data[p++]; px[2] = data[p++];
            } else if (b1 == 0xFF) { // RGBA
                if (p + 4 > end) break;
                px[0] = data[p++]; px[1] = data[p++]; px[2] = data[p++]; px[3] = data[p++];
            } else if ((b1 & 0xC0) == 0x00) { // index
                std::memcpy(px, index[b1 & 0x3F], 4);
            } else if ((b1 & 0xC0) == 0x40) { // diff
                px[0] += ((b1 >> 4) & 3) - 2;
                px[1] += ((b1 >> 2) & 3) - 2;
                px[2] += (b1 & 3) - 2;
            } else if ((b1 & 0xC0) == 0x80) { // luma
                if (p + 1 > end) break;
                const unsigned char b2 = data[p++];
                const int vg = (b1 & 0x3F) - 32;
                px[0] += static_cast<unsigned char>(vg - 8 + ((b2 >> 4) & 0x0F));
                px[1] += static_cast<unsigned char>(vg);
                px[2] += static_cast<unsigned char>(vg - 8 + (b2 & 0x0F));
            } else { // run
                run = (b1 & 0x3F);
            }
            const unsigned hash = (px[0] * 3u + px[1] * 5u + px[2] * 7u + px[3] * 11u) % 64u;
            std::memcpy(index[hash], px, 4);
        }
        std::memcpy(rgba.data() + o, px, 4);
    }
    return true;
}

// ICO / CUR: a directory of images; the largest entry is shown. Entries are PNG files or BMP
// bitmaps without the file header (and with the height doubled for the AND mask).
bool decodeIco(const unsigned char* data, size_t size, std::vector<unsigned char>& rgba, int& w, int& h, std::string& note)
{
    if (size < 6 || data[0] != 0 || data[1] != 0 || (data[2] != 1 && data[2] != 2) || data[3] != 0) {
        note = "not an ICO file";
        return false;
    }
    const unsigned count = le16(data + 4);
    size_t best = 0;
    unsigned bestArea = 0;
    for (unsigned i = 0; i < count; ++i) {
        const size_t e = 6 + size_t(i) * 16;
        if (e + 16 > size)
            break;
        const unsigned ew = data[e] ? data[e] : 256, eh = data[e + 1] ? data[e + 1] : 256;
        if (ew * eh >= bestArea) {
            bestArea = ew * eh;
            best = e;
        }
    }
    if (bestArea == 0) {
        note = "empty ICO file";
        return false;
    }
    const std::uint32_t length = le32(data + best + 8), offset = le32(data + best + 12);
    if (offset >= size || length > size - offset) {
        note = "ICO entry out of range";
        return false;
    }
    const unsigned char* entry = data + offset;
    int channels = 0;
    if (length >= 8 && std::memcmp(entry, "\x89PNG", 4) == 0) {
        stbi_uc* pixels = stbi_load_from_memory(entry, int(length), &w, &h, &channels, 4);
        if (!pixels) {
            note = stbi_failure_reason() ? stbi_failure_reason() : "bad PNG in ICO";
            return false;
        }
        rgba.assign(pixels, pixels + size_t(w) * size_t(h) * 4);
        stbi_image_free(pixels);
        return true;
    }
    // A DIB: make it a BMP file (14-byte header, pixel data offset) with the real height.
    if (length < 40) {
        note = "bad ICO entry";
        return false;
    }
    std::vector<unsigned char> bmp(14 + length);
    const std::uint32_t infoSize = le32(entry);
    const std::uint16_t bpp = le16(entry + 14);
    std::uint32_t colours = le32(entry + 32);
    if (bpp <= 8 && colours == 0)
        colours = 1u << bpp;
    const std::uint32_t pixelOffset = 14 + infoSize + (bpp <= 8 ? colours * 4 : 0);
    bmp[0] = 'B';
    bmp[1] = 'M';
    const std::uint32_t fileSize = std::uint32_t(bmp.size());
    std::memcpy(bmp.data() + 2, &fileSize, 4);
    std::memcpy(bmp.data() + 10, &pixelOffset, 4);
    std::memcpy(bmp.data() + 14, entry, length);
    const std::int32_t height = std::int32_t(le32(entry + 8)) / 2; // XOR + AND masks: only the XOR half
    std::memcpy(bmp.data() + 14 + 8, &height, 4);
    stbi_uc* pixels = stbi_load_from_memory(bmp.data(), int(bmp.size()), &w, &h, &channels, 4);
    if (!pixels) {
        note = stbi_failure_reason() ? stbi_failure_reason() : "bad bitmap in ICO";
        return false;
    }
    rgba.assign(pixels, pixels + size_t(w) * size_t(h) * 4);
    stbi_image_free(pixels);
    return true;
}

void downscale(std::vector<unsigned char>& rgba, int& w, int& h, int maxSide)
{
    int factor = 1;
    while ((w + factor - 1) / factor > maxSide || (h + factor - 1) / factor > maxSide)
        ++factor;
    if (factor == 1)
        return;
    const int nw = (w + factor - 1) / factor, nh = (h + factor - 1) / factor;
    std::vector<unsigned char> out(size_t(nw) * size_t(nh) * 4);
    for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x) {
            unsigned sum[4] = {0, 0, 0, 0}, count = 0;
            for (int sy = y * factor; sy < std::min(h, (y + 1) * factor); ++sy)
                for (int sx = x * factor; sx < std::min(w, (x + 1) * factor); ++sx) {
                    const unsigned char* p = rgba.data() + (size_t(sy) * size_t(w) + size_t(sx)) * 4;
                    for (int k = 0; k < 4; ++k)
                        sum[k] += p[k];
                    ++count;
                }
            unsigned char* o = out.data() + (size_t(y) * size_t(nw) + size_t(x)) * 4;
            for (int k = 0; k < 4; ++k)
                o[k] = static_cast<unsigned char>(count ? sum[k] / count : 0);
        }
    rgba.swap(out);
    w = nw;
    h = nh;
}

// Whole blocks of `f` x `f` pixels averaged into one (integer sums, one pass over the source:
// the bulk of a large shrink); the blocks at the right and bottom edges count what they hold.
void boxReduce(std::vector<unsigned char>& rgba, int& w, int& h, int f)
{
    const int nw = (w + f - 1) / f, nh = (h + f - 1) / f;
    std::vector<unsigned char> out(size_t(nw) * size_t(nh) * 4);
    std::vector<std::uint32_t> sum(size_t(nw) * 4);
    for (int oy = 0; oy < nh; ++oy) {
        std::fill(sum.begin(), sum.end(), 0u);
        const int y0 = oy * f, y1 = std::min(h, y0 + f);
        for (int y = y0; y < y1; ++y) {
            const unsigned char* src = rgba.data() + size_t(y) * size_t(w) * 4;
            std::uint32_t* s = sum.data();
            int x = 0;
            for (int ox = 0; ox < nw; ++ox, s += 4) {
                const int x1 = std::min(w, x + f);
                std::uint32_t r = 0, g = 0, b = 0, a = 0;
                for (; x < x1; ++x, src += 4) {
                    r += src[0];
                    g += src[1];
                    b += src[2];
                    a += src[3];
                }
                s[0] += r;
                s[1] += g;
                s[2] += b;
                s[3] += a;
            }
        }
        unsigned char* dst = out.data() + size_t(oy) * size_t(nw) * 4;
        const std::uint32_t rows = std::uint32_t(y1 - y0);
        for (int ox = 0; ox < nw; ++ox) {
            const std::uint32_t count = rows * std::uint32_t(std::min(w, (ox + 1) * f) - ox * f);
            for (int k = 0; k < 4; ++k)
                dst[ox * 4 + k] = static_cast<unsigned char>((sum[size_t(ox) * 4 + size_t(k)] + count / 2) / count);
        }
    }
    rgba.swap(out);
    w = nw;
    h = nh;
}

// The image resized to nw x nh: each new pixel the average of the source pixels it covers
// (fractions at its edges counted by their share). Shrinking only. A large shrink first averages
// whole blocks (boxReduce), leaving less than twice the size to this exact pass.
void resample(std::vector<unsigned char>& rgba, int& w, int& h, int nw, int nh)
{
    const int f = std::min(w / std::max(1, nw), h / std::max(1, nh)); // the whole part of the shrink
    if (f >= 2)
        boxReduce(rgba, w, h, f);
    if (w == nw && h == nh)
        return;
    const float sx = float(w) / float(nw), sy = float(h) / float(nh);
    std::vector<unsigned char> out(size_t(nw) * size_t(nh) * 4);
    std::vector<float> row(size_t(nw) * 4);
    for (int y = 0; y < nh; ++y) {
        std::fill(row.begin(), row.end(), 0.0f);
        const float y0 = float(y) * sy, y1 = std::min(float(h), y0 + sy);
        for (int iy = int(y0); iy < h && float(iy) < y1; ++iy) {
            const float wy = std::min(y1, float(iy + 1)) - std::max(y0, float(iy));
            const unsigned char* src = rgba.data() + size_t(iy) * size_t(w) * 4;
            for (int x = 0; x < nw; ++x) {
                const float x0 = float(x) * sx, x1 = std::min(float(w), x0 + sx);
                float* o = &row[size_t(x) * 4];
                for (int ix = int(x0); ix < w && float(ix) < x1; ++ix) {
                    const float wgt = wy * (std::min(x1, float(ix + 1)) - std::max(x0, float(ix)));
                    const unsigned char* p = src + size_t(ix) * 4;
                    for (int k = 0; k < 4; ++k)
                        o[k] += wgt * float(p[k]);
                }
            }
        }
        const float area = sx * sy;
        unsigned char* dst = out.data() + size_t(y) * size_t(nw) * 4;
        for (size_t i = 0; i < row.size(); ++i)
            dst[i] = static_cast<unsigned char>(std::clamp(row[i] / area + 0.5f, 0.0f, 255.0f));
    }
    rgba.swap(out);
    w = nw;
    h = nh;
}

} // namespace

void fitImage(std::string& bytes, int maxSide)
{
    os::mem::Scope scope("images");
    model::viewer::MaskedImage masked;
    const bool packed = model::viewer::unpackMasked(bytes, masked);
    const std::string_view source = packed ? masked.image : std::string_view(bytes);
    maxSide = std::max(1, maxSide);
    {   // Raw pixels that already fit: as they are.
        int w = 0, h = 0;
        std::string_view pixels;
        if (!packed && model::viewer::unpackRgba(source, w, h, pixels) && std::max(w, h) <= maxSide)
            return;
    }
    DecodedImage image;
    std::string note;
    // A JPEG through the platform's decoder, decoded at about the size shown (os::image); others,
    // and a JPEG the platform cannot decode, here (at most 16k across: stb's limit; the shrink
    // below brings it down, not decodeImage's).
    if (!decodeScaledJpeg(source, maxSide, image) && !decodeBytes(source, image, note, 1 << 14))
        return;
    if (packed) { // the mask's samples, stretched over the picture when its size differs
        std::vector<unsigned char> alpha;
        int mw = masked.maskWidth, mh = masked.maskHeight;
        if (masked.raw) {
            alpha.assign(masked.mask.begin(), masked.mask.begin() + std::ptrdiff_t(size_t(mw) * size_t(mh)));
        } else {
            DecodedImage m;
            // A JPEG mask at about the picture's size (decoded scaled, like the picture).
            if (!decodeScaledJpeg(masked.mask, std::max(image.width, image.height), m) && !decodeBytes(masked.mask, m, note, 1 << 14))
                return;
            mw = m.width;
            mh = m.height;
            alpha.resize(size_t(mw) * size_t(mh));
            for (size_t i = 0; i < alpha.size(); ++i)
                alpha[i] = m.rgba[i * 4];
        }
        std::vector<size_t> column(size_t(image.width));
        for (int x = 0; x < image.width; ++x)
            column[size_t(x)] = size_t(std::min(mw - 1, int(std::int64_t(x) * mw / image.width)));
        for (int y = 0; y < image.height; ++y) {
            const unsigned char* row = alpha.data() + size_t(std::min(mh - 1, int(std::int64_t(y) * mh / image.height))) * size_t(mw);
            unsigned char* px = image.rgba.data() + size_t(y) * size_t(image.width) * 4 + 3;
            for (int x = 0; x < image.width; ++x, px += 4)
                *px = row[column[size_t(x)]];
        }
    }
    const int side = std::max(image.width, image.height);
    if (side > maxSide) {
        const float k = float(maxSide) / float(side);
        resample(image.rgba, image.width, image.height, std::max(1, int(float(image.width) * k + 0.5f)),
                 std::max(1, int(float(image.height) * k + 0.5f)));
    }
    // Raw pixels: the view only copies them into a texture (no decode on the main thread).
    bytes = model::viewer::packRgba(image.rgba, image.width, image.height);
}

bool readJpegInfo(std::string_view bytes, JpegInfo& out)
{
    out = JpegInfo{};
    const auto* b = reinterpret_cast<const unsigned char*>(bytes.data());
    const std::size_t n = bytes.size();
    if (n < 4 || b[0] != 0xFF || b[1] != 0xD8)
        return false;
    std::size_t at = 2;
    while (at + 4 <= n) {
        if (b[at] != 0xFF)
            return false;
        const unsigned marker = b[at + 1];
        if (marker == 0xFF) { // fill byte
            ++at;
            continue;
        }
        if (marker == 0xD8 || (marker >= 0xD0 && marker <= 0xD7) || marker == 0x01) { // no length
            at += 2;
            continue;
        }
        const std::size_t length = (std::size_t(b[at + 2]) << 8) | b[at + 3];
        if (length < 2 || at + 2 + length > n)
            return false;
        const unsigned char* seg = b + at + 4;
        const std::size_t size = length - 2;
        const bool sof = marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        if (sof && size >= 6) {
            out.height = (int(seg[1]) << 8) | seg[2];
            out.width = (int(seg[3]) << 8) | seg[4];
            out.components = seg[5];
            return out.width > 0 && out.height > 0 && out.components > 0; // the frame: all that matters is before it
        }
        if (marker == 0xEE && size >= 5 && std::memcmp(seg, "Adobe", 5) == 0)
            out.adobe = true;
        if (marker == 0xE1 && size >= 14 && std::memcmp(seg, "Exif\0\0", 6) == 0) { // TIFF: the orientation tag of IFD0
            const unsigned char* t = seg + 6;
            const std::size_t tn = size - 6;
            const bool le = t[0] == 'I' && t[1] == 'I';
            auto u16 = [&](std::size_t o) -> unsigned { return o + 2 > tn ? 0u : le ? unsigned(t[o] | (t[o + 1] << 8)) : unsigned((t[o] << 8) | t[o + 1]); };
            auto u32 = [&](std::size_t o) -> std::size_t {
                return o + 4 > tn ? std::size_t(-1) : le ? std::size_t(le32(t + o)) : std::size_t(be32(t + o));
            };
            const std::size_t ifd = u32(4);
            if (ifd != std::size_t(-1) && ifd + 2 <= tn) {
                const unsigned entries = u16(ifd);
                for (unsigned i = 0; i < entries && ifd + 2 + (i + 1) * 12 <= tn; ++i) {
                    const std::size_t e = ifd + 2 + i * 12;
                    if (u16(e) == 0x0112) {
                        out.orientation = int(u16(e + 8));
                        break;
                    }
                }
            }
        }
        if (marker == 0xDA) // the scan before any frame header: not a JPEG this reads
            return false;
        at += 2 + length;
    }
    return false;
}

std::optional<std::string> inflateRaw(std::string_view data, size_t expectedSize)
{
    int length = 0;
    char* out = stbi_zlib_decode_noheader_malloc(data.data(), int(std::min<size_t>(data.size(), size_t(INT32_MAX))), &length);
    if (!out)
        return std::nullopt;
    std::string result(out, size_t(std::max(0, length)));
    STBI_FREE(out);
    if (expectedSize && result.size() != expectedSize)
        return std::nullopt;
    return result;
}

bool decodeImage(const std::string& bytes, DecodedImage& out, std::string& note, int maxSide)
{
    return decodeBytes(bytes, out, note, maxSide);
}

namespace {

bool decodeBytes(std::string_view bytes, DecodedImage& out, std::string& note, int maxSide)
{
    os::mem::Scope scope("images");
    out = DecodedImage{};
    if (bytes.empty()) {
        note = "empty file";
        return false;
    }
    {   // Raw pixels (model::viewer::packRgba): copied.
        std::string_view pixels;
        if (model::viewer::unpackRgba(bytes, out.width, out.height, pixels)) {
            out.format = "RGBA";
            out.rgba.assign(pixels.begin(), pixels.end());
            out.sourceWidth = out.width;
            out.sourceHeight = out.height;
            downscale(out.rgba, out.width, out.height, std::max(16, maxSide));
            return true;
        }
        out.width = out.height = 0;
    }
    const model::FileType type = model::detectFileType({}, bytes);
    out.format = type.format;
    const unsigned char* data = reinterpret_cast<const unsigned char*>(bytes.data());
    bool ok = false;
    if (type.format == "QOI") {
        ok = decodeQoi(data, bytes.size(), out.rgba, out.width, out.height, note);
    } else if (type.format == "ICO" || type.format == "CUR") {
        ok = decodeIco(data, bytes.size(), out.rgba, out.width, out.height, note);
    } else if (type.format == "WebP" || type.format == "TIFF" || type.format == "SVG" || type.format == "AVIF" || type.format == "HEIC" ||
               type.format == "JPEG 2000") {
        note = type.format + " is not supported yet";
        return false;
    } else {
        int channels = 0;
        stbi_uc* pixels = stbi_load_from_memory(data, int(std::min<size_t>(bytes.size(), size_t(INT32_MAX))), &out.width, &out.height, &channels, 4);
        if (!pixels) {
            const char* reason = stbi_failure_reason();
            note = type.kind == model::FileKind::Image ? (reason ? reason : "cannot decode") : "not an image";
            return false;
        }
        out.rgba.assign(pixels, pixels + size_t(out.width) * size_t(out.height) * 4);
        stbi_image_free(pixels);
        ok = true;
        if (out.format.empty())
            out.format = "image";
    }
    if (!ok)
        return false;
    out.sourceWidth = out.width;
    out.sourceHeight = out.height;
    downscale(out.rgba, out.width, out.height, std::max(16, maxSide));
    return true;
}

} // namespace

} // namespace enp::services
