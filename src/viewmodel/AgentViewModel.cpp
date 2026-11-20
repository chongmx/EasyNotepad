#include "enp/viewmodel/AgentViewModel.h"

#include "enp/model/AgentRoute.h"
#include "enp/model/Diff.h"
#include "enp/model/Json.h"
#include "enp/os/Memory.h"
#include "enp/viewmodel/DummyData.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <optional>

namespace enp::viewmodel {

namespace {

constexpr double kReplyDelay = 1.2; // seconds of simulated thinking
constexpr double kLocalIdleUnload = 300.0; // the local model leaves memory after this long unused
constexpr size_t kLocalHistoryChars = 6000; // earlier turns sent to the local model (about 1,500 tokens)

// Told to the local model with every question.
constexpr const char* kLocalSystemPrompt =
    "You are the assistant inside the EasyNotepad code editor, running offline on this device. Answer briefly and "
    "precisely, in Markdown. You see only the code the user shows you: you cannot open other files, edit files or "
    "run commands. If a request needs that, say so in one sentence and suggest asking Claude Code.";

// Told to the local model when it has the app's tools (the app's guide and skills follow it).
constexpr const char* kLocalAgentPrompt =
    "You are the assistant inside EasyNotepad, a code editor and document reader, running offline on this device. "
    "You work in the user's open folder through the tools you are given. Look before you answer: read the files, "
    "documents, pictures and recordings a question is about with your tools, and never guess what they contain. "
    "Call one or more tools when you need them; when you have what you need, answer briefly and precisely in "
    "Markdown, naming the files and pages you used. If a tool fails, say so or try another way.";

constexpr size_t kToolTextChars = 12000; // of one tool result sent to the model (about 3,000 tokens)

std::optional<interfaces::LocalToolCall> callInText(const std::string& text, const std::vector<interfaces::LocalTool>& tools,
                                                    std::string& rest);

// A call written Python-style: `remember(kind="preference", text="Short answers.")`, values quoted
// or bare (numbers, true, false). The text without it goes to `rest`.
std::optional<interfaces::LocalToolCall> keywordCallInText(const std::string& text, const std::vector<interfaces::LocalTool>& tools,
                                                           std::string& rest)
{
    for (const interfaces::LocalTool& t : tools)
        for (size_t at = text.find(t.name + "("); at != std::string::npos; at = text.find(t.name + "(", at + 1)) {
            if (at > 0 && (std::isalnum(static_cast<unsigned char>(text[at - 1])) || text[at - 1] == '_'))
                continue;
            size_t i = at + t.name.size() + 1;
            model::Json args = model::Json::object();
            bool ok = true;
            auto space = [&]() {
                while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i])))
                    ++i;
            };
            space();
            while (ok && i < text.size() && text[i] != ')') {
                std::string key;
                while (i < text.size() && (std::isalnum(static_cast<unsigned char>(text[i])) || text[i] == '_'))
                    key += text[i++];
                space();
                if (key.empty() || i >= text.size() || (text[i] != '=' && text[i] != ':')) {
                    ok = false;
                    break;
                }
                ++i;
                space();
                std::string value;
                if (i < text.size() && (text[i] == '"' || text[i] == '\'')) {
                    const char quote = text[i++];
                    while (i < text.size() && text[i] != quote) {
                        if (text[i] == '\\' && i + 1 < text.size()) {
                            const char e = text[++i];
                            value += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                        } else {
                            value += text[i];
                        }
                        ++i;
                    }
                    if (i >= text.size()) {
                        ok = false;
                        break;
                    }
                    ++i;
                    args.set(key, value);
                } else {
                    while (i < text.size() && text[i] != ',' && text[i] != ')')
                        value += text[i++];
                    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
                        value.pop_back();
                    char* end = nullptr;
                    const double number = std::strtod(value.c_str(), &end);
                    if (!value.empty() && end && *end == '\0')
                        args.set(key, number);
                    else if (value == "true" || value == "false" || value == "True" || value == "False")
                        args.set(key, value[0] == 't' || value[0] == 'T');
                    else
                        args.set(key, value);
                }
                space();
                if (i < text.size() && text[i] == ',') {
                    ++i;
                    space();
                }
            }
            if (!ok || i >= text.size() || text[i] != ')' || args.members().empty())
                continue;
            size_t end = i + 1;
            size_t begin = at;
            if (begin > 0 && text[begin - 1] == '`' && end < text.size() && text[end] == '`') { // `call(...)`
                --begin;
                ++end;
            }
            rest = text.substr(0, begin) + text.substr(end);
            return interfaces::LocalToolCall{"text_call", t.name, args.dump()};
        }
    return std::nullopt;
}

