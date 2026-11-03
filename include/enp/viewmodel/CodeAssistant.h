// Code suggestions from a model: IntelliSense's provider that writes the code a comment describes.
// The model is the user's choice (saved):
//   - Local: the on-device model (plan 21, Tools > Local Model), offline;
//   - Claude: online, through Claude Code (the Agent panel's set-up), with an optional model name;
//   - Command: any program that reads the prompt on stdin and prints the code: a local server's
//     client (`ollama run qwen2.5-coder:7b`) or an online one (`llm -m gpt-4o-mini`).
// It turns IntellisenseViewModel::Request into a prompt (the language and standard, the headers the
// file includes, the declarations IntelliSense chose, the code around the caret with its comments,
// and the comment to implement), gets the answer, takes the code out of it and offers it in the
// suggestion list.
//
// The local engine answers one question at a time and is shared with the Agent panel: while the
// agent uses it, the comment waits for its turn. A question that is not wanted any more (the list
// closed, the folder closed) is cancelled (local model), or its answer dropped when it arrives (a
// program, which ends by itself).
//
// Memory (docs/MEMORY.md): the question waiting for its turn and the answer being streamed are
// the only state, in the "intellisense" scope, released when the answer is offered or dropped.
// The local model's own memory belongs to the engine (unloaded when idle).
#pragma once

#include "enp/interfaces/IAgentCliService.h"
#include "enp/interfaces/ILocalEngineService.h"
#include "enp/interfaces/IProcessService.h"
#include "enp/viewmodel/AgentViewModel.h"
#include "enp/viewmodel/IntellisenseViewModel.h"
#include "enp/viewmodel/Observable.h"

#include <functional>
#include <memory>
#include <string>

namespace enp::viewmodel {

class CodeAssistant {
public:
    enum class Source { Local, Claude, Command }; // saved as numbers: append only
    static constexpr int kMaxTokens = 384;     // a block of code, not an essay
    static constexpr int kContextTokens = 4096;

    CodeAssistant(IntellisenseViewModel& intellisense, interfaces::ILocalEngineService* engine,
                  std::function<AgentViewModel::LocalSetup()> localSetup, interfaces::IAgentCliService* agentCli,
                  interfaces::IProcessService* processes);
    ~CodeAssistant();
    CodeAssistant(const CodeAssistant&) = delete;
    CodeAssistant& operator=(const CodeAssistant&) = delete;

    // ---- The model (saved) ------------------------------------------------------------------
    Property<int> source{int(Source::Local)}; // CodeAssistant::Source
    Property<std::string> claudeModel;         // "haiku", "sonnet"; empty: Claude Code's own setting
    Property<std::string> command;             // Source::Command: the program and its arguments
    static const char* sourceName(Source source); // "Local model (on this device)"
    // Whether the chosen model can answer now, and why not, in plain words.
    bool ready(std::string* why = nullptr) const;

    // Registers itself as IntelliSense's provider of code from comments.
    void attach();
    // Every frame: starts the question waiting for the local engine once it is free.
    void tick();
    // The folder closed: the question about it is cancelled or its answer dropped.
    void workspaceChanged() { cancel(); }
    bool busy() const { return running_ >= 0 || hasWaiting_; }
    // Swapped in, not assigned: assigning a short path into the string that held a long one keeps
    // the long one's heap storage (docs/MEMORY.md rule 5), the closed folder's path among it.
    void setWorkingDirectory(std::string directory) { workingDirectory_.swap(directory); }

    // The prompt for a request (public for tests): the rules, then the question.
    static std::string systemPrompt(const IntellisenseViewModel::Request& request);
    static std::string userPrompt(const IntellisenseViewModel::Request& request);
    // The code in a model's answer: the first fenced block when there is one, else the text
    // without lines that only repeat the comment; trimmed.
    static std::string extractCode(std::string_view answer, std::string_view instruction);
    // The shell that runs a Command: {"/bin/sh", "-c", command} or {"cmd.exe", "/c", command}.
    static std::vector<std::string> shellArguments(const std::string& command);

private:
    void request(const IntellisenseViewModel::Request& request);
    void cancel();
    void start(const IntellisenseViewModel::Request& request);
    void startLocal(const IntellisenseViewModel::Request& request);
    void startProgram(const IntellisenseViewModel::Request& request);
    void answered(int serial, bool ok, const std::string& answer, const std::string& error, const std::string& detail);
    void finished();
    void clearWaiting();

    IntellisenseViewModel& intellisense_;
    interfaces::ILocalEngineService* engine_;
    std::function<AgentViewModel::LocalSetup()> localSetup_;
    interfaces::IAgentCliService* agentCli_;
    interfaces::IProcessService* processes_;
    std::string workingDirectory_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    int running_ = -1;      // the serial of the request being answered
    bool runningLocal_ = false;
    std::string answer_;    // streamed so far (local model)
    std::string instruction_;
    IntellisenseViewModel::Request waiting_; // asked while the local engine was busy
    bool hasWaiting_ = false;
};

} // namespace enp::viewmodel
