// The emoji pictures (resources/emoji/noto-emoji-72.zip): a zip of PNGs named by key
// (model/Emoji.h). Only the zip's index is read, when a picture is first asked for; each PNG is
// read from its offset when needed, so the 12 MB file is never held in memory. unload() frees the
// index (about 200 KB); it is read again on the next request.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::services {

class EmojiPack {
public:
    // `location`: a bundled resource ("emoji/noto-emoji-72.zip"), or a file path when `isFile`.
    explicit EmojiPack(std::string location = "emoji/noto-emoji-72.zip", bool isFile = false);

    bool contains(std::string_view key);
    // The PNG bytes of `key`, or nothing when the pack has no such picture or cannot be read.
    std::optional<std::string> read(std::string_view key);
    void unload();
    bool loaded() const { return loaded_; }
    size_t size() const { return entries_.size(); }

private:
    struct Entry {
        std::uint32_t name = 0, nameLength = 0; // in names_
        std::uint64_t offset = 0;               // of the local header
        std::uint32_t size = 0, rawSize = 0, method = 0;
    };
    bool load();
    const Entry* find(std::string_view key) const;
    std::optional<std::string> range(std::uint64_t offset, size_t size, std::uint64_t* total = nullptr) const;

    std::string location_;
    bool isFile_;
    bool loaded_ = false;
    bool failed_ = false;     // the pack is missing or unreadable: not tried again
    std::string names_;       // every key, back to back
    std::vector<Entry> entries_; // sorted by key
};

} // namespace enp::services
