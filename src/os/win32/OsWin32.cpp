// Windows backend: Win32 wide-char APIs, UTF-8 at the boundary.
#include "../OsInternal.h"

#ifndef WIN32_LEAN_AND_MEAN
#   define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cctype>
#include <iterator>

namespace enp::os {

namespace {

std::wstring widen(std::string_view s)
{
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string narrow(std::wstring_view s)
{
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring nativePath(const std::string& path)
{
    std::wstring w = widen(path);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return w;
}

std::string lastErrorMessage()
{
    DWORD code = GetLastError();
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string msg = buffer ? narrow(buffer) : "error " + std::to_string(code);
    if (buffer)
        LocalFree(buffer);
    while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == ' '))
        msg.pop_back();
    return msg;
}

std::int64_t fileTimeToUnix(const FILETIME& ft)
{
    ULARGE_INTEGER v;
    v.LowPart = ft.dwLowDateTime;
    v.HighPart = ft.dwHighDateTime;
    return std::int64_t(v.QuadPart / 10000000ULL) - 11644473600LL;
}

bool lessCaseInsensitive(const std::string& a, const std::string& b)
{
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
        return std::tolower((unsigned char)x) < std::tolower((unsigned char)y);
    });
}

} // namespace

// ---- Process ------------------------------------------------------------------

std::optional<std::string> getEnv(const std::string& name)
{
    std::wstring wname = widen(name);
    DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0)
        return std::nullopt;
    std::wstring value(n, L'\0');
    n = GetEnvironmentVariableW(wname.c_str(), value.data(), n);
    value.resize(n);
    return narrow(value);
}

namespace detail {

bool platformInit(const InitParams&, std::vector<std::string>& outArguments)
{
    SetConsoleOutputCP(CP_UTF8);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i)
        outArguments.push_back(narrow(argv[i]));
    if (argv)
        LocalFree(argv);
    return true;
}

void platformShutdown() {}

void writeLogSink(log::Level level, const char* message)
{
    static const char* const prefixes[] = {"[debug] ", "[info] ", "[warn] ", "[error] "};
    std::string line = std::string(prefixes[int(level)]) + message + "\n";
    OutputDebugStringW(widen(line).c_str());
    std::fputs(line.c_str(), stderr);
}


} // namespace detail

// ---- File system ----------------------------------------------------------------

namespace fs {

std::optional<std::string> readFile(const std::string& path, std::string* error)
{
    HANDLE h = CreateFileW(nativePath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = lastErrorMessage();
        return std::nullopt;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string data(size_t(size.QuadPart), '\0');
    size_t total = 0;
    while (total < data.size()) {
        DWORD chunk = DWORD(std::min<size_t>(data.size() - total, 1u << 30));
        DWORD read = 0;
        if (!ReadFile(h, data.data() + total, chunk, &read, nullptr) || read == 0)
            break;
        total += read;
    }
    CloseHandle(h);
    data.resize(total);
    return data;
}

bool writeFile(const std::string& path, std::string_view data, std::string* error)
{
    HANDLE h = CreateFileW(nativePath(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = lastErrorMessage();
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, data.data(), DWORD(data.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok && error)
        *error = lastErrorMessage();
    return ok && written == data.size();
}

FileInfo stat(const std::string& path)
{
    FileInfo info;
    WIN32_FILE_ATTRIBUTE_DATA attr;
    if (!GetFileAttributesExW(nativePath(path).c_str(), GetFileExInfoStandard, &attr))
        return info;
    info.exists = true;
    info.isDirectory = (attr.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info.size = (std::uint64_t(attr.nFileSizeHigh) << 32) | attr.nFileSizeLow;
    info.modifiedTime = fileTimeToUnix(attr.ftLastWriteTime);
    return info;
}

std::vector<DirEntry> listDirectory(const std::string& path, std::string* error)
{
    std::vector<DirEntry> entries;
    std::wstring pattern = nativePath(path);
    if (!pattern.empty() && pattern.back() != L'\\')
        pattern += L'\\';
    pattern += L'*';

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        if (error)
            *error = lastErrorMessage();
        return entries;
    }
    do {
        std::wstring_view name = fd.cFileName;
        if (name == L"." || name == L"..")
            continue;
        if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))
            continue;
        DirEntry e;
        e.name = narrow(name);
        e.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.size = (std::uint64_t(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        entries.push_back(std::move(e));
    } while (FindNextFileW(h, &fd));
    FindClose(h);

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
    if (n.empty() || isDirectory(n))
        return true;
    std::string par = path::parent(n);
    if (par != n && !createDirectories(par))
        return false;
    return CreateDirectoryW(nativePath(n).c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

std::string currentDirectory()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetCurrentDirectoryW(DWORD(std::size(buf)), buf);
    return path::normalize(narrow(std::wstring_view(buf, n)));
}

std::string executableDirectory()
{
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return path::parent(narrow(std::wstring_view(buf, n)));
}

std::string homeDirectory()
{
    if (auto home = getEnv("USERPROFILE"))
        return path::normalize(*home);
    return currentDirectory();
}

std::string appDataDirectory()
{
    std::string base = getEnv("APPDATA").value_or(homeDirectory());
    std::string dir = path::join(base, "EasyNotepad");
    createDirectories(dir);
    return dir;
}

std::vector<std::string> rootDirectories()
{
    std::vector<std::string> roots{homeDirectory()};
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i)
        if (mask & (1u << i))
            roots.push_back(std::string(1, char('A' + i)) + ":/");
    return roots;
}

std::optional<std::string> readAsset(const std::string& relativePath)
{
    const std::string exe = executableDirectory();
    for (const std::string& base : {path::join(exe, "resources"), path::join(exe, "../resources"),
                                    path::join(currentDirectory(), "resources")}) {
        if (auto data = readFile(path::join(base, relativePath)))
            return data;
    }
    return std::nullopt;
}

} // namespace fs

// ---- Dialogs ---------------------------------------------------------------------

namespace dialog {

bool hasNativeOpenFile() { return true; }

void openFile(const std::string& title, const std::vector<FileFilter>& filters, OpenFileCallback callback)
{
    // Filter format: "Name\0*.a;*.b\0...\0\0"
    std::wstring filter;
    for (const FileFilter& f : filters) {
        std::wstring patterns;
        for (const std::string& ext : f.extensions)
            patterns += (patterns.empty() ? L"*." : L";*.") + widen(ext);
        filter += widen(f.name) + L" (" + patterns + L")" + L'\0' + patterns + L'\0';
    }
    filter += L"All files (*.*)";
    filter += L'\0';
    filter += L"*.*";
    filter += L'\0';
    filter += L'\0';

    wchar_t file[MAX_PATH * 4] = {};
    std::wstring wtitle = widen(title);
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = GetActiveWindow();
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = DWORD(std::size(file));
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;

    std::optional<std::string> result;
    if (GetOpenFileNameW(&ofn))
        result = fs::path::normalize(narrow(file));
    dispatch::postToMain([callback = std::move(callback), result]() { callback(result); });
}

} // namespace dialog

// ---- Shell -------------------------------------------------------------------------

namespace shell {

bool openExternal(const std::string& urlOrPath)
{
    std::wstring target = detail::isUrl(urlOrPath) ? widen(urlOrPath) : nativePath(urlOrPath);
    auto rc = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return rc > 32;
}

} // namespace shell

} // namespace enp::os
