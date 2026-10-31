#include "enp/viewmodel/AgentViewModel.h"

#include "enp/viewmodel/DummyData.h"

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
    static const std::vector<const char*> names = {"Claude Opus 5.5", "Claude Sonnet 5.5", "Claude Haiku 5.5"};
    return names;
}

const std::vector<const char*>& AgentViewModel::autonomyNames()
{
    static const std::vector<const char*> names = {"Ask before edits", "Auto-apply edits", "Plan only"};
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
        busy.set(false);
        replied.emit(prompt);
    });
}

} // namespace enp::viewmodel
