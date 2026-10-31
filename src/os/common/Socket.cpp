// BSD sockets / Winsock implementation of the loopback socket API.
#include "enp/os/Socket.h"

#if defined(_WIN32)
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <winsock2.h>
#   include <ws2tcpip.h>
using NativeSocket = SOCKET;
#   define ENP_INVALID_SOCKET INVALID_SOCKET
#   define ENP_CLOSE_SOCKET closesocket
#   define ENP_SHUT_BOTH SD_BOTH
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <netinet/tcp.h>
#   include <sys/select.h>
#   include <sys/socket.h>
#   include <unistd.h>
#   include <cerrno>
using NativeSocket = int;
#   define ENP_INVALID_SOCKET (-1)
#   define ENP_CLOSE_SOCKET ::close
#   define ENP_SHUT_BOTH SHUT_RDWR
#endif

#include <algorithm>
#include <cstring>

namespace enp::os::net {

namespace {

bool initialize()
{
#if defined(_WIN32)
    static const bool ok = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

std::string lastError(const char* what)
{
#if defined(_WIN32)
    return std::string(what) + " failed (error " + std::to_string(WSAGetLastError()) + ")";
#else
    return std::string(what) + " failed: " + std::strerror(errno);
#endif
}

// Writing to a closed peer must fail, not raise SIGPIPE: MSG_NOSIGNAL on Linux, SO_NOSIGPIPE on macOS.
#if defined(MSG_NOSIGNAL)
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

void noSigPipe(NativeSocket s)
{
#if defined(SO_NOSIGPIPE)
    int on = 1;
    ::setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)s;
#endif
}

class TcpSocket final : public Socket {
public:
    explicit TcpSocket(NativeSocket s) : socket_(s) {}
    ~TcpSocket() override
    {
        if (socket_ != ENP_INVALID_SOCKET)
            ENP_CLOSE_SOCKET(socket_);
    }

    NativeSocket native() const { return socket_; }

    bool sendAll(std::string_view data) override
    {
        size_t sent = 0;
        while (sent < data.size()) {
            const int chunk = int(std::min<size_t>(data.size() - sent, 1 << 20));
#if defined(_WIN32)
            const int n = ::send(socket_, data.data() + sent, chunk, 0);
#else
            const ssize_t n = ::send(socket_, data.data() + sent, size_t(chunk), kSendFlags);
#endif
            if (n <= 0)
                return false;
            sent += size_t(n);
        }
        return true;
    }

    bool readLine(std::string& line) override
    {
        for (;;) {
            const size_t newline = buffer_.find('\n');
            if (newline != std::string::npos) {
                line = buffer_.substr(0, newline);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                buffer_.erase(0, newline + 1);
                return true;
            }
            if (buffer_.size() > (16u << 20))
                return false; // refuse absurdly long lines
            char chunk[4096];
#if defined(_WIN32)
            const int n = ::recv(socket_, chunk, int(sizeof(chunk)), 0);
#else
            const ssize_t n = ::recv(socket_, chunk, sizeof(chunk), 0);
#endif
            if (n <= 0)
                return false;
            buffer_.append(chunk, size_t(n));
        }
    }

    bool waitReadable(int milliseconds) override
    {
        if (!buffer_.empty())
            return true;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(socket_, &set);
        timeval timeout{milliseconds / 1000, (milliseconds % 1000) * 1000};
        return ::select(int(socket_) + 1, &set, nullptr, nullptr, &timeout) > 0;
    }

    void shutdown() override { ::shutdown(socket_, ENP_SHUT_BOTH); }

private:
    NativeSocket socket_;
    std::string buffer_;
};

sockaddr_in loopback(int port)
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<unsigned short>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return address;
}

} // namespace

std::unique_ptr<Socket> listenLoopback(int& port, std::string* error)
{
    if (!initialize()) {
        if (error)
            *error = "socket library initialisation failed";
        return nullptr;
    }
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == ENP_INVALID_SOCKET) {
        if (error)
            *error = lastError("socket");
        return nullptr;
    }
    auto socket = std::make_unique<TcpSocket>(s);
    sockaddr_in address = loopback(port);
    if (::bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || ::listen(s, 8) != 0) {
        if (error)
            *error = lastError("bind/listen");
        return nullptr;
    }
    socklen_t length = sizeof(address);
    ::getsockname(s, reinterpret_cast<sockaddr*>(&address), &length);
    port = ntohs(address.sin_port);
    return socket;
}

std::unique_ptr<Socket> accept(Socket& listener)
{
    const NativeSocket s = ::accept(static_cast<TcpSocket&>(listener).native(), nullptr, nullptr);
    if (s == ENP_INVALID_SOCKET)
        return nullptr;
    noSigPipe(s);
    return std::make_unique<TcpSocket>(s);
}

std::unique_ptr<Socket> connectLoopback(int port, std::string* error)
{
    if (!initialize()) {
        if (error)
            *error = "socket library initialisation failed";
        return nullptr;
    }
    const NativeSocket s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == ENP_INVALID_SOCKET) {
        if (error)
            *error = lastError("socket");
        return nullptr;
    }
    auto socket = std::make_unique<TcpSocket>(s);
    noSigPipe(s);
    sockaddr_in address = loopback(port);
    if (::connect(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        if (error)
            *error = lastError("connect");
        return nullptr;
    }
    return socket;
}

} // namespace enp::os::net
