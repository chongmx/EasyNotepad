#include "enp/model/FileKind.h"

#include "enp/model/BinaryInfo.h"
#include "enp/model/viewer/Viewer.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>

namespace enp::model {

namespace {

struct Magic {
    const char* bytes;
    size_t length;
    size_t offset;
    FileKind kind;
    const char* format;
};

constexpr Magic kMagics[] = {
    {"\x89PNG\r\n\x1a\n", 8, 0, FileKind::Image, "PNG"},
    {"\xFF\xD8\xFF", 3, 0, FileKind::Image, "JPEG"},
    {"GIF87a", 6, 0, FileKind::Image, "GIF"},
    {"GIF89a", 6, 0, FileKind::Image, "GIF"},
    {"BM", 2, 0, FileKind::Image, "BMP"},
    {"WEBP", 4, 8, FileKind::Image, "WebP"},
    {"qoif", 4, 0, FileKind::Image, "QOI"},
    {"#?RADIANCE", 10, 0, FileKind::Image, "Radiance HDR"},
    {"#?RGBE", 6, 0, FileKind::Image, "Radiance HDR"},
    {"8BPS", 4, 0, FileKind::Image, "Photoshop PSD"},
    {"\x00\x00\x01\x00", 4, 0, FileKind::Image, "ICO"},
    {"\x00\x00\x02\x00", 4, 0, FileKind::Image, "CUR"},
    {"II*\x00", 4, 0, FileKind::Image, "TIFF"},
    {"MM\x00*", 4, 0, FileKind::Image, "TIFF"},
    {"\x00\x00\x00\x0cjP  ", 8, 0, FileKind::Image, "JPEG 2000"},
    {"ftypavif", 8, 4, FileKind::Image, "AVIF"},
    {"ftypheic", 8, 4, FileKind::Image, "HEIC"},
    {"\x7F" "ELF", 4, 0, FileKind::Binary, "ELF executable"},
    {"MZ", 2, 0, FileKind::Binary, "Windows executable"},
    {"\xCA\xFE\xBA\xBE", 4, 0, FileKind::Binary, "Mach-O or Java class"},
    {"\xCF\xFA\xED\xFE", 4, 0, FileKind::Binary, "Mach-O executable"},
    {"PK\x03\x04", 4, 0, FileKind::Binary, "ZIP archive"},
    {"PK\x05\x06", 4, 0, FileKind::Binary, "ZIP archive"},
    {"\x1F\x8B", 2, 0, FileKind::Binary, "gzip archive"},
    {"BZh", 3, 0, FileKind::Binary, "bzip2 archive"},
    {"\xFD" "7zXZ\x00", 6, 0, FileKind::Binary, "xz archive"},
    {"7z\xBC\xAF\x27\x1C", 6, 0, FileKind::Binary, "7-Zip archive"},
    {"Rar!\x1A\x07", 6, 0, FileKind::Binary, "RAR archive"},
    {"%PDF", 4, 0, FileKind::Binary, "PDF document"},
    {"SQLite format 3", 15, 0, FileKind::Binary, "SQLite database"},
    {"OggS", 4, 0, FileKind::Binary, "Ogg media"},
    {"fLaC", 4, 0, FileKind::Binary, "FLAC audio"},
    {"ID3", 3, 0, FileKind::Binary, "MP3 audio"},
    {"WAVE", 4, 8, FileKind::Binary, "WAV audio"},
    {"ftyp", 4, 4, FileKind::Binary, "MP4 media"},
    {"\x1A\x45\xDF\xA3", 4, 0, FileKind::Binary, "Matroska or WebM media"},
    {"wOFF", 4, 0, FileKind::Binary, "WOFF font"},
    {"wOF2", 4, 0, FileKind::Binary, "WOFF2 font"},
    {"OTTO", 4, 0, FileKind::Binary, "OpenType font"},
    {"\x00\x01\x00\x00", 4, 0, FileKind::Binary, "TrueType font"},
    {"GGUF", 4, 0, FileKind::Binary, "GGUF model"},
    {"\x93NUMPY", 6, 0, FileKind::Binary, "NumPy array"},
};

struct Extension {
    const char* ext;
    const char* format;
};
constexpr Extension kImageExtensions[] = {
    {"png", "PNG"},   {"jpg", "JPEG"},  {"jpeg", "JPEG"}, {"jpe", "JPEG"},   {"gif", "GIF"},   {"bmp", "BMP"},  {"dib", "BMP"},
    {"tga", "TGA"},   {"psd", "Photoshop PSD"}, {"ppm", "PPM"}, {"pgm", "PGM"},  {"pbm", "PBM"},  {"pnm", "PNM"},  {"pam", "PAM"},
    {"hdr", "Radiance HDR"}, {"qoi", "QOI"}, {"ico", "ICO"}, {"cur", "CUR"},  {"webp", "WebP"}, {"tif", "TIFF"}, {"tiff", "TIFF"},
    {"svg", "SVG"},   {"avif", "AVIF"}, {"heic", "HEIC"}, {"heif", "HEIC"},  {"jp2", "JPEG 2000"}, {"jxl", "JPEG XL"}, {"dds", "DDS"},
    {"exr", "OpenEXR"}, {"pcx", "PCX"},
};
constexpr const char* kBinaryExtensions[] = {"exe", "dll", "so", "dylib", "o", "obj", "a", "lib", "bin", "dat", "class", "jar", "pyc",
                                             "zip", "gz", "tgz", "xz", "bz2", "7z", "rar", "pdf", "db", "sqlite", "mp3", "mp4", "wav",
                                             "ogg", "flac", "mkv", "webm", "avi", "mov", "ttf", "otf", "woff", "woff2", "gguf", "safetensors",
                                             "npy", "npz", "pt", "pth", "onnx", "iso", "img", "dmg", "wasm"};

std::string extensionOf(std::string_view path)
{
    const size_t slash = path.find_last_of("/\\");
    const std::string_view name = slash == std::string_view::npos ? path : path.substr(slash + 1);
    const size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    std::string ext(name.substr(dot + 1));
    for (char& c : ext)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

bool looksLikeText(std::string_view bytes)
{
    // git's rule: a NUL in the first 8000 bytes means binary. Beyond that, a high share of control
    // characters (outside tab, newline, form feed, escape) does too.
    const size_t n = std::min<size_t>(bytes.size(), 8000);
    size_t control = 0;
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c == 0)
            return false;
        if (c < 0x20 && c != '\t' && c != '\n' && c != '\r' && c != '\f' && c != 0x1B && c != '\v')
            ++control;
    }
    return n == 0 || control * 10 < n; // under 10 % control characters
}

// A short magic number of printable letters only ("BM", "MZ", "ID3", "BZh", "OggS", ...) can begin
// a text file too ("BMW", "MZ-80"): it counts only when the file does not read as text. Real files
// of those kinds have NUL or control bytes in their first kilobytes (BMP's sizes, ...). Longer ones
// ("GIF89a", "#?RADIANCE", "SQLite format 3") are believed: a Radiance HDR file's header is text,
// and a small one can read as text throughout.
bool printableMagic(const Magic& m)
{
    if (m.length >= 6)
        return false;
    for (size_t i = 0; i < m.length; ++i) {
        const unsigned char c = static_cast<unsigned char>(m.bytes[i]);
        if (c < 0x20 || c >= 0x7F)
            return false;
    }
    return true;
}

// ICO and CUR begin 00 00 01 00 / 00 00 02 00, as other files can (an uncompressed TGA: 00 00 02
// 00): the icon directory after it must make sense too. At least one image, its entry's reserved
// byte 0, a size, and its data after the directory.
bool isIconDirectory(std::string_view b)
{
    if (b.size() < 22)
        return false;
    auto u16 = [&b](size_t i) { return std::uint32_t(static_cast<unsigned char>(b[i])) | std::uint32_t(static_cast<unsigned char>(b[i + 1])) << 8; };
    auto u32 = [&u16](size_t i) { return u16(i) | u16(i + 2) << 16; };
    const std::uint32_t count = u16(4);
    return count > 0 && b[9] == 0 && u32(14) > 0 && u32(18) >= 6 + 16 * count;
}

bool isPnm(std::string_view bytes)
{
    return bytes.size() >= 3 && bytes[0] == 'P' && bytes[1] >= '1' && bytes[1] <= '7' && std::isspace(static_cast<unsigned char>(bytes[2]));
}

} // namespace

