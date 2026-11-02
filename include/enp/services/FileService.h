// IFileService on the OS layer's file system API.
#pragma once

#include "enp/interfaces/IFileService.h"

namespace enp::services {

class FileService final : public interfaces::IFileService {
public:
    std::optional<std::string> read(const std::string& path, std::string* error = nullptr) override;
    bool readInto(const std::string& path, std::string& out, size_t maxBytes, std::string* error = nullptr) override;
    bool write(const std::string& path, std::string_view data, std::string* error = nullptr) override;
    std::optional<std::string> readRange(const std::string& path, std::uint64_t offset, size_t size, std::uint64_t* total = nullptr) override;
    std::vector<interfaces::DirectoryEntry> list(const std::string& path) override;
    std::vector<interfaces::DirectoryEntry> list(const std::string& path, size_t limit, bool* more) override;
    bool isDirectory(const std::string& path) override;
    std::string absolute(const std::string& path) override;
    bool createDirectories(const std::string& path) override;
    std::string homeDirectory() override;
    std::vector<std::string> roots() override;
    bool rename(const std::string& from, const std::string& to, std::string* error = nullptr) override;
    bool remove(const std::string& path, bool* recycled = nullptr, std::string* error = nullptr) override;
    bool reveal(const std::string& path) override;
    bool openExternal(const std::string& url) override;
    std::optional<interfaces::FileStamp> stamp(const std::string& path) override;
};

} // namespace enp::services
