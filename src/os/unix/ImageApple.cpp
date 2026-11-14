// JPEG pictures through ImageIO (macOS): a thumbnail at the size wanted, which ImageIO decodes at
// a reduced scale, drawn into RGBA8 pixels.
#include "enp/os/Image.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#include <algorithm>
#include <climits>

namespace enp::os::image {

namespace {

// A Core Foundation object released when it goes.
template <class T>
class Cf {
public:
    explicit Cf(T ref = nullptr) : ref_(ref) {}
    Cf(const Cf&) = delete;
    Cf& operator=(const Cf&) = delete;
    ~Cf()
    {
        if (ref_)
            CFRelease(ref_);
    }
    T get() const { return ref_; }

private:
    T ref_;
};

} // namespace

bool decodeJpeg(std::string_view jpeg, int maxSide, std::vector<unsigned char>& rgba, int& width, int& height)
{
    if (jpeg.empty() || jpeg.size() > std::size_t(LONG_MAX) || maxSide < 1)
        return false;
    Cf<CFDataRef> data(CFDataCreateWithBytesNoCopy(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(jpeg.data()), CFIndex(jpeg.size()),
                                                   kCFAllocatorNull));
    if (!data.get())
        return false;
    // Nothing cached by ImageIO (the picture is decoded once, here); the thumbnail decoded now, on
    // this thread, at the size wanted (subsampled), from the picture itself (never its embedded
    // thumbnail), as stored (no EXIF turn).
    const void* sourceKeys[] = {kCGImageSourceShouldCache};
    const void* sourceValues[] = {kCFBooleanFalse};
    Cf<CFDictionaryRef> sourceOptions(CFDictionaryCreate(kCFAllocatorDefault, sourceKeys, sourceValues, 1, &kCFTypeDictionaryKeyCallBacks,
                                                         &kCFTypeDictionaryValueCallBacks));
    Cf<CGImageSourceRef> source(CGImageSourceCreateWithData(data.get(), sourceOptions.get()));
    if (!source.get())
        return false;
    const int side = maxSide;
    Cf<CFNumberRef> size(CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &side));
    const void* keys[] = {kCGImageSourceCreateThumbnailFromImageAlways, kCGImageSourceThumbnailMaxPixelSize,
                          kCGImageSourceCreateThumbnailWithTransform, kCGImageSourceShouldCacheImmediately};
    const void* values[] = {kCFBooleanTrue, size.get(), kCFBooleanFalse, kCFBooleanTrue};
    Cf<CFDictionaryRef> options(
        CFDictionaryCreate(kCFAllocatorDefault, keys, values, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    Cf<CGImageRef> image(CGImageSourceCreateThumbnailAtIndex(source.get(), 0, options.get()));
    if (!image.get())
        return false;
    const std::size_t w = CGImageGetWidth(image.get()), h = CGImageGetHeight(image.get());
    if (w == 0 || h == 0 || w > 65535 || h > 65535)
        return false;
    std::vector<unsigned char> pixels(w * h * 4);
    Cf<CGColorSpaceRef> space(CGColorSpaceCreateWithName(kCGColorSpaceSRGB));
    Cf<CGContextRef> context(CGBitmapContextCreate(pixels.data(), w, h, 8, w * 4, space.get(),
                                                   kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big));
    if (!context.get())
        return false;
    CGContextDrawImage(context.get(), CGRectMake(0, 0, CGFloat(w), CGFloat(h)), image.get());
    rgba.swap(pixels);
    width = int(w);
    height = int(h);
    return true;
}

} // namespace enp::os::image
