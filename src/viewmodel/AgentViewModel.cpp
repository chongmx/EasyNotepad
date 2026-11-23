#include "enp/viewmodel/AgentViewModel.h"

#include "enp/model/AgentRoute.h"
#include "enp/model/Diff.h"
#include "enp/model/Json.h"
#include "enp/os/Memory.h"
#include "enp/viewmodel/DummyData.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
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
    "Markdown, naming the files and pages you used. If a tool fails, say so or try another way. "
    "Never work out a number yourself - not a sum, difference, total, average, percentage, share, "
    "conversion or rounding, however small: call calculate (several sums at once, separated by ';') "
    "and give the figures it returns. Only counting a few things you can see one by one is yours to do: "
    "a total of quantities (3 x pens, 2 x folders) is a sum for calculate. Write figures as digits. Answer "
    "what was asked, in the question's words: \"how many are still free\" is not how many there are.";

// Said with each step of a question taken in steps.
constexpr const char* kStepAsk = "Use your tools, then tell me what you found. If a tool finds nothing or says the call is "
                                  "wrong, fix the call from what it tells you and try again before you report.";

constexpr size_t kToolTextChars = 12000; // of one tool result sent to the model (about 3,000 tokens)
constexpr size_t kRepeatChars = 1500;  // a tool reply longer than this, already in the context, is pointed to
constexpr float kLoopTemperature = 0.3f;  // once it repeats a call: some variety, out of the loop

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

// The last tool result (since the user's last turn) says how to fix the call: no rows matched
// (with the values to use), no such column, a missing argument, no such tool, the wrong tool for
// the file (with the right call), a region it cannot read.
bool toolAsksToRetry(const std::vector<interfaces::LocalChatMessage>& messages)
{
    for (size_t i = messages.size(); i-- > 0;) {
        const interfaces::LocalChatMessage& m = messages[i];
        if (m.role == "user" && m.media.empty())
            return false;
        if (m.role != "tool")
            continue;
        for (const char* cue : {"no rows match", "There is no column", "Call it like this", "There is no tool called",
                                "It is a document: call", "It is a picture: call", "It is a recording: call",
                                "cannot read the region", "does not exist. ", "is a table, not a document", "There is no sheet", "as a bare number",
                                "} for its text."})
            if (m.text.find(cue) != std::string::npos)
                return true;
        return false;
    }
    return false;
}

// ---- Figures no tool gave ----------------------------------------------------------------------
// The local model is fine at counting a few things and poor at anything else with numbers: a figure
// in its answer that no tool returned and the user did not give was worked out in its head (or
// made up). Such an answer goes back to it, to be done with calculate.

struct Figure {
    double value = 0;
    int decimals = 0;
    bool scaled = false; // "3.78 million": compared loosely
    std::string text;
};

std::vector<Figure> figuresIn(const std::string& text)
{
    std::vector<Figure> out;
    auto digit = [&](size_t i) { return i < text.size() && std::isdigit(static_cast<unsigned char>(text[i])); };
    auto letter = [&](size_t i) { return i < text.size() && (std::isalpha(static_cast<unsigned char>(text[i])) || text[i] == '_'); };
    for (size_t i = 0; i < text.size();) {
        if (!digit(i) || (i > 0 && (letter(i - 1) || digit(i - 1) || text[i - 1] == '#' || text[i - 1] == '.'))) {
            ++i;
            continue;
        }
        const size_t start = i;
        std::string n;
        int decimals = 0;
        bool point = false;
        while (i < text.size()) {
            if (digit(i)) {
                n += text[i];
                decimals += point;
                ++i;
            } else if (text[i] == ',' && digit(i + 1) && digit(i + 2) && digit(i + 3) && !digit(i + 4) && !point) {
                ++i; // a thousands separator
            } else if (text[i] == '.' && !point && digit(i + 1)) {
                n += '.';
                point = true;
                ++i;
            } else {
                break;
            }
        }
        // Not a quantity: part of a word or name (x64, v1.2), a date or time, a range's ends.
        const bool joined = letter(i) || (i < text.size() && (text[i] == ':' || (text[i] == '-' && digit(i + 1)))) ||
                            (start > 0 && (text[start - 1] == ':' || (text[start - 1] == '-' && start > 1 && digit(start - 2))));
        if (joined)
            continue;
        Figure f;
        f.value = std::atof(n.c_str());
        f.decimals = decimals;
        f.text = text.substr(start, i - start);
        std::string next;
        for (size_t k = i; k < text.size() && next.size() < 9; ++k)
            next += char(std::tolower(static_cast<unsigned char>(text[k])));
        for (const auto& [word, factor] : {std::pair<const char*, double>{" million", 1e6}, {" billion", 1e9}, {" thousand", 1e3},
                                           {"m ", 1e6}, {"k ", 1e3}, {"bn", 1e9}})
            if (next.rfind(word, 0) == 0) {
                f.value *= factor;
                f.scaled = true;
                break;
            }
        out.push_back(std::move(f));
    }
    return out;
}

