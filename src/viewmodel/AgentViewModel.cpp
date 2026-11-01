#include "enp/viewmodel/AgentViewModel.h"

#include "enp/model/Json.h"
#include "enp/viewmodel/DummyData.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>

namespace enp::viewmodel {

namespace {

constexpr double kReplyDelay = 1.2; // seconds of simulated thinking

} // namespace

AgentViewModel::AgentViewModel()
    : messages_(dummy::conversation()), tasks_(dummy::tasks()), changes_(dummy::changes())
{
}

const std::vector<const char*>& AgentViewModel::modelNames()
{
    // The first is whatever Claude Code is set to; the others are its model aliases (--model).
    static const std::vector<const char*> names = {"Default model", "Opus", "Sonnet", "Haiku"};
    return names;
}

const std::vector<const char*>& AgentViewModel::autonomyNames()
{
    static const std::vector<const char*> names = {"Edits files", "Plan only (no edits)"};
    return names;
}

const model::FileChange* AgentViewModel::currentChange() const
{
    const int index = selectedChange.get();
    return index >= 0 && index < int(changes_.size()) ? &changes_[size_t(index)] : nullptr;
}

int AgentViewModel::pendingChangeCount() const
{
    int count = 0;
    for (const model::FileChange& c : changes_)
        if (c.state == model::ChangeState::Proposed)
            ++count;
    return count;
}

void AgentViewModel::submitPrompt(const std::string& prompt, const std::string& context)
{
    if (prompt.empty() || busy.get())
        return;
    std::string text = prompt;
    if (!context.empty())
        text += "\n[context: " + context + "]";
    messages_.push_back({model::ChatRole::User, std::move(text)});
    pendingPrompt_ = prompt;
    busy.set(true);
    if (live_) {
        askCli(prompt, context);
        return;
    }
    replyAt_ = now_ + kReplyDelay;
}

void AgentViewModel::newChat()
{
    stop();
    sessionId_.clear(); // the next prompt starts a new conversation
    messages_.clear();
    tasks_.clear();
    replyCount_ = 0;
    statusMessage.emit("Started a new agent conversation");
}

void AgentViewModel::stop()
{
    if (!busy.get())
        return;
    replyAt_ = -1.0;
    ++generation_; // the CLI's answer, if it still comes, is dropped
    if (cli_)
        cli_->cancel();
    activity_.clear();
    messages_.push_back({model::ChatRole::Tool, "Stopped by user."});
    busy.set(false);
}

void AgentViewModel::acceptChange(int index)
{
    if (index < 0 || index >= int(changes_.size()) || changes_[size_t(index)].state != model::ChangeState::Proposed)
        return;
    model::FileChange& c = changes_[size_t(index)];
    c.state = model::ChangeState::Accepted;
    changeAccepted.emit(c.path, c.after);
    statusMessage.emit("Accepted the agent's change to " + c.path);
}

void AgentViewModel::rejectChange(int index)
{
    if (index < 0 || index >= int(changes_.size()) || changes_[size_t(index)].state != model::ChangeState::Proposed)
        return;
    changes_[size_t(index)].state = model::ChangeState::Rejected;
    statusMessage.emit("Rejected the agent's change to " + changes_[size_t(index)].path);
}

void AgentViewModel::acceptAll()
{
    for (int i = 0; i < int(changes_.size()); ++i)
        acceptChange(i);
}

void AgentViewModel::rejectAll()
{
    for (int i = 0; i < int(changes_.size()); ++i)
        rejectChange(i);
}

void AgentViewModel::tick(double now)
{
    now_ = now;
    if (replyAt_ < 0.0 || now < replyAt_)
        return;
    replyAt_ = -1.0;
    messages_.push_back({model::ChatRole::Agent, dummy::agentReply(replyCount_++, pendingPrompt_)});
    for (model::AgentTask& task : tasks_) {
        if (task.state == model::TaskState::Running) {
            task.state = model::TaskState::Done;
        } else if (task.state == model::TaskState::Pending) {
            task.state = model::TaskState::Running;
            break;
        }
    }
    busy.set(false);
    replied.emit(pendingPrompt_);
}

void AgentViewModel::goLive(interfaces::IAgentCliService* cli, const std::string& workingDirectory)
{
    ++generation_;
    live_ = true;
    cli_ = cli;
    workingDirectory_ = workingDirectory;
    sessionId_.clear();
    messages_.clear();
    tasks_.clear();
    changes_.clear();
    selectedChange.set(-1);
    replyAt_ = -1.0;
    busy.set(false);
}

void AgentViewModel::askCli(const std::string& prompt, const std::string& context)
{
    if (!cli_ || !cli_->available()) {
        messages_.push_back({model::ChatRole::Agent,
                             "No agent CLI is available" + std::string(cli_ ? " ('" + cli_->command() + "' was not found)" : "") +
                                 ". Install Claude Code (the `claude` command), or set EASYNOTEPAD_AGENT_CLI to your agent's "
                                 "command line (it gets the prompt on stdin and answers on stdout). Agents can also work "
                                 "in the editor through the MCP connector: EasyNotepad --mcp."});
        busy.set(false);
        replied.emit(prompt);
        return;
    }
    if (cli_->canStream()) {
        askStream(prompt, context);
        return;
    }
    // One-shot CLI: the recent conversation and what the user is looking at travel with the prompt.
    std::string full = "You are the assistant inside the EasyNotepad code editor, working in this folder. Answer concisely.\n";
    if (!context.empty())
        full += "The user is looking at " + context + ".\n";
    const size_t first = messages_.size() > 9 ? messages_.size() - 9 : 0;
    if (messages_.size() > 1) {
        full += "\nConversation so far:\n";
        for (size_t i = first; i + 1 < messages_.size(); ++i)
            full += std::string(messages_[i].role == model::ChatRole::User ? "User: " : "Assistant: ") + messages_[i].text + "\n";
    }
    full += "\nUser: " + prompt + "\n";
    const int generation = ++generation_;
    cli_->run(full, workingDirectory_, [this, generation, prompt](bool ok, const std::string& output) {
        if (generation != generation_)
            return; // stopped or superseded
        messages_.push_back({ok ? model::ChatRole::Agent : model::ChatRole::Tool, ok ? output : "The agent failed: " + output});
        if (!ok)
            noteFailure(output);
        busy.set(false);
        replied.emit(prompt);
    });
}

namespace {

const std::string kAppTools = "mcp__easynotepad__";

// Told to the agent with every turn.
constexpr const char* kSystemPrompt =
    "You are working inside the EasyNotepad code editor, in the folder the user has open. Work with the files "
    "directly: the editor reloads files you change. The easynotepad MCP tools operate the editor the user is "
    "looking at, through a glowing agent pointer that never moves the user's own mouse: get_state, list_elements, "
    "point, click, open_file, execute_command and more. Use them when the user asks you to show, point at or do "
    "something in the app; otherwise do not. Keep answers short.";

// "C:\work\src\a.cpp" in folder "C:/work" -> "src/a.cpp" (any slashes; letter case ignored).
std::string relativeTo(const std::string& folder, std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string base = folder;
    std::replace(base.begin(), base.end(), '\\', '/');
    if (base.empty() || path.size() <= base.size() + 1 || path[base.size()] != '/')
        return path;
    for (size_t i = 0; i < base.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(path[i])) != std::tolower(static_cast<unsigned char>(base[i])))
            return path;
    return path.substr(base.size() + 1);
}

// "Read src/main.cpp", "click command:ToggleTerminal": a tool call in a few words.
std::string describeTool(const std::string& name, const model::Json& input, const std::string& folder)
{
    const std::string tool = name.compare(0, kAppTools.size(), kAppTools) == 0 ? name.substr(kAppTools.size()) : name;
    for (const char* key : {"file_path", "path", "pattern", "command", "id", "url", "query", "description"}) {
        std::string value = input.getString(key);
        if (value.empty())
            continue;
        if (std::string(key) == "file_path" || std::string(key) == "path")
            value = relativeTo(folder, value);
        if (const size_t newline = value.find('\n'); newline != std::string::npos)
            value = value.substr(0, newline) + " ...";
        if (value.size() > 120)
            value = value.substr(0, 117) + "...";
        return tool + " " + value;
    }
    return tool;
}

// A tool result's text (a string, or a list of text blocks).
std::string resultText(const model::Json& content)
{
    if (content.isString())
        return content.asString();
    std::string text;
    for (const model::Json& block : content.items())
        text += block.getString("text");
    return text;
}

} // namespace