// Every call written as text, in either form, in order; the text without them goes to `rest`.
std::vector<interfaces::LocalToolCall> callsInText(const std::string& text, const std::vector<interfaces::LocalTool>& tools, std::string& rest)
{
    std::vector<interfaces::LocalToolCall> calls;
    std::string current = text;
    for (int k = 0; k < 8; ++k) {
        std::string left;
        std::optional<interfaces::LocalToolCall> call = callInText(current, tools, left);
        if (!call)
            call = keywordCallInText(current, tools, left);
        if (!call)
            break;
        call->id = "text_call_" + std::to_string(k + 1);
        calls.push_back(std::move(*call));
        current = std::move(left);
    }
    // What is left of the text: lines that held only a call (a bullet, a backtick) go too.
    std::string kept;
    for (size_t from = 0; from <= current.size();) {
        size_t nl = current.find('\n', from);
        if (nl == std::string::npos)
            nl = current.size();
        const std::string line = current.substr(from, nl - from);
        from = nl + 1;
        if (!calls.empty() && line.find_first_not_of(" \t*-`") == std::string::npos && !line.empty())
            continue;
        kept += (kept.empty() ? "" : "\n") + line;
    }
    while (!kept.empty() && std::isspace(static_cast<unsigned char>(kept.back())))
        kept.pop_back();
    rest = kept;
    return calls;
}

// The user asks for something to be kept: "remember", "from now on", "always ...", "never ...".
bool asksToRemember(const std::string& prompt)
{
    std::string lower = prompt;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* cue : {"remember", "from now on", "always ", "never ", "don't forget", "do not forget", "keep in mind"})
        if (lower.find(cue) != std::string::npos)
            return true;
    return false;
}

bool hasTool(const std::vector<interfaces::LocalTool>& tools, const char* name)
{
    return std::any_of(tools.begin(), tools.end(), [&](const interfaces::LocalTool& t) { return t.name == name; });
}

bool calledTool(const std::vector<interfaces::LocalChatMessage>& messages, const char* name)
{
    for (const interfaces::LocalChatMessage& m : messages)
        for (const interfaces::LocalToolCall& c : m.toolCalls)
            if (c.name == name)
                return true;
    return false;
}

// The answer only says it will call one of the tools ("I will now call inspect_binary with ..."),
// without calling it.
bool announcesCall(const std::string& text, const std::vector<interfaces::LocalTool>& tools)
{
    std::string lower = text;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    static const char* const intents[] = {"i will now", "i will call", "i will use", "i will try", "i'll call", "i'll use",
                                          "i'll try", "i will check", "let me call", "let me use", "let me check", "next, i will",
                                          "i will first", "i need to call", "i need to use", "i'll now", "i will read", "i'll read"};
    size_t at = std::string::npos;
    for (const char* intent : intents)
        at = std::min(at, lower.rfind(intent));
    if (at == std::string::npos)
        return false;
    for (const interfaces::LocalTool& t : tools)
        if (lower.find(t.name, at) != std::string::npos || lower.find("tool", at) != std::string::npos)
            return true;
    return false;
}

// The end of the JSON object starting at `from` ('{'), strings respected; npos when it does not close.
size_t objectEnd(const std::string& text, size_t from)
{
    int depth = 0;
    bool inString = false;
    for (size_t i = from; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\')
                ++i;
            else if (c == '"')
                inString = false;
        } else if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return i;
        }
    }
    return std::string::npos;
}