// Numbers written in words ("forty two thousand", "nine hundred and five"): a transcript says them so,
// and an answer giving them as digits did not make them up.
std::vector<Figure> spelledFigures(const std::string& text)
{
    static const char* const units[] = {"zero", "one", "two", "three", "four", "five", "six", "seven", "eight", "nine", "ten",
                                        "eleven", "twelve", "thirteen", "fourteen", "fifteen", "sixteen", "seventeen", "eighteen",
                                        "nineteen"};
    static const char* const tens[] = {"twenty", "thirty", "forty", "fifty", "sixty", "seventy", "eighty", "ninety"};
    std::vector<Figure> out;
    double total = 0, current = 0;
    bool active = false;
    auto flush = [&]() {
        if (active) {
            Figure f;
            f.value = total + current;
            f.text = std::to_string(static_cast<long long>(f.value));
            out.push_back(std::move(f));
        }
        total = current = 0;
        active = false;
    };
    std::string word;
    for (size_t i = 0; i <= text.size(); ++i) {
        const char c = i < text.size() ? char(std::tolower(static_cast<unsigned char>(text[i]))) : ' ';
        if (std::isalpha(static_cast<unsigned char>(c))) {
            word += c;
            continue;
        }
        if (word.empty()) {
            if (c != ' ' && c != '-')
                flush(); // punctuation ends a number
            continue;
        }
        double value = -1;
        for (size_t k = 0; k < std::size(units); ++k)
            if (word == units[k])
                value = double(k);
        for (size_t k = 0; k < std::size(tens); ++k)
            if (word == tens[k])
                value = double(20 + 10 * k);
        if (value >= 0) {
            current += value;
            active = true;
        } else if (word == "hundred" && active) {
            current = (current == 0 ? 1 : current) * 100;
        } else if ((word == "thousand" || word == "million" || word == "billion") && active) {
            total += (current == 0 ? 1 : current) * (word == "thousand" ? 1e3 : word == "million" ? 1e6 : 1e9);
            current = 0;
        } else if (!(word == "and" && active)) {
            flush();
        }
        word.clear();
        if (c != ' ' && c != '-')
            flush();
    }
    flush();
    return out;
}

// The answer's figures that no tool result or user message holds (as given, or rounded, or as a
// percentage of it). Small whole numbers (counting) and years are left alone. When the model was
// given pictures or sounds, figures read from them cannot be checked: only lines that work
// something out ("=", an operator between numbers) are.
std::vector<std::string> unsourcedFigures(const std::string& answer, const std::vector<interfaces::LocalChatMessage>& messages,
                                          bool sawMedia)
{
    std::vector<Figure> known;
    bool media = sawMedia;
    for (const interfaces::LocalChatMessage& m : messages) {
        if (m.role != "tool" && m.role != "user")
            continue;
        media = media || !m.media.empty();
        for (Figure& f : figuresIn(m.text))
            known.push_back(std::move(f));
        for (Figure& f : spelledFigures(m.text))
            known.push_back(std::move(f));
    }
    auto sourced = [&](const Figure& a) {
        for (const Figure& k : known)
            for (const double v : {k.value, k.value * 100.0, k.value / 100.0}) {
                if (a.scaled || k.scaled) {
                    if (std::abs(a.value - v) <= 0.01 * std::max(std::abs(a.value), std::abs(v)))
                        return true;
                } else if (std::abs(a.value - v) <= 0.5 * std::pow(10.0, -a.decimals) + 1e-9 * std::max(1.0, std::abs(v))) {
                    return true;
                }
            }
        return false;
    };
    std::vector<std::string> out;
    size_t at = 0;
    while (at < answer.size()) {
        size_t end = answer.find('\n', at);
        if (end == std::string::npos)
            end = answer.size();
        const std::string line = answer.substr(at, end - at);
        at = end + 1;
        bool works = line.find('=') != std::string::npos;
        for (size_t i = 1; i + 1 < line.size() && !works; ++i)
            if (std::strchr("+*/x", line[i]) && (std::isdigit(static_cast<unsigned char>(line[i - 1])) || line[i - 1] == ' ') &&
                (std::isdigit(static_cast<unsigned char>(line[i + 1])) || line[i + 1] == ' ')) {
                const size_t l = line.find_last_not_of(' ', i - 1), r = line.find_first_not_of(' ', i + 1);
                works = l != std::string::npos && r != std::string::npos && std::isdigit(static_cast<unsigned char>(line[l])) &&
                        std::isdigit(static_cast<unsigned char>(line[r]));
            }
        if (media && !works)
            continue;
        for (const Figure& f : figuresIn(line)) {
            const bool small = f.decimals == 0 && !f.scaled && std::abs(f.value) <= 12;
            const bool year = f.decimals == 0 && f.value >= 1900 && f.value <= 2100 && f.text.find(',') == std::string::npos;
            if (!small && !year && !sourced(f) && std::find(out.begin(), out.end(), f.text) == out.end())
                out.push_back(f.text);
        }
    }
    return out;
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
    // Its last sentence is what it means to do next ("Let me read the file in smaller chunks."): a
    // step not taken, whatever it names.
    {
        size_t end = lower.find_last_not_of(" \t\r\n*");
        if (end != std::string::npos && (lower[end] == '.' || lower[end] == ':' || lower[end] == '!'))
            --end;
        // A sentence ends at a stop followed by a space, not at the dot of a file name ("notice.eml").
        size_t stop = std::string::npos;
        for (size_t i = end == std::string::npos ? 0 : end + 1; end != std::string::npos && i-- > 0;)
            if (lower[i] == '\n' || ((lower[i] == '.' || lower[i] == '!' || lower[i] == '?') && i + 1 < lower.size() &&
                                     std::isspace(static_cast<unsigned char>(lower[i + 1])))) {
                stop = i;
                break;
            }
        size_t start = stop == std::string::npos ? 0 : stop + 1;
        while (start < lower.size() && (std::isspace(static_cast<unsigned char>(lower[start])) || lower[start] == '*'))
            ++start;
        const std::string last = lower.substr(start);
        for (const char* next : {"let me ", "i'll ", "i will ", "now i'll ", "now let me ", "next, let me ", "next, i'll "})
            if (last.rfind(next, 0) == 0)
                return true;
    }
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

// The paths of the files a question names, from what the app told the model ("..., the files my
// question names: audio/a.md (text: read_file); data/b.csv (a table: query_table).").
std::vector<std::string> namedPaths(const std::string& named)
{
    std::vector<std::string> out;
    const size_t colon = named.find("names:");
    if (colon == std::string::npos)
        return out;
    std::string rest = named.substr(colon + 6);
    if (!rest.empty() && rest.back() == '.')
        rest.pop_back();
    for (size_t from = 0; from <= rest.size();) {
        size_t end = rest.find(';', from);
        if (end == std::string::npos)
            end = rest.size();
        std::string part = rest.substr(from, end - from);
        from = end + 1;
        if (const size_t use = part.rfind(" ("); use != std::string::npos)
            part.erase(use);
        const size_t first = part.find_first_not_of(' ');
        if (first != std::string::npos)
            out.push_back(part.substr(first));
    }
    return out;
}

// The file a reading tool reads ("path" of read_file, read_document, look_at_image ...), as the
// folder-relative path the app names files with; empty for any other call.
std::string readPath(const interfaces::LocalToolCall& call)
{
    static const char* const reading[] = {"read_file", "read_document", "look_at_image", "look_at_page", "query_table",
                                          "listen_to", "transcribe", "inspect_binary", "scan_document", "scan_image"};
    if (std::none_of(std::begin(reading), std::end(reading), [&](const char* r) { return call.name == r; }))
        return {};
    const std::optional<model::Json> args = model::Json::parse(call.arguments);
    std::string path = args ? args->getString("path") : std::string();
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.rfind("./", 0) == 0)
        path.erase(0, 2);
    return path;
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
    os::mem::release(localNamed_);
    localNamedRead_ = false;
    os::mem::release(localStepSkills_);
    os::mem::release(localStepFacts_);
    os::mem::release(localCallCounts_);
    os::mem::release(localQuestion_);
    os::mem::release(localLongTerm_);
    os::mem::release(localKeep_);
    os::mem::release(localFound_);
    os::mem::release(localEvidence_);
    localBase_ = 0;
    localSawMedia_ = false;
    localToolsUsed_ = 0;
    if (localNamedFiles_)
        localNamedFiles_({}); // the tools forget the question too
}