bool isImageExtension(std::string_view extension)
{
    for (const Extension& e : kImageExtensions)
        if (extension == e.ext)
            return true;
    return false;
}

FileType detectFileType(std::string_view path, std::string_view bytes)
{
    FileType type;
    const bool text = looksLikeText(bytes);
    // Libraries and programs (ELF, PE, ar archives) get a precise name: the editor shows what they
    // contain. A COFF object has no magic number (only a plausible header): it is looked for last,
    // after every format that has one, so a picture is never taken for an object file. All of them
    // have NUL bytes in their headers: text that starts with "MZ" or "!<arch>" is text.
    bool coffObject = false;
    if (!text && bin::recognises(bytes)) {
        std::string format = bin::describe(bytes);
        if (format != "COFF object file") {
            type.kind = FileKind::Binary;
            type.format = std::move(format);
            return type;
        }
        coffObject = true;
    }
    // Documents the viewer reads: PDF by content, the ZIP-based Office formats by extension too.
    if (std::string format = viewer::viewerFormat(path, bytes); !format.empty()) {
        type.kind = FileKind::Document;
        type.format = std::move(format);
        return type;
    }
    for (const Magic& m : kMagics) {
        if (bytes.size() >= m.offset + m.length && std::memcmp(bytes.data() + m.offset, m.bytes, m.length) == 0) {
            if (std::strcmp(m.format, "WebP") == 0 && bytes.substr(0, 4) != "RIFF")
                continue;
            if (std::strcmp(m.format, "WAV audio") == 0 && bytes.substr(0, 4) != "RIFF")
                continue;
            if ((std::strcmp(m.format, "ICO") == 0 || std::strcmp(m.format, "CUR") == 0) && !isIconDirectory(bytes))
                continue; // an uncompressed TGA starts 00 00 02 00 too
            if (text && printableMagic(m) && std::strcmp(m.format, "PDF document") != 0)
                continue; // "BMW models" is a text file, not a bitmap
            type.kind = m.kind;
            type.format = m.format;
            return type;
        }
    }
    if (coffObject) {
        type.kind = FileKind::Binary;
        type.format = "COFF object file";
        return type;
    }
    const std::string ext = extensionOf(path);
    if (isPnm(bytes) && (ext.empty() || isImageExtension(ext) || ext == "txt")) {
        type.kind = FileKind::Image;
        type.format = "PNM";
        return type;
    }
    if (ext == "svg" && bytes.find("<svg") != std::string_view::npos) {
        type.format = "SVG"; // XML text: opened to edit (rasterizing SVG is not supported yet)
        return type;
    }
    if (!text) {
        type.kind = FileKind::Binary;
        for (const Extension& e : kImageExtensions)
            if (ext == e.ext) { // an image by name that no magic number matched (TGA, DDS, ...)
                type.kind = FileKind::Image;
                type.format = e.format;
                return type;
            }
        for (const char* b : kBinaryExtensions)
            if (ext == b) {
                type.format = ext + " file";
                return type;
            }
        type.format = "binary";
        return type;
    }
    if (ext == "tga" && !bytes.empty()) { // TGA has no magic number; a small header is still binary-ish
        type.kind = FileKind::Image;
        type.format = "TGA";
    }
    return type;
}

} // namespace enp::model
