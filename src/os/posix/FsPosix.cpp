// POSIX file-system backend shared by Linux, macOS, Android and the web (Emscripten MEMFS).
#include "../OsInternal.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace enp::os {

std::optional<std::string> getEnv(const std::string& name)
{
    const char* v = std::getenv(name.c_str());
    if (!v)
        return std::nullopt;
    return std::string(v);
}

namespace fs {

namespace {

std::string errnoMessage() { return std::strerror(errno); }

bool lessCaseInsensitive(const std::string& a, const std::string& b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
    });
}

} // namespace

std::optional<std::string> readFile(const std::string& path, std::string* error)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error)
            *error = errnoMessage();
        return std::nullopt;
    }
    std::string data;
    struct stat st{};
    if (::fstat(fd, &st) == 0 && st.st_size > 0)
        data.reserve(size_t(st.st_size));
    char buffer[64 * 1024];
    for (;;) {
        ssize_t n = ::read(fd, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            if (error)
                *error = errnoMessage();
            ::close(fd);
            return std::nullopt;
        }
        if (n == 0)
            break;
        data.append(buffer, size_t(n));
    }
    ::close(fd);
    return data;
}

bool writeFile(const std::string& path, std::string_view data, std::string* error)
{
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        if (error)
            *error = errnoMessage();
        return false;
    }
    size_t total = 0;
    while (total < data.size()) {
        ssize_t n = ::write(fd, data.data() + total, data.size() - total);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (error)
                *error = errnoMessage();
            ::close(fd);
            return false;
        }
        total += size_t(n);
    }
    ::close(fd);
    return true;
}

FileInfo stat(const std::string& path)
{
    FileInfo info;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
        return info;
    info.exists = true;
    info.isDirectory = S_ISDIR(st.st_mode);
    info.size = std::uint64_t(st.st_size);
    info.modifiedTime = std::int64_t(st.st_mtime);
    return info;
}

std::vector<DirEntry> listDirectory(const std::string& p, std::string* error)
{
    std::vector<DirEntry> entries;
    DIR* dir = ::opendir(p.c_str());
    if (!dir) {
        if (error)
            *error = errnoMessage();
        return entries;
    }
    while (dirent* d = ::readdir(dir)) {
        std::string name = d->d_name;
        if (name == "." || name == ".." || name[0] == '.')
            continue;
        FileInfo info = stat(path::join(p, name));
        if (!info.exists)
            continue;
        entries.push_back({name, info.isDirectory, info.size});
    }
    ::closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDirectory != b.isDirectory)
            return a.isDirectory;
        return lessCaseInsensitive(a.name, b.name);
    });
    return entries;
}

bool createDirectories(const std::string& p)
{
    std::string n = path::normalize(p);
    if (n.empty() || n == "/" || isDirectory(n))
        return true;
    std::string par = path::parent(n);
    if (par != n && !createDirectories(par))
        return false;
    return ::mkdir(n.c_str(), 0755) == 0 || errno == EEXIST;
}

std::string currentDirectory()
{
    char buf[4096];
    if (::getcwd(buf, sizeof(buf)))
        return buf;
    return "/";
}

std::string homeDirectory()
{
    if (auto home = getEnv("HOME"))
        return *home;
#if !defined(ENP_OS_WEB)
    if (passwd* pw = ::getpwuid(::getuid()))
        return pw->pw_dir;
#endif
    return "/";
}

} // namespace fs

} // namespace enp::os
