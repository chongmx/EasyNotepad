// Contract for reading and writing workspace files.
#pragma once

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
};

class IFileService {
public:
    virtual ~IFileService() = default;
    virtual std::optional<std::string> read(const std::string& path, std::string* error = nullptr) = 0;
    virtual bool write(const std::string& path, std::string_view data, std::string* error = nullptr) = 0;
    // Directories first, then files, each sorted by name.
    virtual std::vector<DirectoryEntry> list(const std::string& path) = 0;
    virtual bool isDirectory(const std::string& path) = 0;
    virtual std::string absolute(const std::string& path) = 0;
    virtual bool createDirectories(const std::string& path) = 0;
    virtual std::string homeDirectory() { return {}; }
    // nullopt where the platform cannot tell.
    virtual std::optional<FileStamp> stamp(const std::string& path) { (void)path; return std::nullopt; }
};

} // namespace enp::interfaces