// A tool call the model wrote as text instead of in its chat format: `read_file {"path": "a"}`,
// `read_file({...})`, or {"name": "read_file", "arguments": {...}} (in a code fence or not). The
// text without it goes to `rest`.
std::optional<interfaces::LocalToolCall> callInText(const std::string& text, const std::vector<interfaces::LocalTool>& tools,
                                                    std::string& rest)
{
    for (size_t open = text.find('{'); open != std::string::npos; open = text.find('{', open + 1)) {
        const size_t close = objectEnd(text, open);
        if (close == std::string::npos)
            break;
        const std::optional<model::Json> j = model::Json::parse(std::string_view(text).substr(open, close - open + 1));
        if (!j || !j->isObject())
            continue;
        size_t begin = open, end = close + 1;
        interfaces::LocalToolCall call;
        // {"name": ..., "arguments": {...}}
        const std::string named = j->getString("name", j->getString("tool"));
        for (const interfaces::LocalTool& t : tools)
            if (t.name == named)
                for (const char* key : {"arguments", "args", "parameters", "input"})
                    if (const model::Json* a = j->find(key); a && a->isObject() && call.name.empty()) {
                        call.name = named;
                        call.arguments = a->dump();
                    }
        // tool_name {...} or tool_name({...})
        if (call.name.empty()) {
            size_t k = open;
            while (k > 0 && (text[k - 1] == ' ' || text[k - 1] == '(' || text[k - 1] == ':'))
                --k;
            for (const interfaces::LocalTool& t : tools)
                if (k >= t.name.size() && text.compare(k - t.name.size(), t.name.size(), t.name) == 0 &&
                    (k == t.name.size() || !std::isalnum(static_cast<unsigned char>(text[k - t.name.size() - 1])))) {
                    call.name = t.name;
                    call.arguments = j->dump();
                    begin = k - t.name.size();
                    if (end < text.size() && text[end] == ')')
                        ++end;
                }
        }
        if (call.name.empty())
            continue;
        call.id = "text_call";
        // Drop the call and a code fence around it.
        size_t fenceStart = text.rfind("```", begin);
        if (fenceStart != std::string::npos && text.find_first_not_of(" \t\r\njsonJSON`", fenceStart) >= begin)
            begin = fenceStart;
        if (const size_t fenceEnd = text.find("```", end); fenceEnd != std::string::npos && text.find_first_not_of(" \t\r\n", end) == fenceEnd)
            end = fenceEnd + 3;
        rest = text.substr(0, begin) + text.substr(end);
        while (!rest.empty() && std::isspace(static_cast<unsigned char>(rest.back())))
            rest.pop_back();
        return call;
    }
    return std::nullopt;
}

// A tool step as the chat shows it: "read_file  src/main.cpp", with the arguments' values.
std::string describeCall(const interfaces::LocalToolCall& call)
{
    std::string args;
    if (std::optional<model::Json> j = model::Json::parse(call.arguments); j && j->isObject())
        for (const auto& [key, value] : j->members()) {
            const std::string v = value.isString() ? value.asString() : value.dump();
            if (key == "content" || key == "new_text" || key == "old_text")
                continue; // edits show as changes to review
            args += (args.empty() ? "" : ", ") + v;
        }
    if (args.size() > 100)
        args = args.substr(0, 97) + "...";
    return "\xe2\x9a\x99 " + call.name + (args.empty() ? std::string() : "  " + args);
}

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

const std::vector<const char*>& AgentViewModel::choiceNames()
{
    static const std::vector<const char*> names = {"Auto", "Claude", "Local"};
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
    lastContext_ = context;
    busy.set(true);
    route(prompt, context);
}

void AgentViewModel::setLocal(interfaces::ILocalEngineService* engine, std::function<LocalSetup()> setup,
                              std::function<std::string()> contextText)
{
    local_ = engine;
    localSetup_ = std::move(setup);
    localContext_ = std::move(contextText);
}

bool AgentViewModel::localReady() const { return local_ && localSetup_ && localSetup_().ready; }

bool AgentViewModel::claudeReady() const { return live_ && cli_ && cli_->available() && !needsLogin_; }

std::string AgentViewModel::lastAnswerBy() const
{
    for (auto it = messages_.rbegin(); it != messages_.rend(); ++it)
        if (it->role == model::ChatRole::Agent)
            return it->by;
    return {};
}

