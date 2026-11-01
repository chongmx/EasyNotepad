#include "enp/os/Process.h"

#include "enp/os/Fs.h"
#include "enp/os/Os.h"

#include <algorithm>
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
extern char** environ;
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

namespace {

// The program and whatever it starts (cmd.exe -> claude -> node ...) live in one job: ending the job
// ends all of them, and closing it when we are done (or when this app dies) ends any left over.
HANDLE startInJob(PROCESS_INFORMATION& info)
{
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        if (!AssignProcessToJobObject(job, info.hProcess)) {
            CloseHandle(job);
            job = nullptr;
        }
    }
    ResumeThread(info.hThread);
    return job;
}

// Waits for the process, a tenth of a second at a time, so a cancel or the timeout acts at once.
void waitForExit(HANDLE process, HANDLE job, int timeoutSeconds, const CancelFlag& cancel, Result& result)
{
    const ULONGLONG deadline = GetTickCount64() + ULONGLONG(timeoutSeconds) * 1000;
    while (WaitForSingleObject(process, 100) == WAIT_TIMEOUT) {
        const bool stop = cancel && cancel->load();
        if (!stop && GetTickCount64() < deadline)
            continue;
        result.cancelled = stop;
        result.timedOut = !stop;
        if (job)
            TerminateJobObject(job, 1);
        else
            TerminateProcess(process, 1);
        WaitForSingleObject(process, 5000);
        break;
    }
}

} // namespace

Result run(const std::string& commandLine, const std::string& input, const std::string& workingDirectory, int timeoutSeconds,
           const CancelFlag& cancel)
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
    const BOOL ok = CreateProcessW(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
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
    HANDLE job = startInJob(info);
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
    waitForExit(info.hProcess, job, timeoutSeconds, cancel, result);
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    result.exitCode = int(code);
    CloseHandle(info.hProcess);
    if (job)
        CloseHandle(job); // ends anything it left running
    writer.join();
    reader.join();
    CloseHandle(outRead);
    return result;
}

