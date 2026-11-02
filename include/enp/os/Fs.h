// File-system access. All paths are UTF-8; '/' is accepted as separator on every platform.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
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
std::string homeDirectory();
// Per-user writable directory for settings (created on demand).
std::string appDataDirectory();
// Starting points for a file browser: drive letters on Windows, "/" and well-known folders elsewhere.
std::vector<std::string> rootDirectories();

// Read-only resources bundled with the application (fonts, sample documents).
// `relativePath` uses '/' separators, e.g. "fonts/Roboto-Medium.ttf".
std::optional<std::string> readAsset(const std::string& relativePath);
// Part of a file or of a bundled resource: up to `size` bytes from `offset` (fewer at the end),
// so a large pack is never read whole; `*total` gets the whole size. On Android the asset must be
// stored uncompressed in the APK (android/app/build.gradle: noCompress).
std::optional<std::string> readFileRange(const std::string& path, std::uint64_t offset, size_t size, std::uint64_t* total = nullptr);
std::optional<std::string> readAssetRange(const std::string& relativePath, std::uint64_t offset, size_t size,
                                          std::uint64_t* total = nullptr);

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