void AgentViewModel::askStream(const std::string& prompt, const std::string& context)
{
    interfaces::AgentStreamRequest request;
    request.prompt = context.empty() ? prompt : "(I am looking at " + context + ")\n\n" + prompt;
    request.workingDirectory = workingDirectory_;
    request.sessionId = sessionId_;
    request.systemPrompt = kSystemPrompt;
    static const char* const aliases[] = {"", "opus", "sonnet", "haiku"};
    if (modelIndex.get() > 0 && modelIndex.get() < int(std::size(aliases)))
        request.model = aliases[modelIndex.get()];
    request.planOnly = autonomyIndex.get() == 1;
    request.appControl = appControl.get();
    const int generation = ++generation_;
    streamingMessage_ = -1;
    streamedText_ = false;
    resultSeen_ = false;
    activity_ = sessionId_.empty() ? "Starting Claude Code" : "Thinking";
    cli_->stream(
        request,
        [this, generation, prompt](const std::string& line) {
            if (generation == generation_)
                streamLine(line, prompt);
        },
        [this, generation, prompt](bool ok, const std::string& error) {
            if (generation != generation_ || resultSeen_)
                return; // stopped, superseded, or already answered
            messages_.push_back({model::ChatRole::Tool, ok ? std::string("The agent ended without an answer.")
                                                           : "The agent failed: " + error});
            noteFailure(error);
            activity_.clear();
            busy.set(false);
            replied.emit(prompt);
        });
}

