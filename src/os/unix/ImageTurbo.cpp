// JPEG pictures through libjpeg-turbo's TurboJPEG library when the system has it (most Linux
// desktops do: libturbojpeg.so.0, SIMD on x86 and Arm): decoded straight at 1/2, 1/4 or 1/8 of
// their size. Looked up when first needed, so the app neither needs it to build nor to start;
// without it the caller decodes the picture itself. Its 2.x functions (in 3.x too).
#include "enp/os/Image.h"

#include <dlfcn.h>

#include <algorithm>
#include <climits>
#include <mutex>
#include <type_traits>

namespace enp::os::image {

namespace {

using Handle = void*;
struct ScalingFactor {
    int num, denom;
};
constexpr int kPixelRgba = 7; // TJPF_RGBA

struct Api {
    Handle (*init)() = nullptr;
    int (*header)(Handle, const unsigned char*, unsigned long, int*, int*, int*, int*) = nullptr;
    ScalingFactor* (*factors)(int*) = nullptr;
    int (*decompress)(Handle, const unsigned char*, unsigned long, unsigned char*, int, int, int, int, int) = nullptr;
    int (*destroy)(Handle) = nullptr;
    bool ok = false;
};

const Api& api()
{
    static Api a;
    static std::once_flag once;
    std::call_once(once, [] {
        // Kept loaded for the process's life (once: the same handle every time).
        void* lib = dlopen("libturbojpeg.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
            return;
        auto find = [lib](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, name));
            return fn != nullptr;
        };
        a.ok = find(a.init, "tjInitDecompress") && find(a.header, "tjDecompressHeader3") && find(a.factors, "tjGetScalingFactors") &&
               find(a.decompress, "tjDecompress2") && find(a.destroy, "tjDestroy");
    });
    return a;
}

// A decompressor for this thread, for the thread's life (destroyed when it ends).
struct Decompressor {
    Handle h = nullptr;
    Decompressor()
    {
        if (api().ok)
            h = api().init();
    }
    ~Decompressor()
    {
        if (h)
            api().destroy(h);
    }
    Decompressor(const Decompressor&) = delete;
    Decompressor& operator=(const Decompressor&) = delete;
};

} // namespace

bool decodeJpeg(std::string_view jpeg, int maxSide, std::vector<unsigned char>& rgba, int& width, int& height)
{
    const Api& a = api();
    if (!a.ok || jpeg.empty() || jpeg.size() > std::size_t(ULONG_MAX) || maxSide < 1)
        return false;
    static thread_local Decompressor decompressor;
    if (!decompressor.h)
        return false;
    const auto* data = reinterpret_cast<const unsigned char*>(jpeg.data());
    int w = 0, h = 0, subsampling = 0, colorspace = 0;
    if (a.header(decompressor.h, data, static_cast<unsigned long>(jpeg.size()), &w, &h, &subsampling, &colorspace) != 0 || w <= 0 ||
        h <= 0)
        return false;
    // The smallest scale still at least `maxSide` across (the caller shrinks the rest).
    int count = 0;
    const ScalingFactor* factors = a.factors(&count);
    int tw = w, th = h;
    for (int i = 0; factors && i < count; ++i) {
        const ScalingFactor f = factors[i];
        if (f.num <= 0 || f.denom <= 0 || f.num > f.denom)
            continue;
        const int sw = (w * f.num + f.denom - 1) / f.denom, sh = (h * f.num + f.denom - 1) / f.denom;
        if (std::max(sw, sh) >= std::min(maxSide, std::max(w, h)) && std::size_t(sw) * std::size_t(sh) < std::size_t(tw) * std::size_t(th)) {
            tw = sw;
            th = sh;
        }
    }
    std::vector<unsigned char> pixels(std::size_t(tw) * std::size_t(th) * 4);
    if (a.decompress(decompressor.h, data, static_cast<unsigned long>(jpeg.size()), pixels.data(), tw, tw * 4, th, kPixelRgba, 0) != 0)
        return false;
    rgba.swap(pixels);
    width = tw;
    height = th;
    return true;
}

} // namespace enp::os::image
