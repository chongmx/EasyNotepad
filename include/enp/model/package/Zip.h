// Reads ZIP archives in memory: the container of .docx, .xlsx and .pptx. Stored and deflated
// entries; no ZIP64, no encryption. Entries are read on request, each bounded in size, and the
// archive as a whole is bounded in entry count, so a hostile file fails instead of filling memory.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model::package {

struct ZipEntry {
    std::string name;            // as stored: '/' separated, no leading '/'
    std::uint16_t method = 0;    // 0 stored, 8 deflated
    std::uint32_t compressedSize = 0;
    std::uint32_t size = 0;      // uncompressed, as the directory states
    std::uint32_t localOffset = 0;
};

class ZipArchive {
public:
    static constexpr std::size_t kMaxEntries = 20000;
    static constexpr std::size_t kMaxEntryBytes = std::size_t(256) << 20;

    // Takes the whole file. False (and `error`) when it is not a ZIP archive this reads.
    bool open(std::string bytes, std::string* error = nullptr);

    const std::vector<ZipEntry>& entries() const { return entries_; }
    // By name; OOXML part names are case-insensitive, so a case-insensitive match is tried second.
    const ZipEntry* find(std::string_view name) const;
    bool contains(std::string_view name) const { return find(name) != nullptr; }
    // The entry's bytes, inflated; nullopt when missing, broken or larger than `limit`.
    std::optional<std::string> read(std::string_view name, std::size_t limit = kMaxEntryBytes) const;
    std::optional<std::string> read(const ZipEntry& entry, std::size_t limit = kMaxEntryBytes) const;

private:
    std::string bytes_;
    std::vector<ZipEntry> entries_;
};

} // namespace enp::model::package
