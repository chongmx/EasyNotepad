#include "enp/model/package/Inflate.h"

#include <algorithm>
#include <cstring>
#include <new>

// stb_image's zlib decoder, compiled here with internal linkage (STB_IMAGE_STATIC) so it does not
// clash with the image decoder's copy in the services. It allocates through the global operator
// new (the accounting sees it), and its growth goes through limitedRealloc, which refuses to grow
// past the caller's limit: a zip bomb fails at the limit instead of filling memory.
namespace enp::model::package {
namespace {
thread_local std::size_t g_limit = 0;

// realloc's contract: on failure the old block stays (stb frees it itself).
void* limitedRealloc(void* ptr, std::size_t oldSize, std::size_t newSize)
{
    if (newSize > g_limit)
        return nullptr;
    void* fresh = ::operator new(newSize, std::nothrow);
    if (!fresh)
        return nullptr;
    if (ptr)
        std::memcpy(fresh, ptr, std::min(oldSize, newSize));
    ::operator delete(ptr);
    return fresh;
}
} // namespace
} // namespace enp::model::package

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_MALLOC(size) ::operator new(size, std::nothrow)
#define STBI_REALLOC_SIZED(ptr, oldSize, newSize) enp::model::package::limitedRealloc(ptr, oldSize, newSize)
#define STBI_FREE(ptr) ::operator delete(ptr)
#if defined(__GNUC__)
#   pragma GCC diagnostic push
#   pragma GCC diagnostic ignored "-Wunused-function"
#   pragma GCC diagnostic ignored "-Wsign-compare"
#   pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#   pragma GCC diagnostic ignored "-Wunused-parameter"
#   if !defined(__clang__)
#       pragma GCC diagnostic ignored "-Wstringop-overflow" // MinGW GCC, in stb's PNG code (not used here)
#   endif
#   if !defined(__clang__) && __GNUC__ >= 12
#       pragma GCC diagnostic ignored "-Wuse-after-free"
#   endif
#elif defined(_MSC_VER)
#   pragma warning(push)
#   pragma warning(disable : 4100 4244 4245 4456 4457 4505 4701 4703)
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#   pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#   pragma warning(pop)
#endif

namespace enp::model::package {

std::optional<std::string> inflate(std::string_view data, bool zlibHeader, std::size_t limit, std::size_t expected)
{
    if (data.empty() || data.size() > std::size_t(0x7FFFFFFF) || limit == 0)
        return std::nullopt;
    g_limit = std::min<std::size_t>(limit, 0x7FFFFFFF);
    const std::size_t guess = std::clamp<std::size_t>(expected ? expected : data.size() * 4, 64, g_limit);
    // stb reads its bit buffer ahead and fails ("unexpected end") on a valid stream whose last
    // code ends near the end of the input: a few zero bytes after it give it the look-ahead.
    std::string padded;
    padded.reserve(data.size() + 8);
    padded.append(data);
    padded.append(8, '\0');
    int outLength = 0;
    char* out = stbi_zlib_decode_malloc_guesssize_headerflag(padded.data(), int(padded.size()), int(guess), &outLength, zlibHeader ? 1 : 0);
    g_limit = 0;
    if (!out)
        return std::nullopt;
    std::optional<std::string> result(std::in_place, out, std::size_t(outLength));
    STBI_FREE(out);
    return result;
}

} // namespace enp::model::package
