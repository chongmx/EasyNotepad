#include "enp/model/package/Zip.h"

#include "enp/model/package/Inflate.h"

#include <algorithm>
#include <cctype>

namespace enp::model::package {
namespace {

std::uint16_t u16(std::string_view b, std::size_t at)
{
    return std::uint16_t(std::uint8_t(b[at]) | (std::uint8_t(b[at + 1]) << 8));
}

std::uint32_t u32(std::string_view b, std::size_t at)
{
    return std::uint32_t(u16(b, at)) | (std::uint32_t(u16(b, at + 2)) << 16);
}

bool sameIgnoringCase(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

} // namespace

bool ZipArchive::open(std::string bytes, std::string* error)
{
    auto fail = [&](const char* why) {
        if (error)
            *error = why;
        entries_.clear();
        bytes_.clear();
        return false;
    };
    bytes_ = std::move(bytes);
    entries_.clear();
    const std::string_view b(bytes_);
    // The end-of-central-directory record: in the last 64 KB + 22 bytes (it ends with a comment).
    if (b.size() < 22)
        return fail("too short for a ZIP archive");
    std::size_t eocd = std::string_view::npos;
    const std::size_t lowest = b.size() > 65557 ? b.size() - 65557 : 0;
    for (std::size_t at = b.size() - 22 + 1; at-- > lowest;)
        if (b[at] == 'P' && b[at + 1] == 'K' && b[at + 2] == 5 && b[at + 3] == 6) {
            eocd = at;
            break;
        }
    if (eocd == std::string_view::npos)
        return fail("no ZIP directory (not a ZIP archive, or truncated)");
    const std::size_t count = u16(b, eocd + 10);
    const std::uint32_t dirSize = u32(b, eocd + 12);
    const std::uint32_t dirOffset = u32(b, eocd + 16);
    if (count == 0xFFFF || dirOffset == 0xFFFFFFFFu)
        return fail("ZIP64 archives are not supported");
    if (count > kMaxEntries)
        return fail("too many entries in the ZIP archive");
    if (std::size_t(dirOffset) + dirSize > eocd)
        return fail("the ZIP directory lies outside the file");
    entries_.reserve(count);
    std::size_t at = dirOffset;
    for (std::size_t i = 0; i < count; ++i) {
        if (at + 46 > eocd || u32(b, at) != 0x02014b50u)
            return fail("a broken ZIP directory entry");
        ZipEntry e;
        const std::uint16_t flags = u16(b, at + 8);
        e.method = u16(b, at + 10);
        e.compressedSize = u32(b, at + 20);
        e.size = u32(b, at + 24);
        const std::size_t nameLength = u16(b, at + 28), extraLength = u16(b, at + 30), commentLength = u16(b, at + 32);
        e.localOffset = u32(b, at + 42);
        if (at + 46 + nameLength > eocd)
            return fail("a broken ZIP directory entry");
        e.name.assign(b.substr(at + 46, nameLength));
        std::replace(e.name.begin(), e.name.end(), '\\', '/');
        at += 46 + nameLength + extraLength + commentLength;
        if (flags & 1)
            continue; // encrypted: left out (read() reports it missing)
        entries_.push_back(std::move(e));
    }
    return true;
}

const ZipEntry* ZipArchive::find(std::string_view name) const
{
    if (!name.empty() && name.front() == '/')
        name.remove_prefix(1);
    for (const ZipEntry& e : entries_)
        if (e.name == name)
            return &e;
    for (const ZipEntry& e : entries_)
        if (sameIgnoringCase(e.name, name))
            return &e;
    return nullptr;
}

std::optional<std::string> ZipArchive::read(std::string_view name, std::size_t limit) const
{
    const ZipEntry* e = find(name);
    return e ? read(*e, limit) : std::nullopt;
}

std::optional<std::string> ZipArchive::read(const ZipEntry& e, std::size_t limit) const
{
    const std::string_view b(bytes_);
    const std::size_t at = e.localOffset;
    if (at + 30 > b.size() || u32(b, at) != 0x04034b50u)
        return std::nullopt;
    const std::size_t data = at + 30 + u16(b, at + 26) + u16(b, at + 28);
    if (data > b.size() || e.compressedSize > b.size() - data)
        return std::nullopt;
    const std::string_view packed = b.substr(data, e.compressedSize);
    limit = std::min(limit, kMaxEntryBytes);
    if (e.method == 0)
        return packed.size() <= limit ? std::optional<std::string>(std::in_place, packed) : std::nullopt;
    if (e.method == 8) {
        if (e.size == 0 && e.compressedSize <= 2)
            return std::string();
        return inflate(packed, false, limit, e.size);
    }
    return std::nullopt; // another method (bzip2, LZMA): not read
}

} // namespace enp::model::package
