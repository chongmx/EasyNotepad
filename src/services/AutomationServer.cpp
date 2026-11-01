#include "enp/services/AutomationServer.h"

#include "enp/os/Os.h"
#include "enp/os/Process.h"
#include "enp/os/Socket.h"

#include <chrono>
#include <future>
#include <random>

#if defined(_WIN32)
#   include <process.h>
#   define ENP_GETPID _getpid
#else
#   include <unistd.h>
#   define ENP_GETPID getpid
#endif

namespace enp::services {

namespace {

constexpr int kRequestTimeoutMs = 10000;

std::string randomToken()
{
    std::random_device device;
    std::mt19937_64 rng((uint64_t(device()) << 32) ^ device());
    static const char hex[] = "0123456789abcdef";
    std::string token;
    for (int i = 0; i < 32; ++i)
        token += hex[rng() & 15];
    return token;
}

model::Json errorReply(const model::Json& id, const std::string& message)
{
    return model::Json::object().set("id", id).set("error", message);
}

} // namespace

AutomationServer::AutomationServer(Handler handler) : handler_(std::move(handler)), token_(randomToken()) {}

AutomationServer::~AutomationServer()
{
    stop();
}

std::string AutomationServer::endpointPath()
{
    // Overridable so tests (or several side-by-side instances) do not clash.
    if (std::optional<std::string> path = os::getEnv("EASYNOTEPAD_ENDPOINT"); path && !path->empty())
        return *path;
    return os::fs::path::join(os::fs::appDataDirectory(), "mcp-endpoint.json");
}

bool AutomationServer::start(std::string* error)
{
    if (listener_)
        return true;
    int port = 0;
    listener_ = os::net::listenLoopback(port, error);
    if (!listener_)
        return false;
    port_ = port;
    const model::Json endpoint = model::Json::object()
                                     .set("port", port_)
                                     .set("token", token_)
                                     .set("pid", double(ENP_GETPID()))
                                     .set("app", "EasyNotepad");
    std::string writeError;
    if (!os::fs::writeFile(endpointPath(), endpoint.dump() + "\n", &writeError)) {
        if (error)
            *error = "cannot write " + endpointPath() + ": " + writeError;
        listener_.reset();
        return false;
    }
    acceptThread_ = std::thread([this]() { acceptLoop(); });
    return true;
}

void AutomationServer::stop()
{
    if (!listener_)
        return;
    stopping_ = true;
    if (acceptThread_.joinable())
        acceptThread_.join();
    {
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        for (auto& c : connections_)
            c->shutdown();
    }
    for (std::thread& t : connectionThreads_)
        if (t.joinable())
            t.join();
    connectionThreads_.clear();
    connections_.clear();
    listener_.reset();
    // Only remove the endpoint file if it is still ours (another instance may have taken over).
    if (std::optional<std::string> text = os::fs::readFile(endpointPath()))
        if (auto json = model::Json::parse(*text); json && json->getString("token") == token_)
            os::fs::writeFile(endpointPath(), "");
}

void AutomationServer::acceptLoop()
{
    while (!stopping_) {
        if (!listener_->waitReadable(200))
            continue;
        std::shared_ptr<os::net::Socket> connection = os::net::accept(*listener_);
        if (!connection)
            continue;
        std::lock_guard<std::mutex> lock(connectionsMutex_);
        connections_.push_back(connection);
        connectionThreads_.emplace_back([this, connection]() { serve(connection); });
    }
}

namespace {

// A handler that needs a program's output answers {"__process": {arguments, directory, key}}: the
// program runs here, on the connection's thread, so a slow one never stalls the UI. Its stdout
// becomes {key: output}.
model::Json runDeferred(const model::Json& spec)
{
    os::process::Command command;
    if (const model::Json* arguments = spec.find("arguments"); arguments && arguments->isArray())
        for (const model::Json& argument : arguments->items())
            command.arguments.push_back(argument.isString() ? argument.asString() : std::string());
    command.workingDirectory = spec.getString("directory");
    command.timeoutSeconds = 30;
    const os::process::Result r = os::process::run(command);
    if (!r.started || r.timedOut || r.exitCode != 0)
        return model::Json::object().set("error", r.timedOut          ? std::string("timed out")
                                                  : !r.errorOutput.empty() ? r.errorOutput
                                                  : !r.error.empty()       ? r.error
                                                                           : std::string("failed"));
    const size_t limit = size_t(spec.getNumber("limit", 200000));
    return model::Json::object().set(spec.getString("key", "output"),
                                     r.output.size() > limit ? r.output.substr(0, limit) + "\n[cut]" : r.output);
}

} // namespace

void AutomationServer::serve(std::shared_ptr<os::net::Socket> connection)
{
    std::string line;
    while (!stopping_ && connection->readLine(line)) {
        if (line.empty())
            continue;
        std::string parseError;
        const std::optional<model::Json> request = model::Json::parse(line, &parseError);
        model::Json reply;
        if (!request || !request->isObject()) {
            reply = errorReply(nullptr, "invalid JSON: " + parseError);
        } else if (request->getString("token") != token_) {
            reply = errorReply(request->find("id") ? *request->find("id") : model::Json(), "invalid token");
        } else {
            const model::Json id = request->find("id") ? *request->find("id") : model::Json();
            const std::string method = request->getString("method");
            const model::Json params = request->find("params") ? *request->find("params") : model::Json::object();
            // Run on the UI thread, where the view-models and the UI registry live.
            auto promise = std::make_shared<std::promise<model::Json>>();
            std::future<model::Json> future = promise->get_future();
            os::dispatch::postToMain([this, promise, method, params]() { promise->set_value(handler_(method, params)); });
            int waited = 0;
            while (future.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready && !stopping_ &&
                   (waited += 50) < kRequestTimeoutMs) {
            }
            if (future.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
                reply = errorReply(id, "the application did not respond in time");
            } else {
                model::Json result = future.get();
                if (const model::Json* deferred = result.find("__process"); deferred && deferred->isObject())
                    result = runDeferred(*deferred);
                if (const model::Json* err = result.find("error"); err && err->isString())
                    reply = errorReply(id, err->asString());
                else
                    reply = model::Json::object().set("id", id).set("result", std::move(result));
            }
        }
        if (!connection->sendAll(reply.dump() + "\n"))
            break;
    }
}

} // namespace enp::services