// Who answers: the user's choice; in Auto, the rules of model::routeTask; and the local model when
// Claude Code cannot (if allowed). The sample workspace has no real Claude: a ready local model
// answers there instead of the made-up replies.
void AgentViewModel::route(const std::string& prompt, const std::string& context)
{
    const Choice choice = Choice(agentChoice.get());
    const bool local = localReady(), claude = claudeReady();
    if (choice == Choice::Local) {
        askLocal(prompt, context, {});
        return;
    }
    if (choice == Choice::Auto && local) {
        // A follow-up in a conversation Claude is having stays with Claude.
        const bool claudeConversation = claude && !sessionId_.empty() && lastAnswerBy() == "Claude";
        if (model::routeTask(prompt).route == model::AgentRoute::Local && !claudeConversation) {
            askLocal(prompt, context, {});
            return;
        }
    }
    if (!claude && local && (localFallback.get() || choice == Choice::Auto)) {
        askLocal(prompt, context, live_ ? "Claude Code is not available here, so the local model answers." : std::string());
        return;
    }
    askClaude(prompt, context);
}

void AgentViewModel::askClaude(const std::string& prompt, const std::string& context)
{
    claudeFrom_ = messages_.size();
    if (live_) {
        askCli(prompt, context);
        return;
    }
    replyAt_ = now_ + kReplyDelay; // the sample workspace: a made-up reply
}

void AgentViewModel::tagClaude()
{
    for (size_t i = claudeFrom_; i < messages_.size(); ++i)
        if (messages_[i].role == model::ChatRole::Agent && messages_[i].by.empty())
            messages_[i].by = "Claude";
}

bool AgentViewModel::fallBackToLocal(const std::string& prompt, const std::string& why)
{
    if (!localFallback.get() || !localReady())
        return false;
    askLocal(prompt, lastContext_, why + ", so the local model answers.");
    return true;
}

void AgentViewModel::setLocalTools(std::function<std::vector<interfaces::LocalTool>(bool canEdit)> tools, ToolRunner run,
                                   std::function<std::string()> guide)
{
    localTools_ = std::move(tools);
    runTool_ = std::move(run);
    localGuide_ = std::move(guide);
}

void AgentViewModel::proposeChange(const std::string& path, const std::string& before, const std::string& after)
{
    model::FileChange c;
    c.path = path;
    c.before = before;
    c.after = after;
    c.diff = model::diffLines(model::splitLines(before), model::splitLines(after));
    c.stats = model::diffStats(c.diff);
    // A newer proposal for the same file replaces one still waiting: one version to review.
    changes_.erase(std::remove_if(changes_.begin(), changes_.end(),
                                  [&](const model::FileChange& o) { return o.path == path && o.state == model::ChangeState::Proposed; }),
                   changes_.end());
    changes_.push_back(std::move(c));
    selectedChange.set(int(changes_.size()) - 1);
}

void AgentViewModel::endLocal()
{
    os::mem::release(localRequest_.messages); // pictures and sounds can be megabytes
    os::mem::release(localRequest_.tools);
    localRounds_ = 0;
    localNudges_ = 0;
    os::mem::release(localSteps_);
    localStep_ = 0;
}

std::vector<std::string> AgentViewModel::planSteps(const std::string& plan)
{
    std::vector<std::string> steps;
    auto space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    for (size_t start = 0; start <= plan.size() && steps.size() < kMaxPlanSteps;) {
        size_t end = plan.find('\n', start);
        if (end == std::string::npos)
            end = plan.size();
        std::string line = plan.substr(start, end - start);
        start = end + 1;
        line.erase(std::remove(line.begin(), line.end(), '*'), line.end()); // **bold**
        size_t i = 0;
        while (i < line.size() && (space(line[i]) || line[i] == '#' || line[i] == '-'))
            ++i;
        if (line.size() - i > 5 && std::tolower(static_cast<unsigned char>(line[i])) == 's' &&
            std::tolower(static_cast<unsigned char>(line[i + 1])) == 't' && line.compare(i + 2, 3, "ep ") == 0)
            i += 5; // "Step 2: ..."
        const size_t digits = i;
        while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i])))
            ++i;
        if (i == digits || i >= line.size() || (line[i] != '.' && line[i] != ')' && line[i] != ':'))
            continue;
        ++i;
        while (i < line.size() && space(line[i]))
            ++i;
        std::string text = line.substr(i);
        while (!text.empty() && space(text.back()))
            text.pop_back();
        if (!text.empty())
            steps.push_back(std::move(text));
    }
    return steps;
}