namespace {

// Quotes one argument the way CommandLineToArgvW and the C runtime split it again.
std::wstring quoteArgument(const std::wstring& arg)
{
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; ++i) {
        size_t backslashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            ++i;
            ++backslashes;
        }
        if (i == arg.size()) {
            out.append(backslashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
            out.append(backslashes * 2 + 1, L'\\');
        else
            out.append(backslashes, L'\\');
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

// Our environment with `overrides` replacing (or adding) variables, as a CreateProcess block.
std::wstring environmentBlock(const std::vector<std::pair<std::string, std::string>>& overrides)
{
    std::vector<std::wstring> entries;
    if (wchar_t* env = GetEnvironmentStringsW()) {
        for (const wchar_t* p = env; *p; p += wcslen(p) + 1)
            entries.emplace_back(p);
        FreeEnvironmentStringsW(env);
    }
    for (const auto& [name, value] : overrides) {
        const std::wstring key = widen(name) + L"=";
        entries.erase(std::remove_if(entries.begin(), entries.end(),
                                     [&](const std::wstring& e) { return _wcsnicmp(e.c_str(), key.c_str(), key.size()) == 0; }),
                      entries.end());
        entries.push_back(key + widen(value));
    }
    std::sort(entries.begin(), entries.end(),
              [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    std::wstring block;
    for (const std::wstring& e : entries) {
        block += e;
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

} // namespace

Result run(const Command& command)
{
    Result result;
    if (command.arguments.empty()) {
        result.error = "no program to run";
        return result;
    }
    std::string program = command.arguments[0];
    if (program.find_first_of("/\\") == std::string::npos) {
        program = findExecutable(program);
        if (program.empty()) {
            result.error = command.arguments[0] + " was not found";
            return result;
        }
    }
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE inRead = nullptr, inWrite = nullptr, outRead = nullptr, outWrite = nullptr, errRead = nullptr, errWrite = nullptr;
    if (!CreatePipe(&inRead, &inWrite, &inherit, 0) || !CreatePipe(&outRead, &outWrite, &inherit, 0) ||
        !CreatePipe(&errRead, &errWrite, &inherit, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    // Our ends must not be inherited, or the pipes never report EOF.
    SetHandleInformation(inWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = inRead;
    startup.hStdOutput = outWrite;
    startup.hStdError = errWrite;
    const std::wstring application = widen(program);
    std::wstring line = quoteArgument(application);
    for (size_t i = 1; i < command.arguments.size(); ++i)
        line += L" " + quoteArgument(widen(command.arguments[i]));
    std::wstring environment = environmentBlock(command.environment);
    const std::wstring directory = widen(command.workingDirectory);
    PROCESS_INFORMATION info{};
    const BOOL ok = CreateProcessW(application.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                                   environment.data(), directory.empty() ? nullptr : directory.c_str(), &startup, &info);
    CloseHandle(inRead);
    CloseHandle(outWrite);
    CloseHandle(errWrite);
    if (!ok) {
        result.error = "could not start " + program + " (error " + std::to_string(GetLastError()) + ")";
        CloseHandle(inWrite);
        CloseHandle(outRead);
        CloseHandle(errRead);
        return result;
    }
    result.started = true;
    HANDLE job = startInJob(info);
    CloseHandle(info.hThread);
    std::thread writer([inWrite, input = command.input]() {
        DWORD written = 0;
        size_t done = 0;
        while (done < input.size() && WriteFile(inWrite, input.data() + done, DWORD(input.size() - done), &written, nullptr))
            done += written;
        CloseHandle(inWrite);
    });
    auto drain = [](HANDLE pipe, std::string* into) {
        char buffer[8192];
        DWORD read = 0;
        while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0)
            into->append(buffer, read);
    };
    std::thread outReader([&]() {
        char buffer[8192];
        DWORD read = 0;
        while (ReadFile(outRead, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
            result.output.append(buffer, read);
            if (command.onOutput)
                command.onOutput(buffer, read);
        }
    });
    std::thread errReader(drain, errRead, &result.errorOutput);
    waitForExit(info.hProcess, job, command.timeoutSeconds, command.cancel, result);
    DWORD code = 0;
    GetExitCodeProcess(info.hProcess, &code);
    result.exitCode = int(code);
    CloseHandle(info.hProcess);
    if (job)
        CloseHandle(job); // ends anything it left running
    writer.join();
    outReader.join();
    errReader.join();
    CloseHandle(outRead);
    CloseHandle(errRead);
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

Result run(const std::string&, const std::string&, const std::string&, int, const CancelFlag&)
{
    Result result;
    result.error = "running command-line tools is not supported on this platform";
    return result;
}

std::string findExecutable(const std::string&)
{
    return {};
}

Result run(const Command&)
{
    Result result;
    result.error = "running command-line tools is not supported on this platform";
    return result;
}

#else

Result run(const std::string& commandLine, const std::string& input, const std::string& workingDirectory, int timeoutSeconds,
           const CancelFlag& cancel)
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
        for (int fd : {in[0], in[1], out[0], out[1]})
            close(fd);
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
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
        if (cancel && cancel->load()) {
            result.cancelled = true;
            kill(-pid, SIGKILL);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            result.timedOut = true;
            kill(-pid, SIGKILL);
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

Result run(const Command& command)
{
    Result result;
    if (command.arguments.empty()) {
        result.error = "no program to run";
        return result;
    }
    // Everything the child needs is prepared before fork(): allocating afterwards is unsafe.
    std::string program = command.arguments[0];
    if (program.find('/') == std::string::npos) {
        program = findExecutable(program);
        if (program.empty()) {
            result.error = command.arguments[0] + " was not found";
            return result;
        }
    }
    std::vector<char*> argv;
    for (const std::string& a : command.arguments)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> envStrings;
    for (char** e = environ; e && *e; ++e) {
        const std::string entry = *e;
        const bool replaced = std::any_of(command.environment.begin(), command.environment.end(),
                                          [&](const auto& kv) { return entry.rfind(kv.first + "=", 0) == 0; });
        if (!replaced)
            envStrings.push_back(entry);
    }
    for (const auto& [name, value] : command.environment)
        envStrings.push_back(name + "=" + value);
    std::vector<char*> envp;
    for (std::string& e : envStrings)
        envp.push_back(e.data());
    envp.push_back(nullptr);

    int in[2], out[2], err[2];
    if (pipe(in) != 0 || pipe(out) != 0 || pipe(err) != 0) {
        result.error = "pipe failed";
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]})
            close(fd);
        return result;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(err[1], 2);
        for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]})
            close(fd);
        if (!command.workingDirectory.empty() && chdir(command.workingDirectory.c_str()) != 0)
            _exit(126);
        execve(program.c_str(), argv.data(), envp.data());
        _exit(127);
    }
    result.started = true;
    close(in[0]);
    close(out[1]);
    close(err[1]);
    signal(SIGPIPE, SIG_IGN);
    std::thread writer([fd = in[1], input = command.input]() {
        size_t done = 0;
        while (done < input.size()) {
            const ssize_t n = write(fd, input.data() + done, input.size() - done);
            if (n <= 0)
                break;
            done += size_t(n);
        }
        close(fd);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(command.timeoutSeconds);
    char buffer[8192];
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    int open = 2;
    while (open > 0) {
        if (poll(fds, 2, 200) > 0) {
            for (int k = 0; k < 2; ++k) {
                if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                    continue;
                const ssize_t n = read(fds[k].fd, buffer, sizeof(buffer));
                if (n <= 0) {
                    close(fds[k].fd);
                    fds[k].fd = -1;
                    --open;
                } else {
                    (k == 0 ? result.output : result.errorOutput).append(buffer, size_t(n));
                    if (k == 0 && command.onOutput)
                        command.onOutput(buffer, size_t(n));
                }
            }
        }
        if (command.cancel && command.cancel->load()) {
            result.cancelled = true;
            kill(-pid, SIGKILL);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            result.timedOut = true;
            kill(-pid, SIGKILL);
            break;
        }
    }
    for (pollfd& f : fds)
        if (f.fd >= 0)
            close(f.fd);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    writer.join();
    if (result.exitCode == 126)
        result.error = "could not enter " + command.workingDirectory;
    else if (result.exitCode == 127 && result.output.empty() && result.errorOutput.empty())
        result.error = "could not start " + program;
    return result;
}

#endif

} // namespace enp::os::process
