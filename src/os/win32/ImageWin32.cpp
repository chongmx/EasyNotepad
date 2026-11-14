// JPEG pictures through WIC (Windows Imaging Component): a scaler on the decoder's frame, which
// WIC serves by decoding the JPEG at a reduced scale, then a converter to RGBA8.
#include "enp/os/Image.h"

// Without NOMINMAX, MSVC's <windows.h> defines min and max as macros, which break std::max.
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>

#include <algorithm>
#include <utility>
#include <climits>

namespace enp::os::image {

namespace {

// A COM interface pointer released when it goes.
template <class T>
class Com {
public:
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com()
    {
        if (p_)
            p_->Release();
    }
    T** put() { return &p_; }
    T* operator->() const { return p_; }
    T* get() const { return p_; }

private:
    T* p_ = nullptr;
};

// COM and a WIC factory for this thread, for the thread's life (the reader service's workers live
// as long as the app): the factory let go, then COM uninitialised, when the thread ends.
struct ComThread {
    IWICImagingFactory* factory = nullptr;
    ComThread()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owns_ = SUCCEEDED(hr);
        if (SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE) // already initialised otherwise: usable
            if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
                factory = nullptr;
    }
    ~ComThread()
    {
        if (factory)
            factory->Release();
        if (owns_)
            CoUninitialize();
    }
    ComThread(const ComThread&) = delete;
    ComThread& operator=(const ComThread&) = delete;

private:
    bool owns_ = false;
};

} // namespace

bool decodeJpeg(std::string_view jpeg, int maxSide, std::vector<unsigned char>& rgba, int& width, int& height)
{
    static thread_local ComThread com;
    IWICImagingFactory* factory = com.factory;
    if (!factory || jpeg.empty() || jpeg.size() > DWORD(INT_MAX) || maxSide < 1)
        return false;
    Com<IWICStream> stream;
    if (FAILED(factory->CreateStream(stream.put())) ||
        FAILED(stream->InitializeFromMemory(reinterpret_cast<BYTE*>(const_cast<char*>(jpeg.data())), DWORD(jpeg.size()))))
        return false;
    Com<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()))) // (no EXIF turn)
        return false;
    Com<IWICBitmapFrameDecode> frame;
    UINT w = 0, h = 0;
    if (FAILED(decoder->GetFrame(0, frame.put())) || FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0 || w > 65535 || h > 65535)
        return false;
    UINT tw = w, th = h;
    if (std::max(w, h) > UINT(maxSide)) {
        const double k = double(maxSide) / double(std::max(w, h));
        tw = std::max(1u, UINT(double(w) * k + 0.5));
        th = std::max(1u, UINT(double(h) * k + 0.5));
    }
    IWICBitmapSource* source = frame.get();
    Com<IWICBitmapScaler> scaler;
    if (tw != w || th != h) {
        if (FAILED(factory->CreateBitmapScaler(scaler.put())) || FAILED(scaler->Initialize(frame.get(), tw, th, WICBitmapInterpolationModeFant)))
            return false;
        source = scaler.get();
    }
    // BGRA (every Windows from 7 converts to it; RGBA needs a later one), red and blue swapped here.
    Com<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    const UINT stride = tw * 4;
    std::vector<unsigned char> pixels(size_t(stride) * th);
    if (FAILED(converter->CopyPixels(nullptr, stride, UINT(pixels.size()), pixels.data())))
        return false;
    for (size_t i = 0; i < pixels.size(); i += 4)
        std::swap(pixels[i], pixels[i + 2]);
    rgba.swap(pixels);
    width = int(tw);
    height = int(th);
    return true;
}

} // namespace enp::os::image
