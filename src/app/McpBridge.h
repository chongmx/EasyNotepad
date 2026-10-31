// `EasyNotepad --mcp`: a Model Context Protocol server over stdio (newline-delimited JSON-RPC 2.0)
// that forwards tool calls to the running EasyNotepad through its local automation endpoint.
// Register it with an MCP client as the command `EasyNotepad --mcp`.
#pragma once

#include "enp/model/Json.h"

#include <functional>
#include <optional>
#include <string>

namespace enp::app {

class McpSession {
public:
    // Calls an automation method on the running app; returns nullopt and sets `error` on failure.
    using Forward = std::function<std::optional<model::Json>(const std::string& method, const model::Json& params,
                                                             std::string* error)>;

    McpSession(Forward forward, std::string version);

    // Handles one JSON-RPC message; returns the response, or nullopt for notifications.
    std::optional<model::Json> handle(const model::Json& message);

private:
    Forward forward_;
    std::string version_;
};

// Forwards to the app found through AutomationServer::endpointPath().
std::optional<model::Json> forwardToRunningApp(const std::string& method, const model::Json& params, std::string* error);

// Runs the stdio loop until stdin closes; returns the process exit code.
int runMcpBridge(const std::string& version);

} // namespace enp::app
