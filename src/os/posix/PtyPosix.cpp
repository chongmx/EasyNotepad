// forkpty() backend for Linux and macOS.
#include "enp/os/Pty.h"

#include "enp/os/Dispatch.h"
#include "enp/os/Fs.h"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>

#if defined(__APPLE__)
#   include <util.h>
#else
#   include <pty.h>
#endif

namespace enp::os::pty {

namespace {

winsize windowSize(int columns, int rows)
{
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(columns > 0 ? columns : 80);
    ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
    return ws;
}

// The reader (a pool thread, kept: see dispatch::runAsync) forwards output until the master side
// reports EOF/EIO (all slaves closed), then reaps the child and reports the exit code.
class ForkPtyProcess final : public Process {
public:
    ~ForkPtyProcess() override
    {
        stopping_ = true;
        terminate();
        reader_.wait();
        if (master_ >= 0)
            close(master_);
    }

    bool start(const Options& options, OutputCallback onOutput, ExitCallback onExit, std::string* error)
    {
        // Everything the child needs is prepared before fork(): allocating afterwards is unsafe in a
        // multi-threaded process.
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>(options.command.c_str()));
        for (const std::string& argument : options.arguments)
            argv.push_back(const_cast<char*>(argument.c_str()));
        argv.push_back(nullptr);
        winsize ws = windowSize(options.columns, options.rows);
        const pid_t pid = forkpty(&master_, nullptr, nullptr, &ws);
        if (pid < 0) {
            if (error)
                *error = std::string("forkpty failed: ") + std::strerror(errno);
            return false;
        }
        if (pid == 0) { // child
            if (!options.workingDirectory.empty() && chdir(options.workingDirectory.c_str()) != 0) {
                // keep the inherited directory
            }
            setenv("TERM", "xterm-256color", 1);
            setenv("COLORTERM", "truecolor", 1);
            setenv("TERM_PROGRAM", "EasyNotepad", 1);
            execvp(argv[0], argv.data());
            execl("/bin/sh", "/bin/sh", static_cast<char*>(nullptr));
            _exit(127);
        }
        pid_ = pid;
        reader_.run([this, onOutput = std::move(onOutput), onExit = std::move(onExit)]() {
            char buffer[8192];
            for (;;) {
                pollfd pfd{master_, POLLIN, 0};
                const int ready = poll(&pfd, 1, 100);
                if (stopping_)
                    break;
                if (ready < 0 && errno == EINTR)
                    continue;
                if (ready == 0)
                    continue;
                const ssize_t n = read(master_, buffer, sizeof(buffer));
                if (n > 0) {
                    onOutput(std::string(buffer, size_t(n)));
                    continue;
                }
                if (n < 0 && errno == EINTR)
                    continue;
                break; // EOF or EIO: the shell and its children closed the terminal
            }
            int status = 0;
            waitpid(pid_, &status, 0);
            pid_ = -1;
            if (!stopping_)
                onExit(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
        });
        return true;
    }

    bool write(std::string_view data) override
    {
        size_t done = 0;
        while (done < data.size()) {
            const ssize_t n = ::write(master_, data.data() + done, data.size() - done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                return false;
            done += size_t(n);
        }
        return true;
    }

    void resize(int columns, int rows) override
    {
        winsize ws = windowSize(columns, rows);
        ioctl(master_, TIOCSWINSZ, &ws);
    }

    void terminate() override
    {
        if (pid_ > 0)
            kill(pid_, SIGHUP);
    }

private:
    int master_ = -1;
    std::atomic<pid_t> pid_{-1};
    std::atomic<bool> stopping_{false};
    dispatch::Group reader_;
};

} // namespace

bool supported()
{
    return true;
}

std::vector<Shell> availableShells()
{
    std::vector<Shell> shells;
    auto add = [&](const std::string& name, const std::string& path) {
        if (path.empty() || !fs::exists(path))
            return;
        for (const Shell& s : shells)
            if (s.command == path)
                return;
        shells.push_back({name, path});
    };
    if (const char* user = std::getenv("SHELL")) {
        const std::string path = user;
        add(path.substr(path.rfind('/') + 1), path);
    }
    add("zsh", "/bin/zsh");
    add("bash", "/bin/bash");
    add("fish", "/usr/bin/fish");
    add("sh", "/bin/sh");
    return shells;
}

std::unique_ptr<Process> spawn(const Options& options, OutputCallback onOutput, ExitCallback onExit, std::string* error)
{
    Options resolved = options;
    if (resolved.command.empty()) {
        const std::vector<Shell> shells = availableShells();
        resolved.command = shells.empty() ? "/bin/sh" : shells.front().command;
    }
    auto process = std::make_unique<ForkPtyProcess>();
    if (!process->start(resolved, std::move(onOutput), std::move(onExit), error))
        return nullptr;
    return process;
}

} // namespace enp::os::pty
