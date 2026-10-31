// IFileService on the OS layer's file system API.
#pragma once

#include "enp/interfaces/IFileService.h"

namespace enp::services {

class FileService final : public interfaces::IFileService {
public:
    std::optional<std::string> read(const std::string& path, std::string* error = nullptr) override;
    bool write(const std::string& path, std::string_view data, std::string* error = nullptr) override;
    std::vector<interfaces::DirectoryEntry> list(const std::string& path) override;
    bool isDirectory(const std::string& path) override;
    std::string absolute(const std::string& path) override;
    bool createDirectories(const std::string& path) override;
    std::string homeDirectory() override;
    std::optional<interfaces::FileStamp> stamp(const std::string& path) override;
};

} // namespace enp::services
