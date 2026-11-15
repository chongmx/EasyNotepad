// Draws EasyNotepad's icons from its logo: Font Awesome's solid "pen-to-square" (U+F044, the
// glyph the title bar shows) in the dark theme's accent blue. Writes resources/icons/:
//   easynotepad-<size>.png        the logo on transparent (window icon, Linux, the web)
//   easynotepad-tile-512.png      the logo on the dark title-bar colour, a rounded tile (macOS's Dock)
//   windows/easynotepad.ico       the logo at the sizes Windows asks for (PNG entries)
//   android/mipmap-*/...          launcher icons: adaptive (foreground over a colour) and, before
//                                 Android 8, square and round tiles
// Build and run from the repository's root (no other dependencies):
//   g++ -std=c++17 -O2 -I3rdparty/imgui -I3rdparty/stb tools/icons/make_icons.cpp -o make_icons && ./make_icons
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

constexpr int kGlyph = 0xF044;                                // pen-to-square
constexpr unsigned char kBlue[3] = {59, 130, 246};            // Theme.cpp: the dark theme's accent
constexpr unsigned char kTile[3] = {24, 27, 31};              // Theme.cpp: the dark theme's title bar
const std::string kOut = "resources/icons/";

stbtt_fontinfo font;

struct Image {
    int size = 0;
    std::vector<unsigned char> rgba; // size * size * 4, straight alpha
};

// Paints `color` with coverage `a` (0..1) over pixel (x, y).
void blend(Image& img, int x, int y, const unsigned char color[3], float a)
{
    if (a <= 0.0f || x < 0 || y < 0 || x >= img.size || y >= img.size)
        return;
    unsigned char* p = &img.rgba[size_t(y * img.size + x) * 4];
    const float da = p[3] / 255.0f, oa = a + da * (1.0f - a);
    for (int c = 0; c < 3; ++c)
        p[c] = (unsigned char)std::lround((color[c] * a + p[c] * da * (1.0f - a)) / std::max(oa, 1e-6f));
    p[3] = (unsigned char)std::lround(oa * 255.0f);
}

// The glyph, its larger side `side` pixels, centred on (cx, cy).
void glyph(Image& img, float side, float cx, float cy)
{
    int x0, y0, x1, y1;
    stbtt_GetCodepointBox(&font, kGlyph, &x0, &y0, &x1, &y1);
    const float scale = side / float(std::max(x1 - x0, y1 - y0));
    // Where the glyph's box lands (font units: y up), to the subpixel.
    const float left = cx - (x1 - x0) * scale * 0.5f, top = cy - (y1 - y0) * scale * 0.5f;
    const float ox = left - x0 * scale, oy = top + y1 * scale; // the glyph's origin in the image
    const int ix = int(std::floor(ox)), iy = int(std::floor(oy));
    int bx0, by0, bx1, by1;
    stbtt_GetCodepointBitmapBoxSubpixel(&font, kGlyph, scale, scale, ox - ix, oy - iy, &bx0, &by0, &bx1, &by1);
    const int w = bx1 - bx0, h = by1 - by0;
    std::vector<unsigned char> cov(size_t(w * h));
    stbtt_MakeCodepointBitmapSubpixel(&font, cov.data(), w, h, w, scale, scale, ox - ix, oy - iy, kGlyph);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            blend(img, ix + bx0 + x, iy + by0 + y, kBlue, cov[size_t(y * w + x)] / 255.0f);
}

// A filled rounded square (radius r; a circle when r is half its side), anti-aliased by 4x4 samples.
void tile(Image& img, float inset, float r)
{
    const float lo = inset, hi = img.size - inset;
    for (int y = 0; y < img.size; ++y)
        for (int x = 0; x < img.size; ++x) {
            int inside = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = x + (sx + 0.5f) / 4.0f, py = y + (sy + 0.5f) / 4.0f;
                    if (px < lo || px > hi || py < lo || py > hi)
                        continue;
                    const float dx = std::max({lo + r - px, 0.0f, px - (hi - r)}), dy = std::max({lo + r - py, 0.0f, py - (hi - r)});
                    inside += dx * dx + dy * dy <= r * r ? 1 : 0;
                }
            blend(img, x, y, kTile, inside / 16.0f);
        }
}

Image blank(int size)
{
    Image img;
    img.size = size;
    img.rgba.assign(size_t(size * size * 4), 0);
    return img;
}

// The logo alone: the glyph filling the square but for a margin of about a sixteenth.
Image logo(int size)
{
    Image img = blank(size);
    const float margin = std::max(1.0f, size / 16.0f);
    glyph(img, size - 2.0f * margin, size * 0.5f, size * 0.5f);
    return img;
}

