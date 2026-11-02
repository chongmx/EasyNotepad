#include "enp/services/ImageEncoder.h"

#include <algorithm>
#include <cstring>
#include <new>

// stb_image_write (3rdparty/stb, nothings/stb commit d14d46f6) allocates through the global
// operator new like stb_image in ImageDecoder.cpp: the accounting sees it, and these are raw
// allocation calls outside src/os only to feed a C library.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#define STBIW_MALLOC(size) ::operator new(size, std::nothrow)
#define STBIW_REALLOC_SIZED(ptr, oldSize, newSize) enp::services::stbiwRealloc(ptr, oldSize, newSize)
#define STBIW_FREE(ptr) ::operator delete(ptr)

namespace enp::services {
inline void* stbiwRealloc(void* ptr, size_t oldSize, size_t newSize)
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
#   pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#   if !defined(__clang__) && __GNUC__ >= 12
#       pragma GCC diagnostic ignored "-Wuse-after-free"
#   endif
#endif
#if defined(_MSC_VER)
#   pragma warning(push)
#   pragma warning(disable : 4996 4244 4267)
#endif
#include "stb_image_write.h"
#if defined(_MSC_VER)
#   pragma warning(pop)
#endif
#if defined(__GNUC__)
#   pragma GCC diagnostic pop
#endif

namespace enp::services {

std::string encodePng(const unsigned char* rgba, int width, int height)
{
    std::string png;
    if (!rgba || width <= 0 || height <= 0)
        return png;
    auto append = [](void* context, void* data, int size) { static_cast<std::string*>(context)->append(static_cast<const char*>(data), size_t(size)); };
    if (!stbi_write_png_to_func(append, &png, width, height, 4, rgba, width * 4))
        std::string().swap(png);
    return png;
}

} // namespace enp::services
