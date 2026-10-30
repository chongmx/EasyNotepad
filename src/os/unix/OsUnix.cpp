// Linux and macOS desktop backend.
#include "../OsInternal.h"

#include <climits>
#include <cstdio>
#include <cstdlib>

#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(ENP_OS_MACOS)
#   include <mach-o/dyld.h>
#endif

extern char** environ;

namespace enp::os {

namespace {

bool commandExists(const std::string& name)
{
    std::string pathVar = getEnv("PATH").value_or("/usr/bin:/bin");
    size_t start = 0;
    while (start <= pathVar.size()) {
        size_t end = pathVar.find(':', start);
        if (end == std::string::npos)
            end = pathVar.size();
        std::string candidate = pathVar.substr(start, end - start) + "/" + name;
        if (::access(candidate.c_str(), X_OK) == 0)
            return true;
        start = end + 1;
    }
    return false;
}

// Runs a shell command and returns its trimmed stdout, or nullopt on failure / empty output.
std::optional<std::string> runCapture(const std::string& command)
{
    FILE* pipe = ::popen(command.c_str(), "r");
    if (!pipe)
        return std::nullopt;
    std::string out;
    char buf[1024];
    while (size_t n = std::fread(buf, 1, sizeof(buf), pipe))
        out.append(buf, n);
    int status = ::pclose(pipe);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    if (status != 0 || out.empty())
        return std::nullopt;
    return out;
}

} // namespace

namespace detail {

bool platformInit(const InitParams& params, std::vector<std::string>& outArguments)
{
    for (int i = 1; i < params.argc; ++i)
        outArguments.emplace_back(params.argv[i]);
    return true;
}

void platformShutdown() {}

void writeLogSink(log::Level level, const char* message)
{
    static const char* const prefixes[] = {"[debug] ", "[info] ", "[warn] ", "[error] "};
    std::fprintf(stderr, "%s%s\n", prefixes[int(level)], message);
}

} // namespace detail

namespace fs {

std::string executableDirectory()
{
#if defined(ENP_OS_MACOS)
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        char resolved[PATH_MAX];
        if (::realpath(buf, resolved))
            return path::parent(resolved);
        return path::parent(buf);
    }
#else
    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return path::parent(buf);
    }
#endif
    return currentDirectory();
}

std::string appDataDirectory()
{
#if defined(ENP_OS_MACOS)
    std::string dir = path::join(homeDirectory(), "Library/Application Support/EasyNotepad");
#else
    std::string base = getEnv("XDG_CONFIG_HOME").value_or(path::join(homeDirectory(), ".config"));
    std::string dir = path::join(base, "easynotepad");
#endif
    createDirectories(dir);
    return dir;
}

std::vector<std::string> rootDirectories()
{
#if defined(ENP_OS_MACOS)
    return {homeDirectory(), "/", "/Volumes"};
#else
    return {homeDirectory(), "/", "/media", "/mnt"};
#endif
}

std::optional<std::string> readAsset(const std::string& relativePath)
{
    const std::string exe = executableDirectory();
    for (const std::string& base : {path::join(exe, "resources"), path::join(exe, "../resources"),
                                    path::join(exe, "../Resources/resources"), // macOS .app bundle
                                    path::join(exe, "../share/easynotepad/resources"), // `make install` layout
                                    path::join(currentDirectory(), "resources")}) {
        if (auto data = readFile(path::join(base, relativePath)))
            return data;
    }
    return std::nullopt;
}

} // namespace fs

namespace dialog {

bool hasNativeOpenFile()
{
#if defined(ENP_OS_MACOS)
    return true;
#else
    static const bool available = commandExists("zenity") || commandExists("kdialog");
    return available;
#endif
}

void openFile(const std::string& title, const std::vector<FileFilter>& filters, OpenFileCallback callback)
{
    std::string command;
#if defined(ENP_OS_MACOS)
    std::string script = "POSIX path of (choose file with prompt \"" + title + "\")";
    command = "osascript -e " + detail::shellQuote(script) + " 2>/dev/null";
    (void)filters;
#else
    if (commandExists("zenity")) {
        command = "zenity --file-selection --title=" + detail::shellQuote(title);
        for (const FileFilter& f : filters) {
            std::string spec = f.name + " |";
            for (const std::string& ext : f.extensions)
                spec += " *." + ext;
            command += " --file-filter=" + detail::shellQuote(spec);
        }
        command += " --file-filter='All files | *'";
    } else {
        std::string spec;
        for (const FileFilter& f : filters)
            for (const std::string& ext : f.extensions)
                spec += (spec.empty() ? "*." : " *.") + ext;
        command = "kdialog --title " + detail::shellQuote(title) + " --getopenfilename " +
                  detail::shellQuote(fs::homeDirectory()) + " " + detail::shellQuote(spec);
    }
    command += " 2>/dev/null";
#endif
    // The picker blocks until closed, so run it off the UI thread.
    dispatch::runAsync([command, callback = std::move(callback)]() mutable {
        std::optional<std::string> result = runCapture(command);
        dispatch::postToMain([callback = std::move(callback), result]() { callback(result); });
    });
}

} // namespace dialog

namespace shell {

bool openExternal(const std::string& urlOrPath)
{
#if defined(ENP_OS_MACOS)
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    char* argv[] = {const_cast<char*>(opener), const_cast<char*>(urlOrPath.c_str()), nullptr};
    pid_t pid = 0;
    if (::posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) != 0)
        return false;
    // Reap the launcher asynchronously; xdg-open/open exit quickly.
    dispatch::runAsync([pid]() {
        int status = 0;
        ::waitpid(pid, &status, 0);
    });
    return true;
}

} // namespace shell

} // namespace enp::os
