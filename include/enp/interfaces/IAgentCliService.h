// Contract for asking a command-line AI agent (e.g. `claude -p`) to do a one-shot task.
#pragma once

#include <functional>
#include <string>

namespace enp::interfaces {

// One turn of a streaming conversation with Claude Code.
struct AgentStreamRequest {
    std::string prompt;
    std::string workingDirectory;
    std::string sessionId;    // continues that conversation; empty starts one
    std::string systemPrompt; // added to the agent's own
    bool appControl = true;   // the agent gets the editor's MCP tools (pointer, click, open files...)
    std::string model;        // a model or alias ("sonnet"); empty: Claude Code's own setting
    bool planOnly = false;    // read and plan, change nothing; otherwise it edits files directly
};

class IAgentCliService {
public:
    virtual ~IAgentCliService() = default;
    // The configured command line, e.g. "claude -p".
    virtual std::string command() const = 0;
    // False when the command's executable cannot be found. Never blocks: the search runs in the
    // background, and until its first answer the command is assumed to be there.
    virtual bool available() const = 0;
    // Runs the agent with `prompt` on stdin in `workingDirectory`; `done` is called on the main
    // thread with success and the agent's output (or the error).
    virtual void run(const std::string& prompt, const std::string& workingDirectory,
                     std::function<void(bool ok, const std::string& output)> done) = 0;
    // Stops every running agent (and whatever it started); their `done` reports "stopped".
    virtual void cancel() {}

    // Set-up. The command line to use: empty means $EASYNOTEPAD_AGENT_CLI, else "claude -p".
    virtual void setCommand(const std::string& command) { (void)command; }
    // Looks for the program again now (after installing it, say), in the background.
    virtual void recheck() {}
    virtual bool checking() const { return false; }
    // The command runs Claude Code (which the app can also help install and log in).
    virtual bool isClaude() const { return false; }
    // Full path of the program found by the last check, if any.
    virtual std::string executable() const { return {}; }

    // True when the agent is Claude Code, which can hold a conversation and report as it works.
    virtual bool canStream() const { return false; }
    // Runs one turn: each line of Claude Code's stream-json output reaches `onLine` on the main
    // thread as it is written; `done` follows when the program ends (with its error, if any).
    virtual void stream(const AgentStreamRequest& request, std::function<void(const std::string& line)> onLine,
                        std::function<void(bool ok, const std::string& error)> done)
    {
        (void)request;
        (void)onLine;
        done(false, "this agent cannot stream");
    }
};

} // namespace enp::interfaces
