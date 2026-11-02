#include "enp/services/AgentCliService.h"

#include "enp/model/Json.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Os.h"

#include <algorithm>
#include <cctype>
#include <chrono>

namespace enp::services {

namespace {

constexpr int kTimeoutSeconds = 15 * 60;       // a whole-project dissection can take a while
constexpr int kStreamTimeoutSeconds = 60 * 60; // a conversation turn may edit and test for long

#if defined(_WIN32)
constexpr const char* kExe = ".exe";
#else
constexpr const char* kExe = "";
#endif

double nowSeconds()
{
    return double(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch()).count()) / 1000.0;
}

// The first word of a command line, without quotes.
std::string programOf(const std::string& command)
{
    if (command.size() > 1 && command.front() == '"')
        return command.substr(1, command.find('"', 1) - 1);
    return command.substr(0, command.find(' '));
}

// "2.1.99" < "2.1.100": numbers compare as numbers.
bool versionLess(const std::string& a, const std::string& b)
{
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            size_t ei = i, ej = j;
            while (ei < a.size() && std::isdigit(static_cast<unsigned char>(a[ei])))
                ++ei;
            while (ej < b.size() && std::isdigit(static_cast<unsigned char>(b[ej])))
                ++ej;
            const unsigned long long x = std::stoull(a.substr(i, std::min<size_t>(ei - i, 18)));
            const unsigned long long y = std::stoull(b.substr(j, std::min<size_t>(ej - j, 18)));
            if (x != y)
                return x < y;
            i = ei;
            j = ej;
        } else {
            if (a[i] != b[j])
                return a[i] < b[j];
            ++i;
            ++j;
        }
    }
    return a.size() - i < b.size() - j;
}

// Claude Code's MCP configuration for this app: its own executable in connector mode, which talks
// to the running window.
std::string writeMcpConfig()
{
    const std::string self = os::fs::path::join(os::fs::executableDirectory(), std::string("EasyNotepad") + kExe);
    const model::Json config = model::Json::object().set(
        "mcpServers", model::Json::object().set("easynotepad", model::Json::object()
                                                                   .set("command", self)
                                                                   .set("args", model::Json::array().push("--mcp"))));
    const std::string path = os::fs::path::join(os::fs::appDataDirectory(), "agent-mcp.json");
    os::fs::writeFile(path, config.dump());
    return path;
}

} // namespace

AgentCliService::AgentCliService()
{
    setCommand({});
}

void AgentCliService::setCommand(const std::string& command)
{
    std::string chosen = command;
    while (!chosen.empty() && (chosen.back() == ' ' || chosen.back() == '\t'))
        chosen.pop_back();
    if (chosen.empty()) {
        const std::optional<std::string> configured = os::getEnv("EASYNOTEPAD_AGENT_CLI");
        chosen = configured && !configured->empty() ? *configured : "claude -p";
    }
    if (chosen == command_ && checkedAt_ > -1.0e9)
        return;
    command_ = chosen;
    std::string name = os::fs::path::filename(programOf(command_));
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    claudeCommand_ = name == "claude" || name == "claude.exe" || name == "claude.cmd";
    recheck();
}

void AgentCliService::recheck()
{
    ++checkGeneration_;
    checking_ = false;
    checkedAt_ = -1.0e9;
    executable_.clear();
    available(); // starts the search
}

AgentCliService::~AgentCliService()
{
    *alive_ = false; // results arriving later are dropped
    cancel();        // and the programs do not outlive the app
}

std::string AgentCliService::findClaude()
{
    std::string found = os::process::findExecutable("claude");
    if (!found.empty())
        return found;
    const std::string home = os::fs::homeDirectory();
    for (const char* folder : {".local/bin", ".claude/local"}) {
        const std::string candidate = os::fs::path::join(os::fs::path::join(home, folder), std::string("claude") + kExe);
        if (os::fs::exists(candidate))
            return candidate;
    }
    // The copy that ships inside the VS Code extension (and its forks): the newest version.
    std::string bestVersion;
    for (const char* folder : {".vscode/extensions", ".vscode-insiders/extensions", ".cursor/extensions"}) {
        const std::string extensions = os::fs::path::join(home, folder);
        for (const os::fs::DirEntry& entry : os::fs::listDirectory(extensions)) {
            const std::string prefix = "anthropic.claude-code-";
            if (!entry.isDirectory || entry.hidden || entry.name.compare(0, prefix.size(), prefix) != 0)
                continue;
            const std::string candidate = os::fs::path::join(
                os::fs::path::join(extensions, entry.name), std::string("resources/native-binary/claude") + kExe);
            const std::string version = entry.name.substr(prefix.size());
            if ((found.empty() || versionLess(bestVersion, version)) && os::fs::exists(candidate)) {
                found = candidate;
                bestVersion = version;
            }
        }
    }
    return found;
}

std::string AgentCliService::command() const
{
    return command_;
}

