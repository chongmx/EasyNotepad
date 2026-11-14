#include "enp/model/package/Inflate.h"

#include "libdeflate.h"

#include <algorithm>
#include <cstdint>
#include <new>

// libdeflate (3rdparty/libdeflate, pinned): several times faster than a byte-at-a-time inflater,
// with the processor's vector instructions where it has them (chosen at run time). It inflates
// into a buffer of a known size, so an unknown size is guessed and the guess grown until it fits,
// never past the caller's limit: a zip bomb fails at the limit instead of filling memory. Its
// memory comes from the global operator new (the accounting sees it).
namespace enp::model::package {

namespace {

void* allocate(std::size_t size)
{
    return ::operator new(size, std::nothrow);
}

void release(void* p)
{
    ::operator delete(p);
}

// The decompressor (its tables, about 10 KB): made for each call and freed after it, so nothing is
// kept between calls, whatever thread inflates.
struct Decompressor {
    libdeflate_decompressor* d = nullptr;
    Decompressor()
    {
        libdeflate_options options{};
        options.sizeof_options = sizeof(options);
        options.malloc_func = allocate;
        options.free_func = release;
        d = libdeflate_alloc_decompressor_ex(&options);
    }
    ~Decompressor()
    {
        if (d)
            libdeflate_free_decompressor(d);
    }
    Decompressor(const Decompressor&) = delete;
    Decompressor& operator=(const Decompressor&) = delete;
};

} // namespace

std::optional<std::string> inflate(std::string_view data, bool zlibHeader, std::size_t limit, std::size_t expected)
{
    if (data.empty() || data.size() > std::size_t(0x7FFFFFFF) || limit == 0)
        return std::nullopt;
    limit = std::min<std::size_t>(limit, 0x7FFFFFFF);
    if (zlibHeader) {
        // The 2-byte zlib header (deflate, no preset dictionary), then raw deflate data. Its
        // checksum at the end is not checked: some PDF writers get it wrong, and the data is good.
        if (data.size() < 2)
            return std::nullopt;
        const unsigned cmf = std::uint8_t(data[0]), flg = std::uint8_t(data[1]);
        if ((cmf & 0x0F) != 8 || (cmf * 256 + flg) % 31 != 0 || (flg & 0x20))
            return std::nullopt;
        data.remove_prefix(2);
    }
    Decompressor decompressor;
    if (!decompressor.d)
        return std::nullopt;
    std::size_t size = std::clamp<std::size_t>(expected ? expected : data.size() * 4, 64, limit);
    std::string out;
    for (;;) {
        out.resize(size);
        std::size_t used = 0, produced = 0;
        const libdeflate_result r = libdeflate_deflate_decompress_ex(decompressor.d, data.data(), data.size(), &out[0], out.size(), &used, &produced);
        if (r == LIBDEFLATE_SUCCESS) {
            out.resize(produced);
            if (out.capacity() > produced + produced / 4 + 4096) // a guess well over: the room given back
                out.shrink_to_fit();
            return out;
        }
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE || size >= limit)
            return std::nullopt;
        size = std::min(limit, size * 4); // larger: inflated again from the start
    }
}

} // namespace enp::model::package