void AgentViewModel::keepFacts(const interfaces::LocalToolCall& call, const std::string& result)
{
    // The lines that carry figures, by tool: not the text of documents (that is what was read, not
    // what was found).
    std::vector<std::string> lines;
    size_t from = 0;
    while (from < result.size()) {
        size_t end = result.find('\n', from);
        if (end == std::string::npos)
            end = result.size();
        std::string line = result.substr(from, end - from);
        from = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
            line.pop_back();
        if (line.empty() || line[0] == '(')
            continue; // the app's notes to the model
        const bool keep = call.name == "calculate"                ? line.find(" = ") != std::string::npos || line.find(": true") != std::string::npos ||
                                                                       line.find(": false") != std::string::npos
                          : call.name == "query_table"            ? line.find(" | ") != std::string::npos
                          : call.name == "list_files"             ? line.rfind("In all: ", 0) == 0 || line.rfind("Listed: ", 0) == 0
                          : call.name == "extract_from_documents" ? line.rfind("| ", 0) == 0 && line.find("---") == std::string::npos
                                                                  : false;
        if (keep && lines.size() < 12)
            lines.push_back(line.size() > 200 ? line.substr(0, 200) + "..." : line);
    }
    if (lines.empty())
        return;
    std::string called = describeCall(call);
    if (called.rfind("\xe2\x9a\x99 ", 0) == 0)
        called.erase(0, 4); // the chat's gear
    std::string fact = "- " + called + ":";
    for (const std::string& l : lines)
        fact += "\n  " + l;
    // The step's own, within a budget: shown to the model when it revises its memory after the step
    // (what to keep of it is the model's to decide).
    size_t total = fact.size();
    for (const std::string& f : localStepFacts_)
        total += f.size();
    if (total <= kStepFactsChars)
        localStepFacts_.push_back(std::move(fact));
}

int& AgentViewModel::callCount(const interfaces::LocalToolCall& call)
{
    const std::string key = call.name + "\n" + call.arguments;
    for (auto& [k, n] : localCallCounts_)
        if (k == key)
            return n;
    localCallCounts_.emplace_back(key, 0);
    return localCallCounts_.back().second;
}

bool AgentViewModel::copiedFrom(const std::string& source, const std::string& line)
{
    // Lower case, one space for any run of spaces and new lines, no bullets or quotes around.
    auto plain = [](const std::string& s) {
        std::string out;
        for (const char c : s) {
            const char l = char(std::tolower(static_cast<unsigned char>(c)));
            if (std::isspace(static_cast<unsigned char>(l))) {
                if (!out.empty() && out.back() != ' ')
                    out += ' ';
            } else {
                out += l;
            }
        }
        return out;
    };
    std::string l = plain(line);
    while (!l.empty() && (l.front() == '-' || l.front() == '*' || l.front() == ' ' || l.front() == '"'))
        l.erase(0, 1);
    while (!l.empty() && (l.back() == ' ' || l.back() == '"' || l.back() == '.'))
        l.pop_back();
    if (std::count_if(l.begin(), l.end(), [](unsigned char c) { return std::isalpha(c); }) < 20)
        return false;
    return plain(source).find(l) != std::string::npos;
}