void AgentViewModel::planLocal(int generation, std::string question)
{
    std::string instructions = localPlanner_ ? localPlanner_() : std::string();
    if (instructions.rfind("---", 0) == 0) // the skill's front matter
        if (const size_t end = instructions.find("\n---", 3); end != std::string::npos) {
            const size_t body = instructions.find('\n', end + 4);
            instructions.erase(0, body == std::string::npos ? instructions.size() : body + 1);
        }
    if (instructions.empty()) {
        sendLocal(generation);
        return;
    }
    // A request of its own, without tools: the model only writes the steps. The model's settings are
    // the question's, so the engine keeps what it has loaded.
    interfaces::LocalChatRequest plan;
    plan.modelPath = localRequest_.modelPath;
    plan.drafterPath = localRequest_.drafterPath;
    plan.projectorPath = localRequest_.projectorPath;
    plan.threads = localRequest_.threads;
    plan.useGpu = localRequest_.useGpu;
    plan.contextTokens = localRequest_.contextTokens;
    plan.maxTokens = 512;
    std::string tools;
    for (const interfaces::LocalTool& t : localRequest_.tools)
        tools += "- " + t.name + ": " + t.description.substr(0, t.description.find(". ")) + "\n";
    plan.messages.emplace_back("system", instructions + "\n\nThe tools you will have:\n" + tools);
    plan.messages.emplace_back("user", "Write the steps for this question:\n\n" + question);
    activity_ = "Local model: planning the steps";
    local_->chat(
        plan, [](const std::string&) {}, [](const std::string&) {}, // the plan is shown once it is read
        [this, generation](const interfaces::LocalChatResult& r) {
            if (generation != generation_)
                return; // stopped or replaced
            std::vector<std::string> steps = r.ok ? planSteps(r.text) : std::vector<std::string>();
            if (steps.size() >= 2 && !localRequest_.messages.empty()) {
                std::string list;
                for (size_t i = 0; i < steps.size(); ++i)
                    list += std::to_string(i + 1) + ". " + steps[i] + "\n";
                messages_.push_back({model::ChatRole::Tool, "Plan, one step at a time:\n" + list});
                messages_.push_back({model::ChatRole::Tool, "Step 1 of " + std::to_string(steps.size()) + ": " + steps[0]});
                localRequest_.messages.back().text += "\n\nWe will work through this in steps:\n" + list + "\nDo step 1 only now: " +
                                                      steps[0] + "\nUse your tools, then tell me what you found.";
                localSteps_ = std::move(steps);
                localStep_ = 1;
            }
            sendLocal(generation); // a one-step plan, or none: the question as it is
        });
}

bool AgentViewModel::nextLocalStep(int generation, const std::string& answer)
{
    if (localSteps_.empty() || localStep_ > localSteps_.size())
        return false;
    localRequest_.messages.emplace_back("assistant", answer);
    std::string next;
    if (localStep_ < localSteps_.size()) {
        const std::string number = std::to_string(localStep_ + 1);
        messages_.push_back({model::ChatRole::Tool,
                             "Step " + number + " of " + std::to_string(localSteps_.size()) + ": " + localSteps_[localStep_]});
        next = "Now step " + number + ": " + localSteps_[localStep_] + "\nUse your tools, then tell me what you found.";
    } else {
        messages_.push_back({model::ChatRole::Tool, "All steps done: the answer"});
        next = "All the steps are done. Now answer my question in full, from what the steps found, with the figures exactly "
               "as the tools gave them: " + localPrompt_;
        if (const std::string reminder = localReminder_ ? localReminder_() : std::string(); !reminder.empty())
            next += "\n\n(" + reminder + ")";
    }
    ++localStep_;
    localRounds_ = 0; // each step has its own rounds of tools
    localNudges_ = 0;
    localRequest_.messages.emplace_back("user", std::move(next));
    sendLocal(generation);
    return true;
}

