// Local automation endpoint behind the MCP connector. Listens on 127.0.0.1 (random port) for
// newline-delimited JSON requests {"token", "id", "method", "params"}, runs each on the main
// thread through the handler, and answers {"id", "result"} or {"id", "error"}.
// The port and a per-session token are written to endpointPath() for `EasyNotepad --mcp`.
#pragma once

#include "enp/model/Json.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace enp::os::net {
class Socket;
}

namespace enp::services {

class AutomationServer {
public:
    // Runs on the main thread; throw-free; return {"error": "..."} to report a failure.
    using Handler = std::function<model::Json(const std::string& method, const model::Json& params)>;

    explicit AutomationServer(Handler handler);
    ~AutomationServer();
    AutomationServer(const AutomationServer&) = delete;
    AutomationServer& operator=(const AutomationServer&) = delete;

    // Starts listening and publishes the endpoint file. Safe to call once.
    bool start(std::string* error = nullptr);
    void stop();
    int port() const { return port_; }
    const std::string& token() const { return token_; }

    // Where the running app publishes {"port", "token", "pid"}; $EASYNOTEPAD_ENDPOINT overrides it.
    static std::string endpointPath();

private:
    void acceptLoop();
    void serve(std::shared_ptr<os::net::Socket> connection);

    Handler handler_;
    std::unique_ptr<os::net::Socket> listener_;
    std::thread acceptThread_;
    std::mutex connectionsMutex_;
    std::vector<std::shared_ptr<os::net::Socket>> connections_;
    std::vector<std::thread> connectionThreads_;
    std::atomic<bool> stopping_{false};
    int port_ = 0;
    std::string token_;
};

} // namespace enp::services
