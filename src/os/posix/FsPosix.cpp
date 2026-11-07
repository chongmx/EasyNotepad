// POSIX file-system backend shared by Linux, macOS, Android and the web (Emscripten MEMFS).
#include "../OsInternal.h"

#include <cstdio>
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/mman.h>
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

std::optional<std::string> readFileRange(const std::string& path, std::uint64_t offset, size_t size, std::uint64_t* total)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return std::nullopt;
    struct stat st{};
    if (::fstat(fd, &st) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    const std::uint64_t length = std::uint64_t(st.st_size);
    if (total)
        *total = length;
    std::string data;
    if (offset < length) {
        data.resize(size_t(std::min<std::uint64_t>(size, length - offset)));
        size_t got = 0;
        while (got < data.size()) {
            const ssize_t n = ::pread(fd, data.data() + got, data.size() - got, off_t(offset + got));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                break;
            got += size_t(n);
        }
        data.resize(got);
    }
    ::close(fd);
    return data;
}

bool readFileInto(const std::string& path, std::string& out, size_t maxBytes, std::string* error)
{
    int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        if (error)
            *error = errnoMessage();
        return false;
    }
    struct stat st{};
    if (::fstat(fd, &st) != 0 || st.st_size < 0 || size_t(st.st_size) > maxBytes) {
        if (error)
            *error = "too large";
        ::close(fd);
        return false;
    }
    out.resize(size_t(st.st_size)); // within the buffer's reserved room: no allocation
    size_t got = 0;
    while (got < out.size()) {
        ssize_t n = ::read(fd, &out[got], out.size() - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            if (error)
                *error = errnoMessage();
            ::close(fd);
            out.clear();
            return false;
        }
        if (n == 0)
            break; // shorter than it was a moment ago
        got += size_t(n);
    }
    out.resize(got);
    ::close(fd);
    return true;
}

bool MappedFile::open(const std::string& path)
{
    close();
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return false;
    struct stat st {};
    if (::fstat(fd, &st) == 0 && st.st_size > 0) {
        void* p = ::mmap(nullptr, size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
        if (p != MAP_FAILED) {
            data_ = p;
            size_ = size_t(st.st_size);
        }
    }
    ::close(fd); // the mapping stays without it
    return data_ != nullptr;
}

void MappedFile::close()
{
    if (data_)
        ::munmap(data_, size_);
    data_ = nullptr;
    size_ = 0;
}

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
    return listDirectory(p, 0, nullptr, error);
}

std::vector<DirEntry> listDirectory(const std::string& p, size_t limit, bool* more, std::string* error)
{
    if (more)
        *more = false;
    std::vector<DirEntry> entries;
    DIR* dir = ::opendir(p.c_str());
    if (!dir) {
        if (error)
            *error = errnoMessage();
        return entries;
    }
    while (dirent* d = ::readdir(dir)) {
        std::string name = d->d_name;
        if (name == "." || name == "..")
            continue;
        if (limit > 0 && entries.size() >= limit) {
            if (more)
                *more = true;
            break;
        }
        // The directory itself usually says what an entry is: a stat() per entry made listing a
        // large folder slow. Links and unknown types are still resolved (a dangling link is skipped).
        DirEntry e{name, false, name[0] == '.', 0};
#if defined(DT_DIR) && defined(DT_REG)
        if (d->d_type == DT_DIR)
            e.isDirectory = true;
        else if (d->d_type != DT_REG)
#endif
        {
            FileInfo info = stat(path::join(p, name));
            if (!info.exists)
                continue;
            e.isDirectory = info.isDirectory;
            e.size = info.size;
        }
        entries.push_back(std::move(e));
    }
    ::closedir(dir);
    std::sort(entries.begin(), entries.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDirectory != b.isDirectory)
            return a.isDirectory;
        return lessCaseInsensitive(a.name, b.name);
    });
    return entries;
}

bool renamePath(const std::string& from, const std::string& to, std::string* error)
{
    struct stat st{};
    if (::lstat(to.c_str(), &st) == 0) {
        if (error)
            *error = "already exists";
        return false;
    }
    if (::rename(from.c_str(), to.c_str()) != 0) {
        if (error)
            *error = std::strerror(errno);
        return false;
    }
    return true;
}

bool deletePath(const std::string& path, bool* recycled, std::string* error)
{
    if (recycled)
        *recycled = false;
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) {
        if (error)
            *error = std::strerror(errno);
        return false;
    }
    // A folder's contents first; a link to a folder is removed itself, never followed.
    if (S_ISDIR(st.st_mode)) {
        if (DIR* dir = ::opendir(path.c_str())) {
            std::vector<std::string> names;
            while (dirent* e = ::readdir(dir))
                if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0)
                    names.emplace_back(e->d_name);
            ::closedir(dir);
            for (const std::string& n : names)
                if (!deletePath(path + "/" + n, nullptr, error))
                    return false;
        }
        if (::rmdir(path.c_str()) != 0) {
            if (error)
                *error = std::strerror(errno);
            return false;
        }
        return true;
    }
    if (::unlink(path.c_str()) != 0) {
        if (error)
            *error = std::strerror(errno);
        return false;
    }
    return true;
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