void AgentViewModel::askLocal(const std::string& prompt, const std::string& context, const std::string& note)
{
    const LocalSetup setup = localSetup_ ? localSetup_() : LocalSetup{};
    if (!local_ || !setup.ready) {
        messages_.push_back({model::ChatRole::Tool,
                             "The local model is not ready: " +
                                 (setup.why.empty() ? std::string("download it in the agent set-up (the gear).") : setup.why)});
        activity_.clear();
        busy.set(false);
        replied.emit(prompt);
        return;
    }
    if (!note.empty())
        messages_.push_back({model::ChatRole::Tool, note});

    endLocal();
    interfaces::LocalChatRequest& request = localRequest_;
    request.modelPath = setup.modelPath;
    request.drafterPath = setup.drafterPath;
    request.projectorPath = setup.projectorPath;
    request.threads = setup.threads;
    request.useGpu = setup.gpu;
    const bool tools = runTool_ && localTools_;
    if (tools) {
        request.tools = localTools_(autonomyIndex.get() == 0);
        request.contextTokens = 0; // the largest that fits: documents, pictures and tool results take room
        request.maxTokens = 8192;  // a file written whole, a long summary
        std::string system = kLocalAgentPrompt;
        if (localGuide_)
            system += "\n\n" + localGuide_();
        request.messages.push_back({"system", std::move(system)});
    } else {
        request.contextTokens = 8192;
        request.maxTokens = 768;
        request.messages.push_back({"system", kLocalSystemPrompt});
    }
    // Earlier turns, the newest kept, within a budget (the local model reads about 100 tokens a
    // second on a phone). This question itself is sent last, with the code it is about.
    size_t end = messages_.size();
    while (end > 0 && messages_[end - 1].role != model::ChatRole::User)
        --end;
    if (end > 0)
        --end;
    std::vector<interfaces::LocalChatMessage> history;
    size_t chars = 0;
    for (size_t i = end; i-- > 0;) {
        const model::ChatMessage& m = messages_[i];
        if (m.role == model::ChatRole::Tool || m.text.empty())
            continue;
        std::string text = m.text;
        if (const size_t cut = text.find("\n[context: "); cut != std::string::npos)
            text.erase(cut);
        if (chars + text.size() > kLocalHistoryChars)
            break;
        chars += text.size();
        interfaces::LocalChatMessage turn;
        turn.role = m.role == model::ChatRole::User ? "user" : "assistant";
        turn.text = std::move(text);
        history.push_back(std::move(turn));
    }
    std::reverse(history.begin(), history.end());
    // The chat format wants the turns to alternate, starting with the user.
    while (!history.empty() && history.front().role != "user")
        history.erase(history.begin());
    for (interfaces::LocalChatMessage& m : history)
        if (request.messages.back().role != m.role)
            request.messages.push_back(std::move(m));
        else if (m.role == "assistant")
            request.messages.back().text = std::move(m.text); // a question taken in steps: its last answer
    if (request.messages.back().role == "user")
        request.messages.pop_back();
    std::string question = prompt;
    const std::string code = localContext_ ? localContext_() : std::string();
    if (!code.empty())
        question = "I am looking at " + (context.empty() ? std::string("this code") : context) + ":\n\n" + code + "\n\n" + prompt;
    else if (!context.empty())
        question = "I am looking at " + context + ".\n\n" + prompt;
    interfaces::LocalChatMessage asked;
    asked.role = "user";
    if (const std::string reminder = tools && localReminder_ ? localReminder_() : std::string(); !reminder.empty())
        question += "\n\n(" + reminder + ")";
    asked.text = std::move(question);
    request.messages.push_back(std::move(asked));

    localPrompt_ = prompt;
    const int generation = ++generation_;
    // A question of some length is split into steps first (one word or a greeting is not worth it).
    if (tools && localInSteps.get() && localPlanner_ && prompt.size() >= 20)
        planLocal(generation, context.empty() ? prompt : "I am looking at " + context + ".\n\n" + prompt);
    else
        sendLocal(generation);
}

void AgentViewModel::sendLocal(int generation)
{
    messages_.push_back({model::ChatRole::Agent, std::string(), "Local"});
    const size_t at = messages_.size() - 1;
    activity_ = "Local model";
    local_->chat(
        localRequest_,
        [this, generation](const std::string& stage) {
            if (generation == generation_)
                activity_ = "Local model: " + stage;
        },
        [this, generation, at](const std::string& piece) {
            if (generation == generation_ && at < messages_.size())
                messages_[at].text += piece;
        },
        [this, generation, at](const interfaces::LocalChatResult& r) { localAnswered(generation, at, r); });
}