std::vector<std::string> AgentViewModel::listUnder(const std::string& text, const std::string& heading)
{
    auto lowered = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return s;
    };
    const std::string want = lowered(heading);
    std::vector<std::string> out;
    bool in = false;
    size_t from = 0;
    while (from <= text.size()) {
        size_t end = text.find('\n', from);
        if (end == std::string::npos)
            end = text.size();
        std::string line = text.substr(from, end - from);
        from = end + 1;
        size_t i = 0;
        while (i < line.size() && (std::isspace(static_cast<unsigned char>(line[i])) || line[i] == '#'))
            ++i;
        line.erase(0, i);
        // "- ", "* ", "• " (before **bold** is taken out, which would take a "* " bullet with it)
        const bool bullet = line.rfind("- ", 0) == 0 || line.rfind("* ", 0) == 0 || line.rfind("\xe2\x80\xa2", 0) == 0;
        if (bullet)
            line.erase(0, line[0] == '\xe2' ? 3 : 1);
        line.erase(std::remove(line.begin(), line.end(), '*'), line.end()); // **bold**
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.front())))
            line.erase(line.begin());
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
            line.pop_back();
        if (line.empty())
            continue;
        if (!in) {
            const std::string low = lowered(line);
            if (low.rfind(want, 0) == 0) {
                in = true;
                // "Keep in mind: one thing" on the heading's line
                const std::string rest = line.substr(std::min(line.size(), want.size()));
                size_t r = rest.find_first_not_of(" :");
                if (r != std::string::npos && out.size() < kMemoryLines)
                    out.push_back(rest.substr(r));
            }
            continue;
        }
        if (!bullet)
            break; // the next heading, or the numbered steps
        if (out.size() < kMemoryLines)
            out.push_back(line.substr(0, 300));
    }
    return out;
}

