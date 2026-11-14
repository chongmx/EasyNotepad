// JPEG pictures through AImageDecoder (Android 11, API 30, and later): the system's decoder
// (libjpeg-turbo with NEON) decodes straight at the target size, sampling the JPEG's coefficients
// down first. The app runs from Android 7, so the functions are looked up when first needed, in
// libjnigraphics; without them (older Android) the caller decodes the picture itself.
#include "enp/os/Image.h"

#include <dlfcn.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <type_traits>

namespace enp::os::image {

namespace {

// <android/imagedecoder.h> declares these from API 30 only; the app's minimum is 24, so the
// opaque types and the two constants used are declared here and the functions found at run time.
struct Decoder;
struct HeaderInfo;
constexpr int kSuccess = 0;          // ANDROID_IMAGE_DECODER_SUCCESS
constexpr std::int32_t kRgba8888 = 1; // ANDROID_BITMAP_FORMAT_RGBA_8888

struct Api {
    int (*createFromBuffer)(const void*, std::size_t, Decoder**) = nullptr;
    void (*destroy)(Decoder*) = nullptr;
    const HeaderInfo* (*headerInfo)(const Decoder*) = nullptr;
    std::int32_t (*width)(const HeaderInfo*) = nullptr;
    std::int32_t (*height)(const HeaderInfo*) = nullptr;
    int (*setFormat)(Decoder*, std::int32_t) = nullptr;
    int (*setTargetSize)(Decoder*, std::int32_t, std::int32_t) = nullptr;
    int (*sampledSize)(const Decoder*, int, std::int32_t*, std::int32_t*) = nullptr;
    std::size_t (*minimumStride)(Decoder*) = nullptr;
    int (*decodeImage)(Decoder*, void*, std::size_t, std::size_t) = nullptr;
    bool ok = false;
};

const Api& api()
{
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        // Kept loaded for the process's life (the system library is mapped already: this only
        // takes a reference to it).
        void* lib = dlopen("libjnigraphics.so", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return;
        auto find = [lib](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            return fn != nullptr;
        };
        a.ok = find(a.createFromBuffer, "AImageDecoder_createFromBuffer") && find(a.destroy, "AImageDecoder_delete") &&
               find(a.headerInfo, "AImageDecoder_getHeaderInfo") && find(a.width, "AImageDecoderHeaderInfo_getWidth") &&
               find(a.height, "AImageDecoderHeaderInfo_getHeight") && find(a.setFormat, "AImageDecoder_setAndroidBitmapFormat") &&
               find(a.setTargetSize, "AImageDecoder_setTargetSize") && find(a.sampledSize, "AImageDecoder_computeSampledSize") &&
               find(a.minimumStride, "AImageDecoder_getMinimumStride") &&
               find(a.decodeImage, "AImageDecoder_decodeImage");
    });
    return a;
}

// The decoder, deleted when it goes.
class Handle {
public:
    explicit Handle(const Api& a) : api_(a) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle()
    {
        if (d_)
            api_.destroy(d_);
    }
    Decoder** put() { return &d_; }
    Decoder* get() const { return d_; }

private:
    const Api& api_;
    Decoder* d_ = nullptr;
};

} // namespace

bool decodeJpeg(std::string_view jpeg, int maxSide, std::vector<unsigned char>& rgba, int& width, int& height)
{
    const Api& a = api();
    if (!a.ok || jpeg.empty() || maxSide < 1)
        return false;
    Handle decoder(a);
    if (a.createFromBuffer(jpeg.data(), jpeg.size(), decoder.put()) != kSuccess || !decoder.get())
        return false;
    const HeaderInfo* info = a.headerInfo(decoder.get());
    const std::int32_t w = info ? a.width(info) : 0, h = info ? a.height(info) : 0;
    if (w <= 0 || h <= 0 || a.setFormat(decoder.get(), kRgba8888) != kSuccess)
        return false;
    // The largest sampling (1/2, 1/4, 1/8: the JPEG decoded at that scale, only the coefficients it
    // needs) whose size is still at least `maxSide` across, asked for exactly: no rescaling after it
    // (the caller shrinks the rest with its area average, as on every platform).
    std::int32_t tw = w, th = h;
    for (int sample = 8; sample >= 2 && std::max(w, h) > maxSide; sample /= 2) {
        std::int32_t sw = 0, sh = 0;
        if (a.sampledSize(decoder.get(), sample, &sw, &sh) == kSuccess && sw > 0 && sh > 0 && std::max(sw, sh) >= maxSide) {
            if (a.setTargetSize(decoder.get(), sw, sh) != kSuccess)
                return false;
            tw = sw;
            th = sh;
            break;
        }
    }
    const std::size_t stride = a.minimumStride(decoder.get());
    if (stride < std::size_t(tw) * 4)
        return false;
    std::vector<unsigned char> pixels(stride * std::size_t(th));
    if (a.decodeImage(decoder.get(), pixels.data(), stride, pixels.size()) != kSuccess)
        return false;
    if (stride != std::size_t(tw) * 4) { // rows packed together
        for (std::int32_t y = 1; y < th; ++y)
            std::copy_n(pixels.data() + std::size_t(y) * stride, std::size_t(tw) * 4, pixels.data() + std::size_t(y) * std::size_t(tw) * 4);
        pixels.resize(std::size_t(tw) * std::size_t(th) * 4);
    }
    rgba.swap(pixels);
    width = int(tw);
    height = int(th);
    return true;
}

} // namespace enp::os::image
