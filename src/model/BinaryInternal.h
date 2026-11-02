// Shared by the readers of BinaryInfo (ELF, PE/COFF, archives): a bounded window onto the file
// with a small read-ahead block, byte-order aware field access, and the builder that pools strings.
#pragma once

#include "enp/model/BinaryInfo.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <unordered_map>

namespace enp::model::bin::detail {

// One block of the file kept between reads: archive member headers and small tables are served
// from it instead of a read each.
struct Cache {
    const Reader& reader;
    std::uint64_t at = kNone;
    std::string block;
    std::uint64_t fileSize = 0;
};

// The file, or one member of an archive: offsets are relative to `base`, reads stop at `size`.
class Source {
public:
    Source(Cache& cache, std::uint64_t base, std::uint64_t size) : cache_(&cache), base_(base), size_(size) {}
    std::uint64_t size() const { return size_; }
    Source sub(std::uint64_t offset, std::uint64_t size) const
    {
        offset = std::min(offset, size_);
        return Source(*cache_, base_ + offset, std::min(size, size_ - offset));
    }
    // Up to `size` bytes at `offset` (fewer at the end); false when nothing could be read.
    bool read(std::uint64_t offset, size_t size, std::string& out) const;

private:
    Cache* cache_;
    std::uint64_t base_, size_;
};

// Fields of a header in the file's byte order; reads beyond the end give 0.
struct Bytes {
    std::string_view b;
    bool big = false;
    std::uint8_t u8(size_t at) const { return at < b.size() ? std::uint8_t(b[at]) : 0; }
    std::uint16_t u16(size_t at) const
    {
        if (at + 2 > b.size())
            return 0;
        const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
        return big ? std::uint16_t(p[0] << 8 | p[1]) : std::uint16_t(p[1] << 8 | p[0]);
    }
    std::uint32_t u32(size_t at) const
    {
        if (at + 4 > b.size())
            return 0;
        const auto* p = reinterpret_cast<const unsigned char*>(b.data() + at);
        return big ? std::uint32_t(p[0]) << 24 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 8 | p[3]
                   : std::uint32_t(p[3]) << 24 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[1]) << 8 | p[0];
    }
    std::uint64_t u64(size_t at) const
    {
        if (at + 8 > b.size())
            return 0;
        const std::uint64_t lo = u32(at), hi = u32(at + 4);
        return big ? lo << 32 | hi : hi << 32 | lo;
    }
    std::uint64_t word(size_t at, bool is64) const { return is64 ? u64(at) : u32(at); }
    // A NUL-terminated string starting at `at` (to the end when there is no NUL).
    std::string_view cstr(size_t at) const
    {
        if (at >= b.size())
            return {};
        const size_t end = b.find('\0', at);
        return b.substr(at, (end == std::string_view::npos ? b.size() : end) - at);
    }
};

class Builder {
public:
    explicit Builder(Image& image) : image_(image) {}
    Image& image() { return image_; }

    Text add(std::string_view s);
    Text intern(std::string_view s); // section names, versions, library names: stored once
    void heading(std::string_view title);
    void property(std::string_view key, std::string_view value);
    // A new symbol named `raw` (demangled here); nullptr once kMaxSymbols is reached.
    Symbol* symbol(std::string_view raw, Side side);
    std::uint32_t library(std::string_view name); // found or added
    ListNode& list(std::string_view title, ListType type, int depth);
    // Counts each list node's rows (after all symbols are in).
    void countLists();
    // Names the lists of a linked image ("Exported functions") or of objects ("Defined functions").
    void buildLists(bool image);

private:
    Image& image_;
    std::unordered_map<std::string, Text> interned_;
};

constexpr size_t kMaxName = 16 * 1024; // longer names are cut (a template-heavy C++ name can be long)
constexpr size_t kMaxTable = 256u * 1024u * 1024u; // the largest symbol or string table read

std::string hex(std::uint64_t v);
std::string number(std::uint64_t v); // with thousands separators

// Readers per format. `member`: index of the archive member being read, or kNoIndex for the file
// itself (which then also fills the overview, sections and dependencies). They return a short
// description ("ELF 64-bit object, x86-64"), and set image.error for the file itself on failure.
std::string readElf(const Source& src, Builder& b, std::uint32_t member);
std::string readPe(const Source& src, Builder& b);
std::string readCoff(const Source& src, Builder& b, std::uint32_t member, bool bigObj);
bool isCoffObject(std::string_view head, bool* bigObj);
const char* coffMachine(std::uint16_t machine);

} // namespace enp::model::bin::detail
