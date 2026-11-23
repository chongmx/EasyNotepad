// Agent session: conversation, plan and proposed file changes.
// Runs on dummy data until the agent service exists.
#pragma once

#include "enp/interfaces/IAgentCliService.h"
#include "enp/interfaces/ILocalEngineService.h"
#include "enp/model/Agent.h"
#include "enp/viewmodel/Observable.h"

#include <functional>
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
    // Which agent answers (saved): Auto (the local model for quick questions about the code, Claude
    // Code for the rest), always Claude, or always the local model.
    enum class Choice { Auto, Claude, Local };
    Property<int> agentChoice{0};
    static const std::vector<const char*>& choiceNames();
    // When Claude Code cannot answer (not installed, logged out, offline, failed), the local model
    // does (saved).
    Property<bool> localFallback{true};

    // The local model: whether it can answer and with what, and the text the user is looking at
    // (the local model cannot read files itself, so it gets the code with the question).
    struct LocalSetup {
        bool ready = false;
        std::string why; // when not ready
        std::string modelPath, drafterPath;
        std::string projectorPath; // its eyes and ears (pictures and sounds); empty when not downloaded
        bool hears = true;          // its projector takes sound (Gemma); Qwen's sees pictures only
        // A model without ears of its own: its speech model and that one's audio encoder (Qwen3-ASR),
        // when downloaded; empty otherwise.
        std::string earsPath, earsProjectorPath;
        int tokenPixels = 48 * 48;  // the pixels of a picture one of its tokens covers
        bool compactCache = false;  // its context cache in 8 bits (LocalChatRequest::compactCache)
        int threads = 4;
        bool gpu = false;
    };
    void setLocal(interfaces::ILocalEngineService* engine, std::function<LocalSetup()> setup,
                  std::function<std::string()> contextText);
    // The local model's tools (the app's: src/app/LocalAgentTools.cpp): those it may call (no edit
    // tools in Plan only), who runs a call (the result may come later: a look at a page waits for
    // the page to be drawn), and the guide it gets with every question (what it can do, its skills).
    struct ToolOutput {
        std::string text;
        std::vector<interfaces::LocalMedia> media; // pictures or sounds for the model to take in
        // Pictures for the user to see in the chat (paths: in the open folder, or a tool's output in
        // temp/agent-images next to the app), shown under the tool's step.
        std::vector<std::string> shown;
        // The file a reading tool read when it is not the path it was given (a wrong folder or a
        // bare name, found by its name): folder-relative.
        std::string read{};
    };
    using ToolRunner = std::function<void(const interfaces::LocalToolCall& call, std::function<void(ToolOutput)> done)>;
    void setLocalTools(std::function<std::vector<interfaces::LocalTool>(bool canEdit)> tools, ToolRunner run,
                       std::function<std::string()> guide);
    // Added to each question (the end of the prompt, where a small model looks): the user's preferences.
    void setLocalReminder(std::function<std::string()> reminder) { localReminder_ = std::move(reminder); }
    // Added to each question too: the files it names that the open folder holds, with the tool for
    // each (a file in a subfolder is not in the top folder's listing, and the model gave up on it).
    void setLocalNamedFiles(std::function<std::string(const std::string& question)> named) { localNamedFiles_ = std::move(named); }
    // Questions taken in steps: before the work, the model splits the question into steps with these
    // instructions (the planning skill); the app then gives it one step at a time, and asks for the
    // answer from what the steps found. No instructions, or a one-step plan: the question as it is.
    // The instructions are made for the question (it may name tables whose columns the plan needs).
    void setLocalPlanner(std::function<std::string(const std::string& question)> instructions) { localPlanner_ = std::move(instructions); }
    Property<bool> localInSteps{true};
    // The fewest tokens the local model spends on any picture it looks at (saved): 0 lets it choose
    // per look (70 for a glance to 1120 for small print); 280, 560 or 1120 for at least that.
    Property<int> localPictureTokens{0};
    // For tools that ask the local model themselves (one document at a time): the model, device and
    // context of the exchange in progress; what to show as its activity meanwhile.
    const interfaces::LocalChatRequest& localRequestNow() const { return localRequest_; }
    void setLocalActivity(const std::string& what) { activity_ = what.empty() ? std::string() : "Local model: " + what; }
    // Counts conversations: a new chat or another folder starts the next (working notes go with it).
    int conversation() const { return conversation_; }
    // The local model's working notes (its tools keep them), repeated with each step of a question
    // taken in steps: what it found stays in front of it however long the exchange grows.
    void setLocalNotes(std::function<std::string()> notes) { localNotes_ = std::move(notes); }
    static constexpr int kPictureTokenChoices[] = {0, 280, 560, 1120};
    // The steps in a plan the model wrote ("1. ...", "2) ...", "Step 3: ..."), at most kMaxPlanSteps.
    static std::vector<std::string> planSteps(const std::string& plan);
    static constexpr size_t kMaxPlanSteps = 6;
    // The skills a plan's step names at its end ("... [skills: meetings, office-work]"), at most
    // kMaxStepSkills, taken out of the step's text.
    static std::vector<std::string> stepSkills(std::string& step);
    static constexpr size_t kMaxStepSkills = 2;
    // The task's memory, for a question taken in steps, set up by the model itself: what to keep in
    // mind (written with its plan, revised after each step) and what the steps found (written as it
    // revises), with the question, the plan and where it is, and its long-term memory (the user's
    // preferences). Each step is given it and the step alone, not what the earlier steps read. Written to a file
    // through this (agent-memory/task.md), for the user to see.
    void setLocalTaskMemory(std::function<void(const std::string& memory)> write) { localTaskMemory_ = std::move(write); }
    // What the task in progress keeps in mind (its memory's list): for a tool that reads on its behalf.
    const std::vector<std::string>& localKeepInMind() const { return localKeep_; }
    // The lines of the list under a heading ("Keep in mind:" then "- ..." lines), up to the next
    // heading or numbered line; the heading is matched without case.
    static std::vector<std::string> listUnder(const std::string& text, const std::string& heading);
    // Whether `line` (a line the model wrote) is in `source` word for word, case and spacing aside, and
    // long enough (20 letters or more) not to be a phrase anyone would write.
    static bool copiedFrom(const std::string& source, const std::string& line);
    static constexpr size_t kMemoryLines = 12; // the most lines a list of the memory keeps
    // A skill's instructions by its name (empty: no such skill). The plan names the skills each step
    // needs; each step is given those (once a question), not every skill up front.
    void setLocalSkill(std::function<std::string(const std::string& name)> skill) { localSkill_ = std::move(skill); }
    bool hasLocalTools() const { return runTool_ != nullptr; }
    // An edit the local model proposes (its edit tools): listed for review, applied when accepted.
    void proposeChange(const std::string& path, const std::string& before, const std::string& after);
    // Tool calls the local model may make for one question before it must answer.
    static constexpr int kMaxToolRounds = 16;
    bool localReady() const;
    bool claudeReady() const; // Claude Code can answer here now
    // A local answer: send the question it answered to Claude Code instead.
    void askClaudeInstead(int answerIndex);

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
    // The folder closed or another opened: work about the old one stops (its late answers are dropped), and
    // the local exchange's pictures and sounds go.
    void workspaceChanged();
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

    // Choosing the agent, and the local model.
    interfaces::ILocalEngineService* local_ = nullptr;
    std::function<LocalSetup()> localSetup_;
    std::function<std::string()> localContext_;
    std::string lastContext_;
    size_t claudeFrom_ = 0; // first message of the Claude answer in progress (tagged when it ends)
    void route(const std::string& prompt, const std::string& context);
    void askClaude(const std::string& prompt, const std::string& context);
    void askLocal(const std::string& prompt, const std::string& context, const std::string& note);
    // The local exchange in progress: the request grows with the model's tool calls and their results.
    std::function<std::vector<interfaces::LocalTool>(bool)> localTools_;
    ToolRunner runTool_;
    std::function<std::string()> localGuide_;
    std::function<std::string()> localReminder_;
    std::function<std::string(const std::string&)> localNamedFiles_;
    interfaces::LocalChatRequest localRequest_;
    int localRounds_ = 0;
    int localNudges_ = 0; // times it was told to make the call it announced
    // The final answer's check (does it give the figure asked for?): asked once a question; the
    // answer checked, kept if the model replies OK, at checkedAt_ in messages_ (npos: none waiting).
    int localEmptySearches_ = 0; // searches that found nothing, in this step
    bool localChecked_ = false;
    bool localOutOfRounds_ = false; // told once to answer without more tools
    size_t localCheckedAt_ = std::string::npos;
    std::function<std::string(const std::string&)> localPlanner_;
    std::function<std::string()> localNotes_;
    int conversation_ = 0;
    std::vector<std::string> localSteps_; // the plan of the question in progress
    size_t localStep_ = 0;                // steps given so far (steps + 1: the answer was asked for)
    void planLocal(int generation, std::string question);
    bool nextLocalStep(int generation, const std::string& answer);
    std::string localPrompt_;
    std::string localNamed_; // the files the question names (as told to the model): read before answering
    bool localNamedRead_ = false; // it has read one of them
    std::function<std::string(const std::string&)> localSkill_;
    std::vector<std::vector<std::string>> localStepSkills_; // the skills each step of the plan needs
    std::string skillsFor(size_t step);                     // their instructions, for that step
    // The figures the step in progress found with its tools (each call and its result lines: a
    // calculation, a table's rows, a folder's counts), shown to the model when it revises its memory
    // after the step: what to keep of them is its to decide.
    std::vector<std::string> localStepFacts_;
    void keepFacts(const interfaces::LocalToolCall& call, const std::string& result);
    // The task's memory (setLocalTaskMemory): the request's messages before the question (the system
    // prompt and earlier turns), kept for each step; the question as asked; the rest of the memory.
    std::function<void(const std::string&)> localTaskMemory_;
    size_t localBase_ = 0;
    std::string localQuestion_;
    std::string localLongTerm_;            // the user's preferences, from the long-term memory
    std::vector<std::string> localKeep_;   // what to keep in mind
    std::vector<std::string> localFound_;  // what the steps found
    std::string taskMemory(size_t now) const; // with step `now` (0-based) marked; steps.size(): all done
    void reviseMemory(int generation, const std::string& answer);
    void stepOn(int generation);
    // What the answer's figures are checked against: the question and every tool result of it (a
    // step's own context holds only its tools; the memory holds what the model wrote).
    std::vector<interfaces::LocalChatMessage> localEvidence_;
    bool localSawMedia_ = false;
    int localToolsUsed_ = 0;
    // The calls of the step in progress (name and arguments) and how often each was made: the same
    // call again is said to be so under its result; a third time ends the step (it is stuck).
    std::vector<std::pair<std::string, int>> localCallCounts_;
    int& callCount(const interfaces::LocalToolCall& call);
public:
    static constexpr int kStuckCalls = 3;
    static constexpr size_t kLastStepChars = 6000; // of the last step's tool results, given to the next
private:
public:
    static constexpr size_t kStepFactsChars = 2400; // the first of a step's kept within this
private:
    void sendLocal(int generation);
    void localAnswered(int generation, size_t at, const interfaces::LocalChatResult& r);
    void runLocalTools(int generation, std::vector<interfaces::LocalToolCall> calls, size_t next,
                       std::vector<interfaces::LocalMedia> seen);
    void endLocal();
    bool fallBackToLocal(const std::string& prompt, const std::string& why);
    void tagClaude();
    std::string lastAnswerBy() const;

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