// The logo on a tile: inset by `inset` of the side, corners `radius` of the side (0.5: a circle).
Image tiled(int size, float inset, float radius)
{
    Image img = blank(size);
    tile(img, size * inset, size * radius);
    glyph(img, size * 0.52f, size * 0.5f, size * 0.5f);
    return img;
}

std::string png(const Image& img)
{
    std::string out;
    stbi_write_png_to_func([](void* ctx, void* data, int n) { static_cast<std::string*>(ctx)->append(static_cast<char*>(data), size_t(n)); },
                           &out, img.size, img.size, 4, img.rgba.data(), img.size * 4);
    return out;
}

bool save(const std::string& path, const std::string& bytes)
{
    std::ofstream f(kOut + path, std::ios::binary);
    f.write(bytes.data(), std::streamsize(bytes.size()));
    if (!f)
        std::fprintf(stderr, "cannot write %s%s\n", kOut.c_str(), path.c_str());
    return bool(f);
}

void le16(std::string& s, unsigned v) { s += char(v & 0xFF), s += char(v >> 8); }
void le32(std::string& s, unsigned v) { le16(s, v & 0xFFFF), le16(s, v >> 16); }

// An .ico of PNG images (Windows Vista and later read them; Windows 7 is the oldest supported).
std::string ico(const std::vector<int>& sizes)
{
    std::vector<std::string> images;
    for (int s : sizes)
        images.push_back(png(logo(s)));
    std::string out;
    le16(out, 0), le16(out, 1), le16(out, unsigned(sizes.size()));
    unsigned offset = 6 + 16 * unsigned(sizes.size());
    for (size_t i = 0; i < sizes.size(); ++i) {
        out += char(sizes[i] >= 256 ? 0 : sizes[i]), out += char(sizes[i] >= 256 ? 0 : sizes[i]);
        out += char(0), out += char(0);  // no palette, reserved
        le16(out, 1), le16(out, 32);     // planes, bits per pixel
        le32(out, unsigned(images[i].size())), le32(out, offset);
        offset += unsigned(images[i].size());
    }
    for (const std::string& img : images)
        out += img;
    return out;
}

} // namespace

int main()
{
    std::ifstream f("resources/fonts/fa-solid-900.ttf", std::ios::binary);
    const std::vector<unsigned char> ttf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (ttf.empty() || !stbtt_InitFont(&font, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0)) ||
        !stbtt_FindGlyphIndex(&font, kGlyph)) {
        std::fprintf(stderr, "run from the repository's root: resources/fonts/fa-solid-900.ttf with U+F044 is needed\n");
        return 1;
    }
    bool ok = true;
    for (int s : {16, 24, 32, 48, 64, 128, 256, 512})
        ok = save("easynotepad-" + std::to_string(s) + ".png", png(logo(s))) && ok;
    ok = save("easynotepad-tile-512.png", png(tiled(512, 0.1f, 0.18f))) && ok; // macOS: an icon's tile is inset, rounded
    ok = save("windows/easynotepad.ico", ico({16, 20, 24, 32, 40, 48, 64, 256})) && ok;
    // Android: 48 dp launcher icons (square and round tiles, before Android 8) and the adaptive
    // icon's 108 dp foreground (its glyph within the 66 dp circle every launcher shape keeps).
    const struct { const char* dir; float dp; } densities[] = {
        {"mdpi", 1.0f}, {"hdpi", 1.5f}, {"xhdpi", 2.0f}, {"xxhdpi", 3.0f}, {"xxxhdpi", 4.0f}};
    for (const auto& d : densities) {
        const std::string dir = std::string("android/mipmap-") + d.dir + "/";
        const int legacy = int(std::lround(48.0f * d.dp)), adaptive = int(std::lround(108.0f * d.dp));
        ok = save(dir + "ic_launcher.png", png(tiled(legacy, 1.0f / 24.0f, 0.2f))) && ok;
        ok = save(dir + "ic_launcher_round.png", png(tiled(legacy, 1.0f / 24.0f, 0.5f))) && ok;
        Image fg = blank(adaptive);
        glyph(fg, 42.0f * d.dp, adaptive * 0.5f, adaptive * 0.5f);
        ok = save(dir + "ic_launcher_foreground.png", png(fg)) && ok;
    }
    std::printf(ok ? "icons written to %s\n" : "some icons could not be written to %s\n", kOut.c_str());
    return ok ? 0 : 1;
}
