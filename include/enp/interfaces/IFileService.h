// Contract for reading and writing workspace files.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::interfaces {

// What a file looked like on disk (to notice changes made by other programs).
struct FileStamp {
    bool exists = false;
    unsigned long long size = 0;
    long long modified = 0; // seconds since the epoch
    bool operator==(const FileStamp& o) const { return exists == o.exists && size == o.size && modified == o.modified; }
    bool operator!=(const FileStamp& o) const { return !(*this == o); }
};

struct DirectoryEntry {
    std::string name;
    bool isDirectory = false;
    bool hidden = false; // a dot name, or hidden by the platform (the Windows attribute)
};

class IFileService {
public:
    virtual ~IFileService() = default;
    virtual std::optional<std::string> read(const std::string& path, std::string* error = nullptr) = 0;
    // Reads a whole file into `out`, reusing its storage (one buffer for a whole scan); false when
    // it cannot be read or is larger than `maxBytes`.
    virtual bool readInto(const std::string& path, std::string& out, size_t maxBytes, std::string* error = nullptr)
    {
        std::optional<std::string> text = read(path, error);
        if (!text || text->size() > maxBytes)
            return false;
        out.assign(*text);
        return true;
    }
    virtual bool write(const std::string& path, std::string_view data, std::string* error = nullptr) = 0;
    // Part of a file: up to `size` bytes from `offset` (fewer at the end), so a large file is read in
    // pieces; `*total` gets the file's size. nullopt where it cannot be read (or the service cannot).
    virtual std::optional<std::string> readRange(const std::string& path, std::uint64_t offset, size_t size, std::uint64_t* total = nullptr)
    {
        (void)path, (void)offset, (void)size, (void)total;
        return std::nullopt;
    }
    // Directories first, then files, each sorted by name.
    virtual std::vector<DirectoryEntry> list(const std::string& path) = 0;
    // At most `limit` entries (0: all; which ones is up to the file system, sorted all the same),
    // without reading a huge folder whole where the platform allows; `*more`: the folder holds more.
    virtual std::vector<DirectoryEntry> list(const std::string& path, size_t limit, bool* more)
    {
        std::vector<DirectoryEntry> all = list(path);
        if (more)
            *more = limit > 0 && all.size() > limit;
        if (limit > 0 && all.size() > limit)
            all.resize(limit);
        return all;
    }
    virtual bool isDirectory(const std::string& path) = 0;
    virtual std::string absolute(const std::string& path) = 0;
    virtual bool createDirectories(const std::string& path) = 0;
    virtual std::string homeDirectory() { return {}; }
    // Starting points for browsing (drive letters on Windows), found without touching the drives.
    virtual std::vector<std::string> roots() { return {}; }
    // False while the app may only see its own folders (Android without "All files access").
    virtual bool hasStorageAccess() { return true; }
    // Asks the user for that access (the system settings on Android); returns at once.
    virtual void requestStorageAccess() {}
    virtual bool rename(const std::string& from, const std::string& to, std::string* error = nullptr)
    {
        (void)from, (void)to, (void)error;
        return false;
    }
    // A file or a whole folder; `*recycled` when it went to the Recycle Bin / Trash.
    virtual bool remove(const std::string& path, bool* recycled = nullptr, std::string* error = nullptr)
    {
        (void)path, (void)recycled, (void)error;
        return false;
    }
    virtual bool reveal(const std::string& path) { (void)path; return false; } // in the system's file manager
    virtual bool openExternal(const std::string& url) { (void)url; return false; } // in the browser
    // nullopt where the platform cannot tell.
    virtual std::optional<FileStamp> stamp(const std::string& path) { (void)path; return std::nullopt; }
};

} // namespace enp::interfaces
