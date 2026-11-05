#include "enp/viewmodel/CodeAssistant.h"

#include "enp/os/Memory.h"

#include <cstdio>
#include <utility>

namespace enp::viewmodel {

namespace {

std::string_view trimView(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
        s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

} // namespace

CodeAssistant::CodeAssistant(IntellisenseViewModel& intellisense, interfaces::ILocalEngineService* engine,
                             std::function<AgentViewModel::LocalSetup()> localSetup, interfaces::IAgentCliService* agentCli,
                             interfaces::IProcessService* processes)
    : intellisense_(intellisense), engine_(engine), localSetup_(std::move(localSetup)), agentCli_(agentCli), processes_(processes)
{
}

CodeAssistant::~CodeAssistant()
{
    *alive_ = false; // a program's answer arriving later finds nobody
    if (running_ >= 0 && runningLocal_ && engine_)
        engine_->cancel();
    intellisense_.setProvider({}); // nothing may call into this object any more
}

const char* CodeAssistant::sourceName(Source source)
{
    switch (source) {
    case Source::Local: return "Local model (on this device)";
    case Source::Claude: return "Claude (online, through Claude Code)";
    case Source::Command: return "A command (any local or online model)";
    }
    return "";
}

bool CodeAssistant::ready(std::string* why) const
{
    auto no = [why](const std::string& text) {
        if (why)
            *why = text;
        return false;
    };
    switch (Source(source.get())) {
    case Source::Local: {
        if (!engine_)
            return no("this build has no local model.");
        const AgentViewModel::LocalSetup local = localSetup_ ? localSetup_() : AgentViewModel::LocalSetup{};
        return local.ready ? true : no(local.why.empty() ? "set the local model up in Tools › Local Model." : local.why);
    }
    case Source::Claude:
        if (!agentCli_ || !agentCli_->available())
            return no("Claude Code is not set up: see the Agent panel's set-up (the gear).");
        return true;
    case Source::Command:
        if (!processes_)
            return no("this build cannot run programs.");
        if (trimView(command.get()).empty())
            return no("no command is set: give one in Edit › IntelliSense › Settings.");
        return true;
    }
    return no("unknown model.");
}

void CodeAssistant::attach()
{
    IntellisenseViewModel::Provider provider;
    provider.request = [this](const IntellisenseViewModel::Request& r) { request(r); };
    provider.cancel = [this]() { cancel(); };
    provider.completes = false; // names while typing stay IntelliSense's own: a model is too slow for that
    provider.implements = true;
    intellisense_.setProvider(std::move(provider));
}

std::vector<std::string> CodeAssistant::shellArguments(const std::string& command)
{
#if defined(_WIN32)
    return {"cmd.exe", "/c", command};
#else
    return {"/bin/sh", "-c", command};
#endif
}

std::string CodeAssistant::systemPrompt(const IntellisenseViewModel::Request& request)
{
    std::string language = model::languageSpec(request.language).name;
    if (!request.standard.empty())
        language = request.standard;
    return "You are a code completion engine inside a text editor. You write the " + language +
           " code that implements the comment the programmer wrote, to be inserted right after it. "
           "Reply with the code only: no explanation, no Markdown, and do not repeat the comment or the code that is "
           "already there. Use only what the file includes and the declarations you are shown; prefer them over "
           "inventing names. Match the style of the code around it. Keep it short: the smallest code that does what the "
           "comment says.";
}

std::string CodeAssistant::userPrompt(const IntellisenseViewModel::Request& request)
{
    std::string prompt;
    if (!request.path.empty())
        prompt += "File: " + request.path + "\n";
    if (!request.standard.empty())
        prompt += "Language: " + request.standard + "\n";
    if (!request.headers.empty()) {
        prompt += "The file includes:";
        for (const std::string& h : request.headers)
            prompt += " " + h;
        prompt += "\n";
    }
    if (!request.context.empty())
        prompt += "\nDeclarations you can use:\n" + IntellisenseViewModel::formatContext(request.context);
    // The code with its comments: they say what the code around is for.
    prompt += "\nCode before it (with its comments):\n```\n" + request.before + "\n```\n";
    if (!trimView(request.after).empty())
        prompt += "\nCode after it:\n```\n" + request.after + "\n```\n";
    prompt += "\nThe comment to implement:\n" + request.instruction + "\n\nWrite the code that goes right after the comment.";
    return prompt;
}

std::string CodeAssistant::extractCode(std::string_view answer, std::string_view instruction)
{
    // A fenced block: its content (the language tag after the fence is dropped).
    if (const size_t open = answer.find("```"); open != std::string_view::npos) {
        size_t start = answer.find('\n', open);
        if (start != std::string_view::npos) {
            ++start;
            const size_t close = answer.find("```", start);
            return std::string(trimView(answer.substr(start, close == std::string_view::npos ? std::string_view::npos : close - start)));
        }
    }
    // Plain text: without lines that only repeat the comment.
    std::string out;
    size_t start = 0;
    while (start < answer.size()) {
        size_t end = answer.find('\n', start);
        if (end == std::string_view::npos)
            end = answer.size();
        const std::string_view line = answer.substr(start, end - start);
        const std::string_view words = trimView(line);
        bool repeat = false;
        for (const char* marker : {"//", "#", "--", "/*", "*"})
            if (words.rfind(marker, 0) == 0) {
                const std::string_view said = trimView(words.substr(std::string_view(marker).size()));
                repeat = !said.empty() && instruction.find(said) != std::string_view::npos;
            }
        if (!repeat) {
            out.append(line.data(), line.size());
            out += '\n';
        }
        start = end + 1;
    }
    return std::string(trimView(out));
}

void CodeAssistant::request(const IntellisenseViewModel::Request& request)
{
    if (request.purpose != IntellisenseViewModel::Request::Purpose::Implement)
        return;
    std::string why;
    if (!ready(&why)) {
        intellisense_.providerFailed(request.serial, std::string("Code suggestions: ") + sourceName(Source(source.get())) + ": " + why);
        return;
    }
    {
        os::mem::Scope scope("intellisense");
        waiting_ = request; // started now, or when the local engine is free
        hasWaiting_ = true;
    }
    if (running_ >= 0 && runningLocal_) {
        engine_->cancel(); // our previous question: its answer is not wanted; this one starts after it
        return;
    }
    running_ = -1; // a program still running answers an older question: dropped when it ends
    tick();
}

void CodeAssistant::tick()
{
    if (!hasWaiting_ || running_ >= 0)
        return;
    if (waiting_.serial != intellisense_.requestSerial()) { // not wanted any more
        hasWaiting_ = false;
        clearWaiting();
        return;
    }
    if (Source(source.get()) == Source::Local && engine_ && (engine_->chatting() || engine_->running())) {
        // The Agent panel or the Engine lab has the model: wait for it.
        intellisense_.setProviderStatus(waiting_.serial, "Waiting for the local model (the agent is using it)");
        return;
    }
    IntellisenseViewModel::Request request = std::move(waiting_);
    hasWaiting_ = false;
    clearWaiting();
    start(request);
}

void CodeAssistant::start(const IntellisenseViewModel::Request& request)
{
    {
        os::mem::Scope scope("intellisense");
        instruction_ = request.instruction;
        os::mem::release(answer_);
    }
    running_ = request.serial;
    if (Source(source.get()) == Source::Local)
        startLocal(request);
    else
        startProgram(request);
}

void CodeAssistant::startLocal(const IntellisenseViewModel::Request& request)
{
    const AgentViewModel::LocalSetup local = localSetup_ ? localSetup_() : AgentViewModel::LocalSetup{};
    interfaces::LocalChatRequest chat;
    chat.modelPath = local.modelPath;
    chat.drafterPath = local.drafterPath;
    chat.threads = local.threads;
    chat.useGpu = local.gpu;
    chat.maxTokens = kMaxTokens;
    chat.contextTokens = kContextTokens;
    chat.messages.push_back({"system", systemPrompt(request)});
    chat.messages.push_back({"user", userPrompt(request)});
    const int serial = request.serial;
    runningLocal_ = true;
    std::weak_ptr<bool> alive = alive_;
    intellisense_.setProviderStatus(serial, "Asking the local model");
    engine_->chat(
        chat,
        [this, alive, serial](const std::string& stage) {
            if (!alive.expired() && running_ == serial)
                intellisense_.setProviderStatus(serial, stage);
        },
        [this, alive, serial](const std::string& piece) {
            if (alive.expired() || running_ != serial)
                return;
            {
                os::mem::Scope scope("intellisense");
                answer_ += piece;
            }
            int lines = 1;
            for (char c : answer_)
                lines += c == '\n' ? 1 : 0;
            char text[64];
            std::snprintf(text, sizeof(text), "Writing the code: %d line%s", lines, lines == 1 ? "" : "s");
            intellisense_.setProviderStatus(serial, text);
        },
        [this, alive, serial](const interfaces::LocalChatResult& result) {
            if (alive.expired())
                return; // the assistant is gone: nothing to tell
            char detail[96];
            std::snprintf(detail, sizeof(detail), "local model: %d tokens, %.1f tokens/s", result.generatedTokens, result.tokensPerSecond);
            answered(serial, result.ok, answer_, result.error, detail);
            finished();
        });
}

void CodeAssistant::startProgram(const IntellisenseViewModel::Request& request)
{
    const int serial = request.serial;
    runningLocal_ = false;
    const bool claude = Source(source.get()) == Source::Claude;
    const std::string prompt = systemPrompt(request) + "\n\n" + userPrompt(request);
    std::weak_ptr<bool> alive = alive_;
    intellisense_.setProviderStatus(serial, claude ? "Asking Claude" : "Running the command");
    auto done = [this, alive, serial, claude](bool ok, const std::string& output, const std::string& error) {
        if (alive.expired())
            return; // the assistant is gone: the answer is dropped with this call
        answered(serial, ok, output, error, claude ? "Claude" : "command");
        if (running_ == serial)
            finished();
    };
    if (claude && trimView(claudeModel.get()).empty()) {
        // The Agent panel's own command line (claude -p): the prompt on stdin.
        agentCli_->run(prompt, workingDirectory_, [done](bool ok, const std::string& output) { done(ok, output, ok ? std::string() : output); });
        return;
    }
    interfaces::ProcessRequest process;
    if (claude) {
        const std::string executable = agentCli_->executable();
        process.arguments = {executable.empty() ? std::string("claude") : executable, "-p", "--model", std::string(trimView(claudeModel.get()))};
    } else {
        process.arguments = shellArguments(command.get());
    }
    process.input = prompt;
    process.workingDirectory = workingDirectory_;
    processes_->run(process, [done](const interfaces::ProcessResult& r) {
        const bool ok = r.started && r.exitCode == 0;
        done(ok, r.output, !r.error.empty() ? r.error : !r.errorOutput.empty() ? r.errorOutput : "exit code " + std::to_string(r.exitCode));
    });
}

void CodeAssistant::answered(int serial, bool ok, const std::string& answer, const std::string& error, const std::string& detail)
{
    if (running_ != serial)
        return; // a question that is not wanted any more
    if (!ok) {
        std::string why = error;
        if (why.size() > 200)
            why.resize(200);
        intellisense_.providerFailed(serial, std::string("Code suggestions: ") + sourceName(Source(source.get())) + " could not answer: " + why);
        return;
    }
    IntellisenseViewModel::Item item;
    {
        os::mem::Scope scope("intellisense");
        item.insertText = extractCode(answer, instruction_);
        item.detail = detail;
    }
    std::vector<IntellisenseViewModel::Item> items;
    if (!item.insertText.empty())
        items.push_back(std::move(item));
    intellisense_.provide(serial, std::move(items));
}

void CodeAssistant::cancel()
{
    // Forgotten first: whatever the engine or a program reports about it later is dropped.
    hasWaiting_ = false;
    clearWaiting();
    const bool local = running_ >= 0 && runningLocal_;
    running_ = -1;
    if (local && engine_)
        engine_->cancel(); // its done() follows: finished() frees the answer streamed so far
}

void CodeAssistant::clearWaiting()
{
    // Swapped with an empty one that is destroyed here: assigning {} would keep the strings'
    // storage (docs/MEMORY.md rule 5).
    IntellisenseViewModel::Request empty;
    std::swap(waiting_, empty);
}

void CodeAssistant::finished()
{
    running_ = -1;
    runningLocal_ = false;
    os::mem::release(answer_);
    os::mem::release(instruction_);
    tick(); // a question asked meanwhile
}

} // namespace enp::viewmodel