std::string AgentViewModel::taskMemory(size_t now) const
{
    std::string m = "Your memory for this task (the app keeps it between the steps; it holds what you need from the "
                    "earlier ones):\n\n## My question\n" + localQuestion_ + "\n";
    if (!localKeep_.empty()) {
        m += "\n## Keep in mind\n";
        for (const std::string& k : localKeep_)
            m += "- " + k + "\n";
    }
    m += "\n## The plan\n";
    for (size_t i = 0; i < localSteps_.size(); ++i)
        m += std::to_string(i + 1) + ". " + localSteps_[i] + (i < now ? " (done)" : i == now ? " (now)" : "") + "\n";
    if (!localFound_.empty()) {
        m += "\n## Found so far\n";
        for (const std::string& f : localFound_)
            m += "- " + f + "\n";
    }
    if (!localLongTerm_.empty())
        m += "\n## From your long-term memory\n" + localLongTerm_ + "\n";
    return m;
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

std::vector<std::string> AgentViewModel::stepSkills(std::string& step)
{
    std::string lowered = step;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const size_t at = lowered.rfind("skills:");
    if (at == std::string::npos)
        return {};
    // The tag's start: its bracket ("[skills: ...]", "(skills: ...)") or the word itself.
    size_t start = at;
    while (start > 0 && lowered[start - 1] == ' ')
        --start;
    if (start > 0 && (lowered[start - 1] == '[' || lowered[start - 1] == '('))
        --start;
    std::string list = lowered.substr(at + 7);
    if (const size_t close = list.find_first_of("])"); close != std::string::npos)
        list.erase(close);
    std::vector<std::string> skills;
    std::string name;
    for (const char c : list + ",") {
        if (c == ',' || c == ';' || c == ' ') {
            if (!name.empty() && name != "none" && name != "and" && skills.size() < kMaxStepSkills &&
                std::find(skills.begin(), skills.end(), name) == skills.end())
                skills.push_back(name);
            name.clear();
        } else if (std::isalnum(static_cast<unsigned char>(c)) || c == '-') {
            name += c;
        }
    }
    step.erase(start);
    while (!step.empty() && (std::isspace(static_cast<unsigned char>(step.back())) || step.back() == '-' || step.back() == ','))
        step.pop_back();
    return skills;
}

std::string AgentViewModel::skillsFor(size_t step)
{
    if (!localSkill_ || step >= localStepSkills_.size())
        return {};
    std::string out;
    for (const std::string& name : localStepSkills_[step]) {
        // Each step has its own context (the task's memory and the step): its skills come with it,
        // even when an earlier step had them.
        const std::string text = localSkill_(name);
        if (text.empty())
            continue; // a skill the plan made up
        out += "\n\nYour " + name + " skill, for this step:\n" + text;
    }
    return out;
}

void AgentViewModel::planLocal(int generation, std::string question)
{
    std::string instructions = localPlanner_ ? localPlanner_(question) : std::string();
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
    plan.temperature = 0.0f; // the same plan for the same question
    std::string tools;
    for (const interfaces::LocalTool& t : localRequest_.tools)
        tools += "- " + t.name + ": " + t.description.substr(0, t.description.find(". ")) + "\n";
    plan.messages.emplace_back("system", instructions + "\n\nThe tools you will have:\n" + tools);
    plan.messages.emplace_back("user", "Write the steps for this question:\n\n" + question);
    activity_ = "Local model: planning the steps";
    local_->chat(
        plan, [](const std::string&) {}, [](const std::string&) {}, // the plan is shown once it is read
        [this, generation, instructions = std::move(instructions)](const interfaces::LocalChatResult& r) {
            if (generation != generation_)
                return; // stopped or replaced
            // What to keep in mind, then the steps (after "Steps:" when it wrote that: its list of what
            // to keep in mind may be numbered too).
            std::string stepsText = r.ok ? r.text : std::string();
            {
                std::string low = stepsText;
                std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                low.erase(std::remove(low.begin(), low.end(), '*'), low.end());
                const size_t at = low.rfind("steps:");
                if (at != std::string::npos && (at == 0 || low[at - 1] == '\n' || low[at - 1] == ' ' || low[at - 1] == '#')) {
                    // the same place in the text as written ('*' taken out of the lowered copy only)
                    size_t seen = 0, i = 0;
                    for (; i < stepsText.size() && seen < at; ++i)
                        seen += stepsText[i] != '*';
                    stepsText.erase(0, i);
                }
            }
            std::vector<std::string> steps = planSteps(stepsText);
            localKeep_ = r.ok ? listUnder(r.text, "Keep in mind") : std::vector<std::string>();
            // A line copied from the skill's examples ("cups only: ..." for a question with no cups)
            // is about the example, not the question: left out.
            localKeep_.erase(std::remove_if(localKeep_.begin(), localKeep_.end(),
                                            [&](const std::string& line) { return copiedFrom(instructions, line); }),
                             localKeep_.end());
            // The skills each step needs, out of its text: given with the step, not all up front.
            os::mem::release(localStepSkills_);
            for (std::string& step : steps)
                localStepSkills_.push_back(stepSkills(step));
            if (steps.size() >= 2 && !localRequest_.messages.empty()) {
                std::string shown;
                for (size_t i = 0; i < steps.size(); ++i) {
                    std::string skills;
                    for (const std::string& s : localStepSkills_[i])
                        skills += (skills.empty() ? " [skills: " : ", ") + s;
                    shown += std::to_string(i + 1) + ". " + steps[i] + (skills.empty() ? "" : skills + "]") + "\n";
                }
                std::string keep;
                for (const std::string& k : localKeep_)
                    keep += "- " + k + "\n";
                messages_.push_back({model::ChatRole::Tool, "Plan, one step at a time:\n" + shown +
                                                                (keep.empty() ? std::string() : "Keep in mind:\n" + keep)});
                messages_.push_back({model::ChatRole::Tool, "Step 1 of " + std::to_string(steps.size()) + ": " + steps[0]});
                localSteps_ = std::move(steps);
                localStep_ = 1;
                // Step 1 with the task's memory, in place of the question (the memory holds it).
                localRequest_.messages.erase(localRequest_.messages.begin() + long(std::min(localBase_, localRequest_.messages.size())),
                                             localRequest_.messages.end());
                const std::string memory = taskMemory(0);
                if (localTaskMemory_)
                    localTaskMemory_(memory);
                localRequest_.messages.emplace_back("user", memory + "\nDo step 1 only now: " + localSteps_[0] + "\n" + kStepAsk +
                                                                skillsFor(0));
            } else if (steps.size() == 1 && !localRequest_.messages.empty()) {
                localRequest_.messages.back().text += skillsFor(0); // the question as it is, with its skills
            }
            sendLocal(generation); // a one-step plan, or none: the question as it is
        });
}

bool AgentViewModel::nextLocalStep(int generation, const std::string& answer)
{
    if (localSteps_.empty() || localStep_ > localSteps_.size())
        return false;
    if (localStep_ == localSteps_.size() + 1)
        return false; // the answer was asked for: this is it
    reviseMemory(generation, answer); // then the next step, or the answer (stepOn)
    return true;
}

// After a step: the model revises the task's memory from what the step reported - what to keep in
// mind (corrected, added to, what is done with left out) and what the steps found - in a request of
// its own, without tools. Its figures stay checked against the tools' (the evidence), and the
// tools' own figures stay in the memory as they gave them.
void AgentViewModel::reviseMemory(int generation, const std::string& answer)
{
    const size_t done = localStep_ - 1;
    std::string stepFacts;
    for (const std::string& f : localStepFacts_)
        stepFacts += f + "\n";
    interfaces::LocalChatRequest revise;
    revise.modelPath = localRequest_.modelPath;
    revise.drafterPath = localRequest_.drafterPath;
    revise.projectorPath = localRequest_.projectorPath;
    revise.threads = localRequest_.threads;
    revise.useGpu = localRequest_.useGpu;
    revise.contextTokens = localRequest_.contextTokens;
    revise.maxTokens = 600;
    revise.temperature = 0.0f;
    revise.messages.emplace_back(
        "system",
        "You keep the memory of a task done in steps. From the memory and what the last step reported, write the "
        "memory for the rest of the task, in exactly this form and nothing else:\n"
        "Keep in mind:\n- (each detail the remaining steps and the answer need: the values the question picks by, the "
        "files, columns, units and what it asks for; correct any the step showed wrong, add any it learned, such as an "
        "exact column or file name; leave out what is done with)\n"
        "Found:\n- (each thing the steps found, one line each, with its figures exactly as the tools gave them and "
        "where it came from; keep the earlier ones unless the step showed them wrong; never work out a figure)");
    revise.messages.emplace_back("user", taskMemory(done) + "\nStep " + std::to_string(done + 1) + " (" + localSteps_[done] +
                                             ") reported:\n" + (answer.empty() ? std::string("(nothing)") : answer) +
                                             (stepFacts.empty() ? std::string() : "\n\nIts tools gave:\n" + stepFacts) +
                                             "\n\nNow write the memory.");
    activity_ = "Local model: updating the task's memory";
    local_->chat(
        revise, [](const std::string&) {}, [](const std::string&) {},
        [this, generation, answer, done](const interfaces::LocalChatResult& r) {
            if (generation != generation_)
                return; // stopped or replaced
            std::vector<std::string> keep = r.ok ? listUnder(r.text, "Keep in mind") : std::vector<std::string>();
            std::vector<std::string> found = r.ok ? listUnder(r.text, "Found") : std::vector<std::string>();
            // A revision that lost what was found (or failed): the step's report is added as it is.
            if (found.size() < localFound_.size() || found.empty()) {
                std::string report = answer.substr(0, 600);
                std::replace(report.begin(), report.end(), '\n', ' ');
                localFound_.push_back("Step " + std::to_string(done + 1) + ": " + report);
                if (localFound_.size() > kMemoryLines)
                    localFound_.erase(localFound_.begin());
            } else {
                localFound_ = std::move(found);
            }
            if (!keep.empty())
                localKeep_ = std::move(keep);
            stepOn(generation);
        });
}

void AgentViewModel::stepOn(int generation)
{
    os::mem::release(localStepFacts_);
    os::mem::release(localCallCounts_); // a call made at an earlier step may be made again
    // What the last step's tools returned, the newest first within a budget: the next step often
    // works on it (the transcript read, the rows found), and the memory holds only what the model
    // wrote down. Older steps reach it through the memory alone.
    std::vector<std::string> lastResults;
    size_t lastChars = 0;
    for (size_t k = localRequest_.messages.size(); k-- > std::min(localBase_ + 1, localRequest_.messages.size());) {
        const interfaces::LocalChatMessage& m = localRequest_.messages[k];
        if (m.role != "tool" || m.text.empty())
            continue;
        std::string text = m.text;
        if (lastChars + text.size() > kLastStepChars) {
            if (lastChars + 500 > kLastStepChars)
                break;
            text = text.substr(0, kLastStepChars - lastChars) + "\n[cut]";
        }
        lastChars += text.size();
        lastResults.insert(lastResults.begin(), "- " + m.toolName + ":\n" + text);
    }
    // A fresh context for each step: the messages before the question, then the memory and the step.
    localRequest_.messages.erase(localRequest_.messages.begin() + long(std::min(localBase_, localRequest_.messages.size())),
                                 localRequest_.messages.end());
    localRequest_.temperature = 0.0f; // a loop's variety was for the step it looped in
    const std::string memory = taskMemory(localStep_);
    if (localTaskMemory_)
        localTaskMemory_(memory);
    std::string next = memory;
    if (!lastResults.empty()) {
        next += "\n## What the last step's tools returned\n";
        for (const std::string& r : lastResults)
            next += r + "\n";
    }
    if (localStep_ < localSteps_.size()) {
        const std::string number = std::to_string(localStep_ + 1);
        messages_.push_back({model::ChatRole::Tool,
                             "Step " + number + " of " + std::to_string(localSteps_.size()) + ": " + localSteps_[localStep_]});
        next += "\nNow step " + number + ": " + localSteps_[localStep_] + "\n" + kStepAsk + skillsFor(localStep_);
    } else {
        messages_.push_back({model::ChatRole::Tool, "All steps done: the answer"});
        next += "\nAll the steps are done. Now answer my question in full, from the memory above, with the figures "
               "exactly as the tools gave them. Answer each thing it asks, one by one, in its words, with the figures "
               "each rests on: " + localPrompt_;
    }
    // Its working notes (the note tool), when it kept any.
    if (const std::string notes = localNotes_ ? localNotes_() : std::string(); !notes.empty())
        next += "\n\nYour notes so far:\n" + notes;
    ++localStep_;
    localRounds_ = 0; // each step has its own rounds of tools
    localNudges_ = 0;
    localRequest_.messages.emplace_back("user", std::move(next));
    sendLocal(generation);
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
    // With tools, the file open in the editor is only named (its text, pasted in, took over
    // questions about other files: it reads what it needs); lines the user selected still go in.
    const size_t colon = context.rfind(':');
    const bool selected = colon != std::string::npos && context.find('-', colon) != std::string::npos;
    const std::string code = localContext_ && (!tools || selected) ? localContext_() : std::string();
    // The files the question names; when it names some, the editor's file is not mentioned (the
    // model read a transcript left open by an earlier question instead of the one asked about).
    const std::string named = tools && localNamedFiles_ ? localNamedFiles_(prompt) : std::string();
    if (tools && code.empty() && !context.empty()) {
        if (named.empty())
            question = prompt + "\n\n(For reference only: the editor shows " + context.substr(0, colon) +
                       ". Read it with your tools only if my question is about it.)";
    }
    else if (!code.empty())
        question = "I am looking at " + (context.empty() ? std::string("this code") : context) + ":\n\n" + code + "\n\n" + prompt;
    else if (!context.empty())
        question = "I am looking at " + context + ".\n\n" + prompt;
    interfaces::LocalChatMessage asked;
    asked.role = "user";
    if (!named.empty())
        question += "\n\n(" + named + ")";
    localNamed_ = named;
    // With tools, the likeliest words every time: the same question gets the same calls and answer
    // (at 0.3 a small model took another plan or left out a condition now and then). Back to 0.3
    // when it repeats a call (greedy loops).
    request.temperature = tools ? 0.0f : -1.0f;
    localQuestion_ = question; // for the task's memory, when it is taken in steps
    if (const std::string reminder = tools && localReminder_ ? localReminder_() : std::string(); !reminder.empty()) {
        question += "\n\n(" + reminder + ")";
        localLongTerm_ = reminder;
    }
    asked.text = std::move(question);
    request.messages.push_back(std::move(asked));
    localBase_ = request.messages.size() - 1;
    // The answer's figures are checked against what the user wrote and what the tools gave.
    for (const interfaces::LocalChatMessage& m : request.messages)
        if (m.role == "user") {
            localEvidence_.emplace_back("user", m.text);
            localSawMedia_ = localSawMedia_ || !m.media.empty();
        }

    localPrompt_ = prompt;
    const int generation = ++generation_;
    // A question of some length is split into steps first (one word or a greeting is not worth it).
    if (tools && localInSteps.get() && localPlanner_ && prompt.size() >= 20)
        planLocal(generation, prompt); // the question alone: the file open in the editor misled plans
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
    // It went round in a loop, or wrote a tool call that is not one (a loop cut off, often): once more
    // with some variety, which takes it out of the loop, before the question fails.
    const bool slipped = r.error.find("repeating itself") != std::string::npos || r.error.find("tool call") != std::string::npos ||
                         r.error.find("nested more than") != std::string::npos;
    if (!r.ok && slipped && runTool_ && !localRequest_.tools.empty() && localRequest_.temperature == 0.0f) {
        if (at < messages_.size())
            messages_.erase(messages_.begin() + long(at)); // the loop's text, if any: not an answer
        messages_.push_back({model::ChatRole::Tool, "(The local model went wrong: " + r.error + ". Once more.)"});
        localRequest_.temperature = kLoopTemperature;
        sendLocal(generation);
        return;
    }
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
        // The question names files and it answered without reading anything ("there is no such file"):
        // back to it, to read them first.
        if (r.toolCalls.empty() && runTool_ && localNudges_ < 2 && !localNamed_.empty() && localToolsUsed_ == 0) {
            ++localNudges_;
            if (empty)
                messages_.erase(messages_.begin() + long(at));
            localRequest_.messages.emplace_back("assistant", r.text);
            localRequest_.messages.emplace_back("user", "You have not read anything yet. " + localNamed_ +
                                                            " Read them with those tools now, then answer from what they hold.");
            sendLocal(generation);
            return;
        }
        // A question not in steps whose last query did not pick rows by a value the question names
        // (said at the top of its reply), answered from it all the same: back to it, to query again.
        // (A step of a plan may want all rows: "the total", for a share.)
        if (r.toolCalls.empty() && runTool_ && localNudges_ < 2 && localRounds_ < kMaxToolRounds && localSteps_.empty()) {
            for (size_t k = localRequest_.messages.size(); k-- > 0;) {
                const interfaces::LocalChatMessage& m = localRequest_.messages[k];
                if (m.role != "tool" || m.toolName != "query_table")
                    continue;
                if (m.text.rfind("(My question names", 0) == 0) {
                    ++localNudges_;
                    localRequest_.messages.emplace_back("assistant", r.text);
                    localRequest_.messages.emplace_back("user", "Your last query_table call did not pick rows by what my question "
                                                                "names (the note at the top of its reply). Make the call again "
                                                                "with it in where, then answer from that.");
                    if (empty)
                        messages_.erase(messages_.begin() + long(at));
                    sendLocal(generation);
                    return;
                }
                break; // the last query: only it counts
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
        // A tool said how to fix the call and the model reported that instead of calling again.
        if (r.toolCalls.empty() && runTool_ && localNudges_ < 2 && localRounds_ < kMaxToolRounds &&
            toolAsksToRetry(localRequest_.messages)) {
            ++localNudges_;
            localRequest_.messages.emplace_back("assistant", r.text);
            localRequest_.messages.emplace_back("user", "Do not report that: the tool told you how to fix the call. Make the "
                                                        "corrected call now, with the values it listed.");
            sendLocal(generation);
            return;
        }
        // Figures no tool gave: worked out in its head, or made up. Back to it, to use calculate.
        if (r.toolCalls.empty() && runTool_ && localNudges_ < 2 && localRounds_ < kMaxToolRounds &&
            hasTool(localRequest_.tools, "calculate")) {
            if (const std::vector<std::string> figures = unsourcedFigures(r.text, localEvidence_, localSawMedia_); !figures.empty()) {
                ++localNudges_;
                std::string list;
                for (const std::string& f : figures)
                    list += (list.empty() ? "" : ", ") + f;
                localRequest_.messages.emplace_back("assistant", r.text);
                localRequest_.messages.emplace_back(
                    "user", "No tool gave you these figures: " + list +
                                ". Do not work numbers out yourself. Put each calculation into calculate now (several at "
                                "once, separated by ';', from the figures the tools gave), then answer with what it "
                                "returns. A figure you read in a file or picture: read it again with the tool instead.");
                if (at < messages_.size())
                    messages_.push_back({model::ChatRole::Tool, "Checking the figures " + list + " with the calculator"});
                sendLocal(generation);
                return;
            }
        }
        // Nothing at all after its tools (a small model stops so now and then): ask for the answer.
        if (r.toolCalls.empty() && runTool_ && localNudges_ < 2 &&
            std::all_of(r.text.begin(), r.text.end(), [](unsigned char c) { return std::isspace(c) != 0; }) &&
            localRequest_.messages.back().role == "tool") {
            ++localNudges_;
            if (empty)
                messages_.erase(messages_.begin() + long(at));
            localRequest_.messages.emplace_back("assistant", std::string());
            localRequest_.messages.emplace_back("user", "You have what the tools found: now answer my question from it, in full.");
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
        // The same call a third time in this step (the second time it was told not to): stuck. A step
        // of a plan ends there, saying so (its memory keeps that it could not be done); a question
        // not in steps is asked to answer from what it has.
        if (!r.toolCalls.empty() && runTool_) {
            const auto stuck = std::find_if(r.toolCalls.begin(), r.toolCalls.end(),
                                            [this](const interfaces::LocalToolCall& c) { return callCount(c) >= kStuckCalls - 1; });
            const bool inStep = !localSteps_.empty() && localStep_ <= localSteps_.size();
            if (stuck != r.toolCalls.end() && (inStep || localNudges_ < 2)) {
                std::string what = describeCall(*stuck);
                if (what.rfind("\xe2\x9a\x99 ", 0) == 0)
                    what.erase(0, 4);
                if (empty)
                    messages_.erase(messages_.begin() + long(at));
                if (inStep) {
                    messages_.push_back({model::ChatRole::Tool, "This step is stuck (the same call again: " + what + "): on to the next"});
                    nextLocalStep(generation, "I could not do this step: " + what + " gave the same reply each time I made it.");
                    return;
                }
                {
                    ++localNudges_;
                    localRequest_.messages.emplace_back("assistant", r.text);
                    localRequest_.messages.emplace_back("user", "You have made that same call three times and got the same reply. "
                                                                "Do not make it again: answer my question now from what you have, "
                                                                "and say what you could not find.");
                    sendLocal(generation);
                    return;
                }
            }
        }
        if (!r.toolCalls.empty() && runTool_ && localRounds_ < kMaxToolRounds) {
            ++localRounds_;
            // A call it made before in this step (or question), the same: greedy choice loops; some
            // variety from here on. The same file read again at a later step is no loop.
            size_t stepStart = localRequest_.messages.size();
            while (stepStart > 0 && !(localRequest_.messages[stepStart - 1].role == "user" &&
                                      localRequest_.messages[stepStart - 1].media.empty()))
                --stepStart;
            for (const interfaces::LocalToolCall& c : r.toolCalls)
                for (size_t k = stepStart; k < localRequest_.messages.size(); ++k)
                    for (const interfaces::LocalToolCall& earlier : localRequest_.messages[k].toolCalls)
                        if (earlier.name == c.name && earlier.arguments == c.arguments && localRequest_.temperature == 0.0f)
                            localRequest_.temperature = kLoopTemperature;
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
        char line[512];
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
        // What the tool answered, in short, under its step: the user sees what the answer rests on.
        if (step < messages_.size() && !out.text.empty()) {
            std::string gist;
            size_t at = 0;
            for (int lines = 0; at < out.text.size() && lines < (calls[next].name == "calculate" ? 4 : 1); ++lines) {
                size_t end = out.text.find('\n', at);
                if (end == std::string::npos)
                    end = out.text.size();
                gist += (gist.empty() ? "" : "; ") + out.text.substr(at, std::min<size_t>(end - at, 160));
                at = end + 1;
            }
            if (at < out.text.size())
                gist += " ...";
            messages_[step].text += "\n\xE2\x86\x92 " + gist; // →
        }
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
        keepFacts(calls[next], result.text);
        // The same call again in this step: said, so it changes something instead of looping.
        if (++callCount(calls[next]) == kStuckCalls - 1)
            result.text += "\n\n(You made this same call before in this step and got this same reply: do not make it "
                           "again. Change it - another column, value, file or tool - or answer from what you have.)";
        ++localToolsUsed_;
        localSawMedia_ = localSawMedia_ || !out.media.empty();
        localEvidence_.emplace_back("tool", result.text);
        if (result.text.size() > kToolTextChars)
            result.text = result.text.substr(0, kToolTextChars) + "\n[cut: ask for a smaller part]";
        // A long reply it already has, word for word, in what it sees now (a file read again with
        // other lines asked for, the same two lines): pointed to, not sent again. A bigger model's
        // context holds fewer tokens on the same card, and a 16 KB line read twice filled it.
        if (result.text.size() > kRepeatChars && out.media.empty())
            for (const interfaces::LocalChatMessage& m : localRequest_.messages)
                if (m.role == "tool" && m.text.rfind(result.text, 0) == 0) {
                    result.text = "(The same as the " + (m.toolName.empty() ? std::string("tool") : m.toolName) +
                                  " reply above, unchanged: you have it already. Answer from it, or take another step.)";
                    break;
                }
        // Another file read than the one the question names (the file left open in the editor):
        // said under its result, until it reads a named one.
        if (const std::string read = out.read.empty() ? readPath(calls[next]) : out.read;
            !read.empty() && !localNamed_.empty() && !localNamedRead_) {
            const std::vector<std::string> named = namedPaths(localNamed_);
            if (std::find(named.begin(), named.end(), read) != named.end()) {
                localNamedRead_ = true;
            } else if (!named.empty()) {
                std::string list;
                for (const std::string& n : named)
                    list += (list.empty() ? "" : ", ") + n;
                result.text += "\n\n(This is not the file my question is about: " + list + ". Read that one.)";
            }
        }
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
    ++conversation_;   // ... and the local model's working notes go
    messages_.clear();
    tasks_.clear();
    replyCount_ = 0;
    statusMessage.emit("Started a new agent conversation");
}

void AgentViewModel::workspaceChanged()
{
    ++conversation_; // notes about the old folder go
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