bool AgentCliService::available() const
{
    if (nowSeconds() - checkedAt_ < 30.0 || checking_)
        return available_;
    checking_ = true;
    const int generation = checkGeneration_;
    const std::string program = programOf(command_);
    const bool claude = claudeCommand_;
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    os::dispatch::runAsync([this, alive, program, claude, generation]() {
        std::string executable = claude ? findClaude() : os::process::findExecutable(program);
        const bool found = !executable.empty() || os::fs::exists(program);
        os::dispatch::postToMain([this, alive, found, executable, claude, generation]() {
            if (auto token = alive.lock(); !token || !*token)
                return;
            if (generation != checkGeneration_)
                return; // the command changed meanwhile
            available_ = found;
            // Claude Code runs from where it was found (it may be off PATH); other commands as written.
            executable_ = executable;
            checking_ = false;
            checkedAt_ = nowSeconds();
        });
    });
    return available_;
}

os::process::CancelFlag AgentCliService::track()
{
    os::process::CancelFlag flag = std::make_shared<std::atomic<bool>>(false);
    running_.push_back(flag);
    return flag;
}

void AgentCliService::untrack(const os::process::CancelFlag& flag)
{
    running_.erase(std::remove(running_.begin(), running_.end(), flag), running_.end());
}

void AgentCliService::cancel()
{
    for (const os::process::CancelFlag& flag : running_)
        *flag = true;
}

void AgentCliService::run(const std::string& prompt, const std::string& workingDirectory,
                          std::function<void(bool ok, const std::string& output)> done)
{
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    std::string command = command_;
    if (claudeCommand_ && !executable_.empty()) // Claude Code, perhaps found outside PATH
        command = "\"" + executable_ + "\"" + (command_.find(' ') == std::string::npos ? "" : command_.substr(command_.find(' ')));
    os::process::CancelFlag flag = track();
    os::dispatch::runAsync([this, alive, flag, command, prompt, workingDirectory, done = std::move(done)]() {
        const os::process::Result result = os::process::run(command, prompt, workingDirectory, kTimeoutSeconds, flag);
        const bool ok = result.started && !result.timedOut && !result.cancelled && result.exitCode == 0;
        std::string text = result.output;
        if (!result.started)
            text = result.error;
        else if (result.cancelled)
            text = "stopped";
        else if (result.timedOut)
            text = "the agent did not finish within " + std::to_string(kTimeoutSeconds / 60) + " minutes";
        else if (result.exitCode != 0)
            text = "the agent exited with code " + std::to_string(result.exitCode) + ": " + result.output.substr(0, 400);
        os::dispatch::postToMain([this, alive, flag, ok, text, done]() {
            if (auto token = alive.lock(); !token || !*token)
                return;
            untrack(flag);
            done(ok, text);
        });
    });
}

void AgentCliService::stream(const interfaces::AgentStreamRequest& request, std::function<void(const std::string& line)> onLine,
                             std::function<void(bool ok, const std::string& error)> done)
{
    os::process::Command command;
    command.arguments = {claudeCommand_ && !executable_.empty() ? executable_ : programOf(command_),
                         "-p", "--output-format", "stream-json", "--verbose", "--include-partial-messages",
                         "--permission-mode", request.planOnly ? "plan" : "acceptEdits"};
    if (!request.model.empty())
        command.arguments.insert(command.arguments.end(), {"--model", request.model});
    if (!request.systemPrompt.empty())
        command.arguments.insert(command.arguments.end(), {"--append-system-prompt", request.systemPrompt});
    if (!request.sessionId.empty())
        command.arguments.insert(command.arguments.end(), {"--resume", request.sessionId});
    if (request.appControl)
        command.arguments.insert(command.arguments.end(), {"--mcp-config", writeMcpConfig(), "--allowedTools", "mcp__easynotepad"});
    command.input = request.prompt;
    command.workingDirectory = request.workingDirectory;
    command.timeoutSeconds = kStreamTimeoutSeconds;
    command.cancel = track();

    // Whole lines go to the main thread as they arrive (the reader thread is the only writer).
    std::weak_ptr<std::atomic<bool>> alive = alive_;
    auto pending = std::make_shared<std::string>();
    command.onOutput = [alive, pending, onLine](const char* data, size_t size) {
        pending->append(data, size);
        for (size_t end; (end = pending->find('\n')) != std::string::npos;) {
            std::string line = pending->substr(0, end);
            pending->erase(0, end + 1);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            if (line.empty())
                continue;
            os::dispatch::postToMain([alive, onLine, line = std::move(line)]() {
                if (auto token = alive.lock(); token && *token)
                    onLine(line);
            });
        }
    };
    os::dispatch::runAsync([this, alive, command = std::move(command), done = std::move(done)]() {
        const os::process::Result result = os::process::run(command);
        const bool ok = result.started && !result.timedOut && !result.cancelled && result.exitCode == 0;
        std::string error;
        if (!result.started)
            error = result.error;
        else if (result.cancelled)
            error = "stopped";
        else if (result.timedOut)
            error = "the agent did not finish within " + std::to_string(kStreamTimeoutSeconds / 60) + " minutes";
        else if (result.exitCode != 0)
            error = "the agent exited with code " + std::to_string(result.exitCode) +
                    (result.errorOutput.empty() ? std::string() : ": " + result.errorOutput.substr(0, 400));
        os::dispatch::postToMain([this, alive, flag = command.cancel, ok, error, done]() {
            if (auto token = alive.lock(); !token || !*token)
                return;
            untrack(flag);
            done(ok, error);
        });
    });
}

} // namespace enp::services
