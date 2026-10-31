// Loopback TCP sockets (127.0.0.1 only) for the automation server and the MCP bridge.
// Blocking calls: use them from background threads.
#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace enp::os::net {

class Socket {
public:
    virtual ~Socket() = default; // closes the socket
    virtual bool sendAll(std::string_view data) = 0;
    // Reads up to and excluding the next '\n'; false on EOF or error.
    virtual bool readLine(std::string& line) = 0;
    // Waits up to `milliseconds` for data (or a pending connection on a listener).
    virtual bool waitReadable(int milliseconds) = 0;
    // Ends the connection; a blocked readLine() on another thread returns false.
    virtual void shutdown() = 0;
};

// Listens on 127.0.0.1; `port` 0 picks a free port, which is written back.
std::unique_ptr<Socket> listenLoopback(int& port, std::string* error = nullptr);
// Accepts the next connection on a listening socket (use waitReadable() to poll); null on error.
std::unique_ptr<Socket> accept(Socket& listener);
std::unique_ptr<Socket> connectLoopback(int port, std::string* error = nullptr);

} // namespace enp::os::net