void AgentViewModel::localAnswered(int generation, size_t at, const interfaces::LocalChatResult& r)
{
    if (generation != generation_)
        return; // stopped or replaced
    const bool empty = at < messages_.size() && messages_[at].text.empty() && r.text.empty();
    if (!r.ok) {
        if (empty)
            messages_.erase(messages_.begin() + long(at));
        messages_.push_back({model::ChatRole::Tool, "The local model failed: " + r.error});
    } else {
        if (at < messages_.size() && !r.text.empty())
            messages_[at].text = r.text; // the streamed text, exactly (a tool call's markup left out)
        // A call written as text (a small model slips out of its format now and then): run it all the same.
        if (r.toolCalls.empty() && runTool_ && !localRequest_.tools.empty()) {
            std::string rest;
            if (std::vector<interfaces::LocalToolCall> calls = callsInText(r.text, localRequest_.tools, rest); !calls.empty()) {
                interfaces::LocalChatResult fixed = r;
                fixed.text = rest;
                fixed.toolCalls = std::move(calls);
                if (at < messages_.size())
                    messages_[at].text = rest;
                localAnswered(generation, at, fixed);
                return;
            }
        }
        // The user asked it to remember something and it only said it would: tell it to save it.
        if (r.toolCalls.empty() && runTool_ && localNudges_ < 2 && asksToRemember(localPrompt_) &&
            hasTool(localRequest_.tools, "remember") && !calledTool(localRequest_.messages, "remember")) {
            ++localNudges_;
            localRequest_.messages.emplace_back("assistant", r.text);
            localRequest_.messages.emplace_back("user", "You have not saved it yet: call remember now, once for each thing to keep "
                                                        "(kind preference for how I like things).");
            sendLocal(generation);
            return;
        }
        // It says what it will call and stops (a small model does): tell it to go on, twice at most.
        if (r.toolCalls.empty() && runTool_ && localNudges_ < 2 && localRounds_ < kMaxToolRounds && announcesCall(r.text, localRequest_.tools)) {
            ++localNudges_;
            interfaces::LocalChatMessage said("assistant", r.text);
            localRequest_.messages.push_back(std::move(said));
            localRequest_.messages.emplace_back("user", "Go on: make that tool call now - call the tool, do not describe it.");
            sendLocal(generation);
            return;
        }
        if (!r.toolCalls.empty() && runTool_ && localRounds_ < kMaxToolRounds) {
            ++localRounds_;
            if (empty)
                messages_.erase(messages_.begin() + long(at));
            interfaces::LocalChatMessage turn;
            turn.role = "assistant";
            turn.text = r.text;
            turn.toolCalls = r.toolCalls;
            localRequest_.messages.push_back(std::move(turn));
            runLocalTools(generation, r.toolCalls, 0, {});
            return;
        }
        // A question taken in steps: this step is answered, give the next (or ask for the answer).
        if (r.toolCalls.empty() && !localSteps_.empty() && localStep_ <= localSteps_.size()) {
            if (empty)
                messages_.erase(messages_.begin() + long(at));
            nextLocalStep(generation, r.text);
            return;
        }
        if (!r.toolCalls.empty())
            messages_.push_back({model::ChatRole::Tool, "The local model stopped after " + std::to_string(kMaxToolRounds) +
                                                            " rounds of tools without an answer."});
        else if (empty)
            messages_.erase(messages_.begin() + long(at));
        char line[160];
        std::snprintf(line, sizeof(line), "Local model (%s): %d tokens, %.1f tokens/s, first token after %.1f s%s",
                      r.backend.empty() ? "CPU" : r.backend.c_str(), r.generatedTokens, r.tokensPerSecond,
                      r.firstTokenSeconds, r.loaded ? " (loaded the model)" : "");
        statusMessage.emit(line);
    }
    endLocal();
    activity_.clear();
    busy.set(false);
    replied.emit(localPrompt_);
}

