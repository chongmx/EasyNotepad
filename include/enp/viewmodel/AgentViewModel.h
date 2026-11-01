// Agent session: conversation, plan and proposed file changes.
// Runs on dummy data until the agent service exists.
#pragma once

#include "enp/interfaces/IAgentCliService.h"
#include "enp/model/Agent.h"
#include "enp/viewmodel/Observable.h"

#include <string>
#include <vector>

namespace enp::viewmodel {

class AgentViewModel {
public:
    AgentViewModel();

    // A real workspace: drops the sample session; prompts go to the agent CLI in `workingDirectory`.
    void goLive(interfaces::IAgentCliService* cli, const std::string& workingDirectory);
    bool live() const { return live_; }

    Property<bool> busy{false};
    Property<int> selectedChange{0}; // index into changes(), -1 when none
    Property<int> modelIndex{0};
    Property<int> autonomyIndex{0};
    // Set-up (saved): the agent command (empty: Claude Code) and whether it may operate the window.
    Property<std::string> command{std::string()};
    Property<bool> appControl{true};
    // The set-up card is open (also opens by itself when the agent needs installing or a login).
    Property<bool> showSetup{false};
    // The last run failed in a way that looks like Claude Code is not logged in.
    bool needsLogin() const { return needsLogin_; }
    static bool looksLikeLoginProblem(const std::string& text);

    // A change was accepted: the path and its new contents.
    Event<std::string, std::string> changeAccepted;
    Event<std::string> statusMessage;
    // The agent answered `prompt` (lets the app point at the controls the answer is about).
    Event<std::string> replied;

    static const std::vector<const char*>& modelNames();
    static const std::vector<const char*>& autonomyNames();

    const std::vector<model::ChatMessage>& messages() const { return messages_; }
    const std::vector<model::AgentTask>& tasks() const { return tasks_; }
    const std::vector<model::FileChange>& changes() const { return changes_; }
    const model::FileChange* currentChange() const;
    // What the agent is doing right now ("Thinking", "Read src/main.cpp", ...), while busy.
    const std::string& activity() const { return activity_; }
    // The Claude Code conversation the next prompt continues (empty: a new one).
    const std::string& sessionId() const { return sessionId_; }
    int pendingChangeCount() const;

    // `context` names what the prompt refers to, e.g. "src/main.cpp:12-20".
    void submitPrompt(const std::string& prompt, const std::string& context = {});
    void newChat();
    void stop();
    void acceptChange(int index);
    void rejectChange(int index);
    void acceptAll();
    void rejectAll();

    // Called once per frame with a monotonic time in seconds (drives the simulated replies).
    void tick(double now);

private:
    std::vector<model::ChatMessage> messages_;
    std::vector<model::AgentTask> tasks_;
    std::vector<model::FileChange> changes_;
    std::string pendingPrompt_;
    double replyAt_ = -1.0;
    double now_ = 0.0;
    int replyCount_ = 0;
    bool live_ = false;
    interfaces::IAgentCliService* cli_ = nullptr;
    std::string workingDirectory_;
    int generation_ = 0; // a stopped or replaced request's answer is dropped
    void askCli(const std::string& prompt, const std::string& context);

    // Streaming (Claude Code): one conversation, continued with each prompt.
    std::string sessionId_;
    std::string activity_;
    int streamingMessage_ = -1; // the agent message text is being appended to, -1 when none
    bool streamedText_ = false; // the current message's text came as it was written
    bool resultSeen_ = false;
    bool needsLogin_ = false;
    void noteFailure(const std::string& text);
    void askStream(const std::string& prompt, const std::string& context);
    void streamLine(const std::string& line, const std::string& prompt);
};

} // namespace enp::viewmodel
