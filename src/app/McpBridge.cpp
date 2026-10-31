#include "McpBridge.h"

#include "AutomationController.h"

#include "enp/os/Os.h"
#include "enp/os/Socket.h"
#include "enp/services/AutomationServer.h"

#if defined(_WIN32)
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   include <windows.h>
#else
#   include <iostream>
#endif

namespace enp::app {

using model::Json;

namespace {

constexpr const char* kProtocolVersion = "2025-06-18";

Json rpcError(const Json& id, int code, const std::string& message)
{
    return Json::object()
        .set("jsonrpc", "2.0")
        .set("id", id)
        .set("error", Json::object().set("code", code).set("message", message));
}

Json rpcResult(const Json& id, Json result)
{
    return Json::object().set("jsonrpc", "2.0").set("id", id).set("result", std::move(result));
}

Json textContent(const std::string& text)
{
    return Json::array().push(Json::object().set("type", "text").set("text", text));
}

// stdio that also works for a Windows GUI-subsystem executable started with redirected handles.
bool readStdinLine(std::string& line)
{
#if defined(_WIN32)
    static std::string buffer;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        const size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            buffer.erase(0, newline + 1);
            return true;
        }
        char chunk[4096];
        DWORD read = 0;
        if (!in || in == INVALID_HANDLE_VALUE || !ReadFile(in, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
            if (buffer.empty())
                return false;
            line.swap(buffer);
            buffer.clear();
            return true;
        }
        buffer.append(chunk, read);
    }
#else
    return bool(std::getline(std::cin, line));
#endif
}

void writeStdoutLine(const std::string& text)
{
#if defined(_WIN32)
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const std::string data = text + "\n";
    DWORD written = 0;
    if (out && out != INVALID_HANDLE_VALUE)
        WriteFile(out, data.data(), DWORD(data.size()), &written, nullptr);
#else
    std::cout << text << '\n' << std::flush;
#endif
}

} // namespace

McpSession::McpSession(Forward forward, std::string version) : forward_(std::move(forward)), version_(std::move(version)) {}

std::optional<Json> McpSession::handle(const Json& message)
{
    const Json* idPtr = message.find("id");
    const Json id = idPtr ? *idPtr : Json();
    const std::string method = message.getString("method");
    const bool notification = idPtr == nullptr;
    if (!message.isObject() || method.empty())
        return notification ? std::nullopt : std::optional<Json>(rpcError(id, -32600, "invalid request"));
    if (notification)
        return std::nullopt; // notifications/initialized, notifications/cancelled, ...
    const Json params = message.find("params") ? *message.find("params") : Json::object();

    if (method == "initialize") {
        const std::string requested = params.getString("protocolVersion");
        return rpcResult(id, Json::object()
                                 .set("protocolVersion", requested.empty() ? kProtocolVersion : requested)
                                 .set("capabilities", Json::object().set("tools", Json::object().set("listChanged", false)))
                                 .set("serverInfo", Json::object().set("name", "easynotepad").set("version", version_))
                                 .set("instructions",
                                      "Operates the running EasyNotepad editor. Use get_state and list_elements to see "
                                      "the screen, point to show the user where things are with the glowing agent "
                                      "cursor, and click or execute_command to act without touching the user's mouse."));
    }
    if (method == "ping")
        return rpcResult(id, Json::object());
    if (method == "tools/list") {
        Json tools = Json::array();
        for (const AutomationTool& t : automationTools())
            tools.push(Json::object().set("name", t.name).set("description", t.description).set("inputSchema", t.inputSchema));
        return rpcResult(id, Json::object().set("tools", std::move(tools)));
    }
    if (method == "tools/call") {
        const std::string name = params.getString("name");
        bool known = false;
        for (const AutomationTool& t : automationTools())
            known = known || name == t.name;
        if (!known)
            return rpcError(id, -32602, "unknown tool '" + name + "'");
        const Json arguments = params.find("arguments") ? *params.find("arguments") : Json::object();
        std::string error;
        const std::optional<Json> result = forward_(name, arguments, &error);
        if (!result)
            return rpcResult(id, Json::object().set("content", textContent(error)).set("isError", true));
        return rpcResult(id, Json::object().set("content", textContent(result->dump())).set("isError", false));
    }
    return rpcError(id, -32601, "method not found: " + method);
}

std::optional<Json> forwardToRunningApp(const std::string& method, const Json& params, std::string* error)
{
    const std::optional<std::string> text = os::fs::readFile(services::AutomationServer::endpointPath());
    const std::optional<Json> endpoint = text ? Json::parse(*text) : std::nullopt;
    if (!endpoint || endpoint->getNumber("port") <= 0) {
        if (error)
            *error = "EasyNotepad is not running (start the app first; it publishes " +
                     services::AutomationServer::endpointPath() + ")";
        return std::nullopt;
    }
    std::string connectError;
    std::unique_ptr<os::net::Socket> socket = os::net::connectLoopback(int(endpoint->getNumber("port")), &connectError);
    if (!socket) {
        if (error)
            *error = "cannot reach EasyNotepad: " + connectError;
        return std::nullopt;
    }
    const Json request = Json::object()
                             .set("token", endpoint->getString("token"))
                             .set("id", 1)
                             .set("method", method)
                             .set("params", params);
    std::string line;
    if (!socket->sendAll(request.dump() + "\n") || !socket->readLine(line)) {
        if (error)
            *error = "EasyNotepad closed the connection";
        return std::nullopt;
    }
    const std::optional<Json> reply = Json::parse(line);
    if (!reply) {
        if (error)
            *error = "invalid reply from EasyNotepad";
        return std::nullopt;
    }
    if (const Json* err = reply->find("error"); err && err->isString()) {
        if (error)
            *error = err->asString();
        return std::nullopt;
    }
    return reply->find("result") ? *reply->find("result") : Json::object();
}

int runMcpBridge(const std::string& version)
{
    McpSession session(forwardToRunningApp, version);
    std::string line;
    while (readStdinLine(line)) {
        if (line.empty())
            continue;
        const std::optional<Json> message = Json::parse(line);
        if (!message) {
            writeStdoutLine(rpcError(Json(), -32700, "parse error").dump());
            continue;
        }
        if (std::optional<Json> response = session.handle(*message))
            writeStdoutLine(response->dump());
    }
    return 0;
}

} // namespace enp::app
