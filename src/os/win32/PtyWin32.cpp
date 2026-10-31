// ConPTY backend. The API is resolved at run time so the app still starts on Windows versions
// without it (pty::supported() then returns false).
#include "enp/os/Pty.h"

#include "enp/os/Fs.h"

#ifndef NOMINMAX
#   define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <thread>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#   define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace enp::os::pty {

namespace {

using PseudoConsole = void*;
using CreatePseudoConsoleFn = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, PseudoConsole*);
using ResizePseudoConsoleFn = HRESULT(WINAPI*)(PseudoConsole, COORD);
using ClosePseudoConsoleFn = void(WINAPI*)(PseudoConsole);

struct ConPtyApi {
    CreatePseudoConsoleFn create = nullptr;
    ResizePseudoConsoleFn resize = nullptr;
    ClosePseudoConsoleFn close = nullptr;
};

const ConPtyApi& api()
{
    static const ConPtyApi instance = [] {
        ConPtyApi a;
        if (HMODULE kernel = GetModuleHandleW(L"kernel32.dll")) {
            a.create = reinterpret_cast<CreatePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "CreatePseudoConsole")));
            a.resize = reinterpret_cast<ResizePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "ResizePseudoConsole")));
            a.close = reinterpret_cast<ClosePseudoConsoleFn>(reinterpret_cast<void*>(GetProcAddress(kernel, "ClosePseudoConsole")));
        }
        return a;
    }();
    return instance;
}

std::wstring widen(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::string narrow(const std::wstring& s)
{
    if (s.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string lastErrorText(const char* what)
{
    return std::string(what) + " failed (error " + std::to_string(GetLastError()) + ")";
}

std::string findOnPath(const wchar_t* exe)
{
    wchar_t buffer[MAX_PATH];
    const DWORD n = SearchPathW(nullptr, exe, nullptr, MAX_PATH, buffer, nullptr);
    return n > 0 && n < MAX_PATH ? narrow(buffer) : std::string();
}

COORD coord(int columns, int rows)
{
    return COORD{SHORT(columns > 0 ? columns : 80), SHORT(rows > 0 ? rows : 24)};
}

// Threads: `reader_` forwards output until the pipe closes; `waiter_` waits for the process to exit,
// closes the pseudo console (which ends the output pipe once drained), joins the reader and reports
// the exit. The pseudo console is closed exactly once, by whoever exchanges `console_` first.
class ConPtyProcess final : public Process {
public:
    ~ConPtyProcess() override
    {
        terminate();
        if (waiter_.joinable())
            waiter_.join();
        closeConsole();
        if (reader_.joinable())
            reader_.join();
        closeHandle(input_);
        closeHandle(output_);
        closeHandle(process_);
        closeHandle(waitCancel_);
    }

    bool start(const Options& options, OutputCallback onOutput, ExitCallback onExit, std::string* error)
    {
        HANDLE ptyIn = nullptr, ptyOut = nullptr;
        if (!CreatePipe(&ptyIn, &input_, nullptr, 0) || !CreatePipe(&output_, &ptyOut, nullptr, 0)) {
            if (error)
                *error = lastErrorText("CreatePipe");
            return false;
        }
        PseudoConsole console = nullptr;
        const HRESULT hr = api().create(coord(options.columns, options.rows), ptyIn, ptyOut, 0, &console);
        console_ = console;
        // The pseudo console duplicated its ends; ours must be closed so EOF propagates.
        CloseHandle(ptyIn);
        CloseHandle(ptyOut);
        if (FAILED(hr)) {
            if (error)
                *error = "CreatePseudoConsole failed (HRESULT " + std::to_string(long(hr)) + ")";
            return false;
        }

        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        std::string attributeBuffer(attributeSize, '\0');
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        // Without this, a child would inherit our standard handles when they are redirected
        // (pipes, files) and write there instead of to the pseudo console.
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeBuffer.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeSize) ||
            !UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console,
                                       sizeof(console), nullptr, nullptr)) {
            if (error)
                *error = lastErrorText("UpdateProcThreadAttribute");
            return false;
        }

        std::string line = options.command;
        for (const std::string& argument : options.arguments)
            line += argument.find_first_of(" \t\"") == std::string::npos ? " " + argument : " \"" + argument + "\"";
        std::wstring commandLine = widen(line);
        const std::wstring directory = widen(options.workingDirectory);
        PROCESS_INFORMATION info{};
        const BOOL created = CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                                           EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                           directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo, &info);
        DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (!created) {
            if (error)
                *error = lastErrorText(("Starting \"" + options.command + "\"").c_str());
            return false;
        }
        CloseHandle(info.hThread);
        process_ = info.hProcess;
        waitCancel_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

        reader_ = std::thread([this, onOutput = std::move(onOutput)]() {
            char buffer[8192];
            DWORD read = 0;
            while (ReadFile(output_, buffer, sizeof(buffer), &read, nullptr) && read > 0)
                onOutput(std::string(buffer, read));
        });
        waiter_ = std::thread([this, onExit = std::move(onExit)]() {
            HANDLE handles[] = {process_, waitCancel_};
            if (WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0)
                return; // cancelled by the destructor
            DWORD code = 0;
            GetExitCodeProcess(process_, &code);
            closeConsole(); // the reader ends after the remaining output
            reader_.join();
            onExit(int(code));
        });
        return true;
    }

    bool write(std::string_view data) override
    {
        DWORD written = 0;
        return WriteFile(input_, data.data(), DWORD(data.size()), &written, nullptr) && written == data.size();
    }

    void resize(int columns, int rows) override
    {
        if (PseudoConsole c = console_.load())
            api().resize(c, coord(columns, rows));
    }

    void terminate() override
    {
        if (waitCancel_)
            SetEvent(waitCancel_);
        if (process_)
            TerminateProcess(process_, 1);
    }