void AgentViewModel::runLocalTools(int generation, std::vector<interfaces::LocalToolCall> calls, size_t next,
                                   std::vector<interfaces::LocalMedia> seen)
{
    if (generation != generation_)
        return;
    if (next >= calls.size()) {
        if (!seen.empty()) {
            // Pictures and sounds a tool brought: the model takes them in as the next turn.
            interfaces::LocalChatMessage shown;
            shown.role = "user";
            for (const interfaces::LocalMedia& m : seen)
                shown.text += (shown.text.empty() ? "Here " : ", ") + std::string("is ") + m.name;
            shown.text += ", from your tools.";
            shown.media = std::move(seen);
            localRequest_.messages.push_back(std::move(shown));
        }
        sendLocal(generation);
        return;
    }
    const interfaces::LocalToolCall call = calls[next];
    messages_.push_back({model::ChatRole::Tool, describeCall(call)});
    const size_t step = messages_.size() - 1;
    activity_ = "Local model: " + call.name;
    runTool_(call, [this, generation, step, calls = std::move(calls), next, seen = std::move(seen)](ToolOutput out) mutable {
        if (generation != generation_)
            return;
        // The pictures the tool shows the user, under its step.
        for (const std::string& picture : out.shown) {
            std::string link; // a space would end a Markdown link
            for (char c : picture)
                link += c == ' ' ? std::string("%20") : std::string(1, c);
            if (step < messages_.size())
                messages_[step].text += "\n\n![" + picture.substr(picture.find_last_of("/\\") + 1) + "](" + link + ")";
        }
        interfaces::LocalChatMessage result;
        result.role = "tool";
        result.toolCallId = calls[next].id;
        result.toolName = calls[next].name;
        result.text = std::move(out.text);
        if (result.text.size() > kToolTextChars)
            result.text = result.text.substr(0, kToolTextChars) + "\n[cut: ask for a smaller part]";
        localRequest_.messages.push_back(std::move(result));
        for (interfaces::LocalMedia& m : out.media)
            seen.push_back(std::move(m));
        runLocalTools(generation, std::move(calls), next + 1, std::move(seen));
    });
}

void AgentViewModel::askClaudeInstead(int answerIndex)
{
    if (busy.get() || !claudeReady() || answerIndex <= 0 || answerIndex >= int(messages_.size()))
        return;
    for (int i = answerIndex - 1; i >= 0; --i) {
        if (messages_[size_t(i)].role != model::ChatRole::User)
            continue;
        const std::string asked = messages_[size_t(i)].text;
        std::string prompt = asked, context;
        if (const size_t cut = prompt.find("\n[context: "); cut != std::string::npos) {
            context = prompt.substr(cut + 11);
            if (!context.empty() && context.back() == ']')
                context.pop_back();
            prompt.erase(cut);
        }
        // The question again, as the user's turn: Claude's answer follows it like any other.
        messages_.push_back({model::ChatRole::User, asked});
        pendingPrompt_ = prompt;
        lastContext_ = context;
        busy.set(true);
        askClaude(prompt, context);
        return;
    }
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

void AgentViewModel::workspaceChanged()
{
    if (busy.get()) {
        ++generation_;
        replyAt_ = -1.0;
        if (cli_)
            cli_->cancel();
        if (local_ && local_->chatting())
            local_->cancel();
        activity_.clear();
        busy.set(false);
    }
    endLocal();
}

void AgentViewModel::stop()
{
    if (!busy.get())
        return;
    replyAt_ = -1.0;
    ++generation_; // the CLI's answer, if it still comes, is dropped
    if (cli_)
        cli_->cancel();
    if (local_ && local_->chatting())
        local_->cancel();
    endLocal();
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
    if (local_)
        local_->unloadIfIdle(kLocalIdleUnload);
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
        tagClaude();
        if (!ok) {
            noteFailure(output);
            if (fallBackToLocal(prompt, "The agent failed"))
                return;
        }
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
            tagClaude();
            if (fallBackToLocal(prompt, "Claude Code did not answer"))
                return;
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
            tagClaude();
            if (fallBackToLocal(prompt, "Claude Code stopped")) {
                activity_.clear();
                streamingMessage_ = -1;
                return;
            }
        } else {
            needsLogin_ = false;
            if (messages_.empty() || messages_.back().role == model::ChatRole::User)
                messages_.push_back({model::ChatRole::Agent, event->getString("result")}); // nothing came before
        }
        char cost[64];
        std::snprintf(cost, sizeof(cost), "%.2f", event->getNumber("total_cost_usd"));
        statusMessage.emit("Agent answered in " + std::to_string(int(event->getNumber("num_turns"))) + " turn(s), $" + cost);
        tagClaude();
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