void AgentViewModel::streamLine(const std::string& line, const std::string& prompt)
{
    const std::optional<model::Json> event = model::Json::parse(line);
    if (!event || !event->isObject())
        return;
    const std::string type = event->getString("type");
    if (const std::string session = event->getString("session_id"); !session.empty())
        sessionId_ = session;

    if (type == "system" && event->getString("subtype") == "init") {
        activity_ = "Thinking";
        if (const model::Json* servers = event->find("mcp_servers"))
            for (const model::Json& server : servers->items())
                if (server.getString("name") == "easynotepad" && server.getString("status") != "connected")
                    messages_.push_back({model::ChatRole::Tool, "The agent cannot operate the app this time (connector " +
                                                                    server.getString("status") + ")."});
        return;
    }
    if (type == "stream_event") {
        const model::Json* inner = event->find("event");
        if (!inner)
            return;
        const std::string kind = inner->getString("type");
        if (kind == "message_start") {
            streamingMessage_ = -1;
            streamedText_ = false;
            activity_ = "Thinking";
        } else if (kind == "content_block_delta") {
            const model::Json* delta = inner->find("delta");
            if (!delta || delta->getString("type") != "text_delta")
                return;
            if (streamingMessage_ < 0) {
                messages_.push_back({model::ChatRole::Agent, {}});
                streamingMessage_ = int(messages_.size()) - 1;
            }
            messages_[size_t(streamingMessage_)].text += delta->getString("text");
            streamedText_ = true;
            activity_ = "Writing";
        }
        return;
    }
    if (type == "assistant") {
        const model::Json* message = event->find("message");
        const model::Json* content = message ? message->find("content") : nullptr;
        if (!content)
            return;
        for (const model::Json& block : content->items()) {
            const std::string kind = block.getString("type");
            if (kind == "text" && !streamedText_ && !block.getString("text").empty()) {
                messages_.push_back({model::ChatRole::Agent, block.getString("text")}); // no live text: whole
            } else if (kind == "tool_use") {
                const model::Json* input = block.find("input");
                const std::string step = describeTool(block.getString("name"), input ? *input : model::Json::object(), workingDirectory_);
                messages_.push_back({model::ChatRole::Tool, step});
                activity_ = step;
                streamingMessage_ = -1; // text after the tool goes in a new bubble
            }
        }
        return;
    }
    if (type == "user") {
        const model::Json* message = event->find("message");
        const model::Json* content = message ? message->find("content") : nullptr;
        if (!content)
            return;
        for (const model::Json& block : content->items())
            if (block.getString("type") == "tool_result" && block.getBool("is_error")) {
                std::string text = block.find("content") ? resultText(*block.find("content")) : std::string();
                if (text.size() > 300)
                    text = text.substr(0, 297) + "...";
                messages_.push_back({model::ChatRole::Tool, "  failed: " + text});
            }
        activity_ = "Thinking";
        return;
    }
    if (type == "result") {
        resultSeen_ = true;
        if (event->getBool("is_error") || event->getString("subtype") != "success") {
            const std::string why = event->getString("result");
            messages_.push_back({model::ChatRole::Tool, "The agent stopped: " + (why.empty() ? event->getString("subtype") : why)});
            noteFailure(why);
        } else {
            needsLogin_ = false;
            if (messages_.empty() || messages_.back().role == model::ChatRole::User)
                messages_.push_back({model::ChatRole::Agent, event->getString("result")}); // nothing came before
        }
        char cost[64];
        std::snprintf(cost, sizeof(cost), "%.2f", event->getNumber("total_cost_usd"));
        statusMessage.emit("Agent answered in " + std::to_string(int(event->getNumber("num_turns"))) + " turn(s), $" + cost);
        activity_.clear();
        streamingMessage_ = -1;
        busy.set(false);
        replied.emit(prompt);
    }
}

bool AgentViewModel::looksLikeLoginProblem(const std::string& text)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* sign : {"/login", "not logged in", "log in", "logged out", "invalid api key", "api key",
                             "authenticat", "unauthorized", "oauth", "401"})
        if (lower.find(sign) != std::string::npos)
            return true;
    return false;
}

void AgentViewModel::noteFailure(const std::string& text)
{
    if (!looksLikeLoginProblem(text))
        return;
    needsLogin_ = true;
    showSetup.set(true); // the card says how to log in
}

} // namespace enp::viewmodel
