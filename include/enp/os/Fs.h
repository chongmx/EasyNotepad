// File-system access. All paths are UTF-8; '/' is accepted as separator on every platform.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace enp::os::fs {

struct FileInfo {
    bool exists = false;
    bool isDirectory = false;
    std::uint64_t size = 0;
    std::int64_t modifiedTime = 0; // seconds since the Unix epoch
};

struct DirEntry {
    std::string name;
    bool isDirectory = false;
    // A dot name (everywhere), or the hidden or system attribute (Windows). Listed all the same:
    // whether to show it is the caller's choice.
    bool hidden = false;
    std::uint64_t size = 0;
};

// ---- Native file access (implemented per platform) ----------------------------
std::optional<std::string> readFile(const std::string& path, std::string* error = nullptr);
// Reads a whole file into `out`, reusing its storage (a buffer reserved once serves every file of
// a scan); false, and `out` untouched, when it cannot be read or is larger than `maxBytes`.
bool readFileInto(const std::string& path, std::string& out, size_t maxBytes, std::string* error = nullptr);
bool writeFile(const std::string& path, std::string_view data, std::string* error = nullptr);
FileInfo stat(const std::string& path);
// Directories first, then files; both sorted case-insensitively. Excludes "." and ".."; hidden
// entries are included and flagged. `size` is 0 where the directory listing does not give it.
std::vector<DirEntry> listDirectory(const std::string& path, std::string* error = nullptr);
// Stops after `limit` entries (0: all), so a folder of a million files is not read whole; `*more`
// reports whether the folder holds more. Which entries come first is up to the file system.
std::vector<DirEntry> listDirectory(const std::string& path, size_t limit, bool* more, std::string* error = nullptr);
bool createDirectories(const std::string& path);
// Renames or moves a file or folder; fails if `to` exists.
bool renamePath(const std::string& from, const std::string& to, std::string* error = nullptr);
// Deletes a file, or a folder with everything in it. On Windows it goes to the Recycle Bin
// (`*recycled` is set); elsewhere it is gone.
bool deletePath(const std::string& path, bool* recycled = nullptr, std::string* error = nullptr);

std::string currentDirectory();
std::string executableDirectory();
// Android: the shared storage ("/storage/emulated/0") once the app may read it, else the app's own folder.
std::string homeDirectory();
// Per-user writable directory for settings (created on demand).
std::string appDataDirectory();
// Starting points for a file browser: drive letters on Windows, "/" and well-known folders elsewhere
// (Android: the shared storage, its Download and Documents folders, SD cards, the app's own folders).
std::vector<std::string> rootDirectories();

// Whether the app may read and write files outside its own folders. Android 11 and later: the
// "All files access" the user turns on in the system settings; Android 6 to 10: the storage
// permission. True on every other platform.
bool hasStorageAccess();
// Asks the user for that access and returns at once: Android opens the app's "All files access"
// page in the system settings (or the permission prompt before Android 11). Nothing elsewhere.
void requestStorageAccess();

// Read-only resources bundled with the application (fonts, sample documents).
// `relativePath` uses '/' separators, e.g. "fonts/Roboto-Medium.ttf".
std::optional<std::string> readAsset(const std::string& relativePath);
// Part of a file or of a bundled resource: up to `size` bytes from `offset` (fewer at the end),
// so a large pack is never read whole; `*total` gets the whole size. On Android the asset must be
// stored uncompressed in the APK (android/app/build.gradle: noCompress).
std::optional<std::string> readFileRange(const std::string& path, std::uint64_t offset, size_t size, std::uint64_t* total = nullptr);
std::optional<std::string> readAssetRange(const std::string& relativePath, std::uint64_t offset, size_t size,
                                          std::uint64_t* total = nullptr);

// A file mapped read-only into memory: its bytes are the system's file cache, not the heap, and
// go back the moment it is closed (a large file read in place, such as a font). Moves, never copies.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile() { close(); }
    MappedFile(MappedFile&& other) noexcept { *this = std::move(other); }
    MappedFile& operator=(MappedFile&& other) noexcept;
    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    bool open(const std::string& path); // false (and closed) when it cannot be mapped or is empty
    void close();
    bool isOpen() const { return data_ != nullptr; }
    const void* data() const { return data_; }
    std::size_t size() const { return size_; }

private:
    void* data_ = nullptr;
    std::size_t size_ = 0;
};

inline MappedFile& MappedFile::operator=(MappedFile&& other) noexcept
{
    if (this != &other) {
        close();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

inline bool exists(const std::string& path) { return stat(path).exists; }
inline bool isDirectory(const std::string& path) { return stat(path).isDirectory; }

// ---- Pure path helpers (platform independent) ----------------------------------
namespace path {
std::string normalize(std::string_view p);           // '\\' -> '/', collapses "." / ".." / "//"
std::string join(std::string_view a, std::string_view b);
std::string parent(std::string_view p);
std::string filename(std::string_view p);
std::string extension(std::string_view p);           // lower-case, including the dot: ".md"
bool isAbsolute(std::string_view p);
} // namespace path

} // namespace enp::os::fs
