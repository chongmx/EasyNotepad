#include "enp/services/TerminalService.h"

#include "enp/os/Os.h"
#include "enp/os/Pty.h"

#include <atomic>

namespace enp::services {

namespace {

class TerminalSession final : public interfaces::ITerminalSession {
public:
    // Callbacks posted by the reader threads check this token, so nothing arrives after the
    // session (and the view-model that owns it) is gone.
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    std::unique_ptr<os::pty::Process> process;

    ~TerminalSession() override
    {
        *alive = false;
        process.reset(); // joins the reader threads
    }
    void write(std::string_view bytes) override { process->write(bytes); }
    void resize(int columns, int rows) override { process->resize(columns, rows); }
    void terminate() override { process->terminate(); }
};

} // namespace

TerminalService::TerminalService() : supported_(os::pty::supported())
{
    for (const os::pty::Shell& shell : os::pty::availableShells())
        shells_.push_back({shell.name, shell.command});
}

std::unique_ptr<interfaces::ITerminalSession> TerminalService::start(const interfaces::ShellProfile& shell,
                                                                     const std::string& workingDirectory, int columns,
                                                                     int rows, Callbacks callbacks, std::string* error)
{
    auto session = std::make_unique<TerminalSession>();
    std::weak_ptr<std::atomic<bool>> alive = session->alive;
    auto output = std::make_shared<std::function<void(std::string_view)>>(std::move(callbacks.output));
    auto exited = std::make_shared<std::function<void(int)>>(std::move(callbacks.exited));

    os::pty::Options options;
    options.command = shell.command;
    // The requested folder (the open workspace); if there is none, or it has gone, the home
    // directory; the app's own directory only when even that is missing.
    options.workingDirectory = workingDirectory;
    if (options.workingDirectory.empty() || !os::fs::isDirectory(options.workingDirectory))
        options.workingDirectory = os::fs::homeDirectory();
    if (options.workingDirectory.empty() || !os::fs::isDirectory(options.workingDirectory))
        options.workingDirectory = os::fs::currentDirectory();
    options.columns = columns;
    options.rows = rows;
    session->process = os::pty::spawn(
        options,
        [alive, output](std::string data) {
            os::dispatch::postToMain([alive, output, data = std::move(data)]() {
                if (auto token = alive.lock(); token && *token)
                    (*output)(data);
            });
        },
        [alive, exited](int code) {
            os::dispatch::postToMain([alive, exited, code]() {
                if (auto token = alive.lock(); token && *token)
                    (*exited)(code);
            });
        },
        error);
    if (!session->process)
        return nullptr;
    return session;
}

} // namespace enp::services
