#include "enp/services/EmojiPack.h"

#include "enp/os/Fs.h"
#include "enp/os/Memory.h"
#include "enp/services/ImageDecoder.h"

#include <algorithm>
#include <cstring>

namespace enp::services {

namespace {

std::uint32_t u16(const std::string& b, size_t at) { return std::uint32_t(std::uint8_t(b[at])) | std::uint32_t(std::uint8_t(b[at + 1])) << 8; }
std::uint32_t u32(const std::string& b, size_t at) { return u16(b, at) | u16(b, at + 2) << 16; }

} // namespace

EmojiPack::EmojiPack(std::string location, bool isFile) : location_(std::move(location)), isFile_(isFile) {}

std::optional<std::string> EmojiPack::range(std::uint64_t offset, size_t size, std::uint64_t* total) const
{
    return isFile_ ? os::fs::readFileRange(location_, offset, size, total) : os::fs::readAssetRange(location_, offset, size, total);
}

bool EmojiPack::load()
{
    if (loaded_)
        return true;
    if (failed_)
        return false;
    failed_ = true; // until it has worked
    os::mem::Scope scope("emoji");
    std::uint64_t total = 0;
    if (!range(0, 0, &total) || total < 22)
        return false;
    // The end-of-central-directory record: in the last 22 bytes plus at most a 64 KB comment.
    const size_t tailSize = size_t(std::min<std::uint64_t>(total, 22 + 65535));
    const std::optional<std::string> tail = range(total - tailSize, tailSize);
    if (!tail || tail->size() < 22)
        return false;
    size_t eocd = std::string::npos;
    for (size_t i = tail->size() - 22 + 1; i-- > 0;)
        if (u32(*tail, i) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd == std::string::npos)
        return false;
    const std::uint32_t count = u16(*tail, eocd + 10);
    const std::uint32_t cdSize = u32(*tail, eocd + 12);
    const std::uint32_t cdOffset = u32(*tail, eocd + 16);
    if (count == 0xFFFF || cdOffset == 0xFFFFFFFFu || std::uint64_t(cdOffset) + cdSize > total)
        return false; // zip64: not used by the pack
    const std::optional<std::string> cd = range(cdOffset, cdSize);
    if (!cd || cd->size() != cdSize)
        return false;
    std::vector<Entry> entries;
    std::string names;
    entries.reserve(count);
    for (size_t at = 0; at + 46 <= cd->size() && entries.size() < count;) {
        if (u32(*cd, at) != 0x02014b50)
            break;
        Entry e;
        e.method = u16(*cd, at + 10);
        e.size = u32(*cd, at + 20);
        e.rawSize = u32(*cd, at + 24);
        const std::uint32_t nameLength = u16(*cd, at + 28), extra = u16(*cd, at + 30), comment = u16(*cd, at + 32);
        e.offset = u32(*cd, at + 42);
        if (at + 46 + nameLength > cd->size())
            break;
        std::string_view name(cd->data() + at + 46, nameLength);
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0 && (e.method == 0 || e.method == 8)) {
            name.remove_suffix(4);
            e.name = std::uint32_t(names.size());
            e.nameLength = std::uint32_t(name.size());
            names.append(name);
            entries.push_back(e);
        }
        at += 46 + nameLength + extra + comment;
    }
    std::sort(entries.begin(), entries.end(), [&](const Entry& a, const Entry& b) {
        return std::string_view(names).substr(a.name, a.nameLength) < std::string_view(names).substr(b.name, b.nameLength);
    });
    entries.shrink_to_fit();
    names.shrink_to_fit();
    entries_ = std::move(entries);
    names_ = std::move(names);
    loaded_ = !entries_.empty();
    failed_ = !loaded_;
    return loaded_;
}

const EmojiPack::Entry* EmojiPack::find(std::string_view key) const
{
    const std::string_view names(names_);
    auto it = std::lower_bound(entries_.begin(), entries_.end(), key,
                               [&](const Entry& e, std::string_view k) { return names.substr(e.name, e.nameLength) < k; });
    if (it == entries_.end() || names.substr(it->name, it->nameLength) != key)
        return nullptr;
    return &*it;
}

bool EmojiPack::contains(std::string_view key)
{
    return load() && find(key) != nullptr;
}

std::optional<std::string> EmojiPack::read(std::string_view key)
{
    if (!load())
        return std::nullopt;
    const Entry* e = find(key);
    if (!e)
        return std::nullopt;
    // The local header repeats the name and may have its own extra field: the data follows it.
    const std::optional<std::string> header = range(e->offset, 30);
    if (!header || header->size() < 30 || u32(*header, 0) != 0x04034b50)
        return std::nullopt;
    const std::uint64_t data = e->offset + 30 + u16(*header, 26) + u16(*header, 28);
    std::optional<std::string> bytes = range(data, e->size);
    if (!bytes || bytes->size() != e->size)
        return std::nullopt;
    if (e->method == 8)
        return inflateRaw(*bytes, e->rawSize);
    return bytes;
}

void EmojiPack::unload()
{
    os::mem::release(entries_);
    os::mem::release(names_);
    loaded_ = false;
    failed_ = false;
}

} // namespace enp::services
