#include "enp/services/FileService.h"

#include "enp/os/Shell.h"

#include "enp/os/Fs.h"

namespace enp::services {

std::optional<std::string> FileService::read(const std::string& path, std::string* error)
{
    return os::fs::readFile(path, error);
}

std::optional<std::string> FileService::readRange(const std::string& path, std::uint64_t offset, size_t size, std::uint64_t* total)
{
    return os::fs::readFileRange(path, offset, size, total);
}

bool FileService::write(const std::string& path, std::string_view data, std::string* error)
{
    return os::fs::writeFile(path, data, error);
}

std::vector<interfaces::DirectoryEntry> FileService::list(const std::string& path)
{
    return list(path, 0, nullptr);
}

std::vector<interfaces::DirectoryEntry> FileService::list(const std::string& path, size_t limit, bool* more)
{
    std::vector<interfaces::DirectoryEntry> out;
    for (const os::fs::DirEntry& e : os::fs::listDirectory(path, limit, more))
        out.push_back({e.name, e.isDirectory, e.hidden});
    return out;
}

bool FileService::isDirectory(const std::string& path)
{
    return os::fs::isDirectory(path);
}

std::string FileService::absolute(const std::string& path)
{
    const std::string normalized = os::fs::path::normalize(path);
    return os::fs::path::isAbsolute(normalized) ? normalized : os::fs::path::join(os::fs::currentDirectory(), normalized);
}

bool FileService::createDirectories(const std::string& path)
{
    return os::fs::createDirectories(path);
}

bool FileService::rename(const std::string& from, const std::string& to, std::string* error)
{
    return os::fs::renamePath(from, to, error);
}

bool FileService::remove(const std::string& path, bool* recycled, std::string* error)
{
    return os::fs::deletePath(path, recycled, error);
}

bool FileService::openExternal(const std::string& url)
{
    return os::shell::openExternal(url);
}

bool FileService::reveal(const std::string& path)
{
    return os::shell::revealInFileManager(path);
}

std::vector<std::string> FileService::roots()
{
    return os::fs::rootDirectories();
}

std::string FileService::homeDirectory()
{
    return os::fs::homeDirectory();
}

std::optional<interfaces::FileStamp> FileService::stamp(const std::string& path)
{
    const os::fs::FileInfo info = os::fs::stat(path);
    return interfaces::FileStamp{info.exists && !info.isDirectory, info.size, info.modifiedTime};
}

} // namespace enp::services
