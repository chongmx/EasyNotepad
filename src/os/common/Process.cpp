#include "enp/os/Process.h"

#include "enp/os/Fs.h"
#include "enp/os/Os.h"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#elif !defined(__EMSCRIPTEN__)
#   include <csignal>
#   include <fcntl.h>
#   include <poll.h>
#   include <sys/wait.h>
#   include <unistd.h>
#endif

namespace enp::os::process {

#if defined(_WIN32)

namespace {

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

} // namespace

Result run(const std::string& commandLine, const std::string& input, const std::string& workingDirectory, int timeoutSeconds)
{
    Result result;
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &inherit, 0) || !CreatePipe(&outRead, &outWrite, &inherit, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    // Our ends must not be inherited, or the pipes never report EOF.
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inRead;
    startup.hStdOutput = outWrite;
    startup.hStdError = outWrite;
    std::wstring line = L"cmd.exe /d /s /c \"" + widen(commandLine) + L"\"";
    const std::wstring directory = widen(workingDirectory);
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                                   nullptr, directory.empty() ? nullptr : directory.c_str(), &startup, &info);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    if (!ok) {
        result.error = "could not start: " + commandLine + " (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(inWrite);
        CloseHandle(outRead);
        return result;
    }
    result.started = true;
    CloseHandle(info.hThread);

    // Feed stdin on its own thread so a chatty child cannot deadlock against a full pipe.
    std::thread writer([inWrite, input]() {
        DWORD written = 0;
        size_t done = 0;
        while (done < input.size() && WriteFile(inWrite, input.data() + done, DWORD(input.size() - done), &written, nullptr))
            done += written;
        CloseHandle(inWrite);
    });
    std::thread reader([outRead, &result]() {
        char buffer[8192];
        DWORD read = 0;
        while (ReadFile(outRead, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            result.output.append(buffer, read);
    });
    if (WaitForSingleObject(info.hProcess, DWORD(timeoutSeconds) * 1000) == WAIT_TIMEOUT) {
        result.timedOut = true;
        TerminateProcess(info.hProcess, 1);
        WaitForSingleObject(info.hProcess, 5000);
    }
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    result.exitCode = int(code);
    CloseHandle(info.hProcess);
    writer.join();
    reader.join();
    CloseHandle(outRead);
    return result;
}

std::string findExecutable(const std::string& name)
{
    for (const char* ext : {".exe", ".cmd", ".bat", ""}) {
        wchar_t buffer[MAX_PATH];
        const std::wstring file = widen(name + ext);
        const DWORD n = SearchPathW(nullptr, file.c_str(), nullptr, MAX_PATH, buffer, nullptr);
        if (n > 0 && n < MAX_PATH) {
            const int size = WideCharToMultiByte(CP_UTF8, 0, buffer, int(n), nullptr, 0, nullptr, nullptr);
            std::string out(size_t(size), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, int(n), out.data(), size, nullptr, nullptr);
            return out;
        }
    }
    return {};
}

#elif defined(__EMSCRIPTEN__) || defined(__ANDROID__)

Result run(const std::string&, const std::string&, const std::string&, int)
{
    Result result;
    result.error = "running command-line tools is not supported on this platform";
    return result;
}

std::string findExecutable(const std::string&)
{
    return {};
}

#else

Result run(const std::string& commandLine, const std::string& input, const std::string& workingDirectory, int timeoutSeconds)
{
    Result result;
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0) {
        result.error = "pipe failed";
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        return result;
    }
    if (pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]);
        close(in[1]);
        close(out[0]);
        close(out[1]);
        if (!workingDirectory.empty() && chdir(workingDirectory.c_str()) != 0) {
        }
        execl("/bin/sh", "sh", "-c", commandLine.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    result.started = true;
    close(in[0]);
    close(out[1]);
    signal(SIGPIPE, SIG_IGN);
    std::thread writer([fd = in[1], input]() {
        size_t done = 0;
        while (done < input.size()) {
            const ssize_t n = write(fd, input.data() + done, input.size() - done);
            if (n <= 0)
                break;
            done += size_t(n);
        }
        close(fd);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    char buffer[8192];
    for (;;) {
        pollfd pfd{out[0], POLLIN, 0};
        if (poll(&pfd, 1, 200) > 0) {
            const ssize_t n = read(out[0], buffer, sizeof(buffer));
            if (n <= 0)
                break;
            result.output.append(buffer, size_t(n));
        }
        if (std::chrono::steady_clock::now() > deadline) {
            result.timedOut = true;
            kill(pid, SIGKILL);
            break;
        }
    }
    close(out[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    writer.join();
    return result;
}

std::string findExecutable(const std::string& name)
{
    const std::optional<std::string> path = getEnv("PATH");
    if (!path)
        return {};
    size_t start = 0;
    while (start <= path->size()) {
        const size_t end = path->find(':', start);
        const std::string dir = path->substr(start, end == std::string::npos ? std::string::npos : end - start);
        const std::string candidate = fs::path::join(dir.empty() ? "." : dir, name);
        if (access(candidate.c_str(), X_OK) == 0)
            return candidate;
        if (end == std::string::npos)
            break;
        start = end + 1;
    }
    return {};
}

#endif

} // namespace enp::os::process