private:
    void closeConsole()
    {
        if (PseudoConsole c = console_.exchange(nullptr))
            api().close(c);
    }

    static void closeHandle(HANDLE& h)
    {
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
        h = nullptr;
    }

    std::atomic<PseudoConsole> console_{nullptr};
    HANDLE input_ = nullptr;  // we write keystrokes here
    HANDLE output_ = nullptr; // we read terminal output here
    HANDLE process_ = nullptr;
    HANDLE waitCancel_ = nullptr;
    std::thread reader_;
    std::thread waiter_;
};

} // namespace

bool supported()
{
    const ConPtyApi& a = api();
    return a.create && a.resize && a.close;
}

std::vector<Shell> availableShells()
{
    std::vector<Shell> shells;
    if (std::string pwsh = findOnPath(L"pwsh.exe"); !pwsh.empty())
        shells.push_back({"PowerShell 7", "\"" + pwsh + "\" -NoLogo"});
    if (std::string ps = findOnPath(L"powershell.exe"); !ps.empty())
        shells.push_back({"PowerShell", "\"" + ps + "\" -NoLogo"});
    wchar_t comspec[MAX_PATH];
    if (GetEnvironmentVariableW(L"ComSpec", comspec, MAX_PATH) > 0)
        shells.push_back({"Command Prompt", "\"" + narrow(comspec) + "\""});
    for (const char* bash : {"C:/Program Files/Git/bin/bash.exe", "C:/Program Files (x86)/Git/bin/bash.exe"})
        if (fs::exists(bash)) {
            shells.push_back({"Git Bash", std::string("\"") + bash + "\" --login -i"});
            break;
        }
    if (std::string wsl = findOnPath(L"wsl.exe"); !wsl.empty())
        shells.push_back({"WSL", "\"" + wsl + "\""});
    if (shells.empty())
        shells.push_back({"Command Prompt", "cmd.exe"});
    return shells;
}

std::unique_ptr<Process> spawn(const Options& options, OutputCallback onOutput, ExitCallback onExit, std::string* error)
{
    if (!supported()) {
        if (error)
            *error = "This version of Windows has no pseudo console (ConPTY needs Windows 10 1809 or later)";
        return nullptr;
    }
    Options resolved = options;
    if (resolved.command.empty())
        resolved.command = availableShells().front().command;
    auto process = std::make_unique<ConPtyProcess>();
    if (!process->start(resolved, std::move(onOutput), std::move(onExit), error))
        return nullptr;
    return process;
}

} // namespace enp::os::pty
