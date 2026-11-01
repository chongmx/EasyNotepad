#include "enp/model/AgentRoute.h"

#include <algorithm>
#include <cctype>

namespace enp::model {

namespace {

bool has(const std::string& text, std::string_view phrase) { return text.find(phrase) != std::string::npos; }

// `word` as a whole word ("add" but not "address").
bool hasWord(const std::string& text, std::string_view word)
{
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
        const bool startOk = at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1]));
        const size_t end = at + word.size();
        const bool endOk = end >= text.size() || !std::isalnum(static_cast<unsigned char>(text[end]));
        if (startOk && endOk)
            return true;
    }
    return false;
}

} // namespace

RouteDecision routeTask(std::string_view prompt)
{
    std::string text(prompt);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    text.erase(0, text.find_first_not_of(" \t\r\n"));

    if (text.size() > 600)
        return {AgentRoute::Claude, "a long request"};

    // Questions and short writing about what is on screen: the local model is enough.
    static const char* const kLocal[] = {"commit message", "explain", "what does", "what is", "what are", "what's",
                                         "why does", "why is", "how does", "how do i", "summar", "describe",
                                         "meaning of", "translate", "tl;dr", "in plain words"};
    for (const char* phrase : kLocal)
        if (has(text, phrase))
            return {AgentRoute::Local, "a question about the code"};

    // Changing things, the whole project, or the app itself: Claude Code, which can do them.
    static const char* const kClaude[] = {"edit", "change", "fix", "refactor", "rename", "implement", "add", "create",
                                          "write", "test", "tests", "delete", "remove", "replace", "update", "move",
                                          "convert", "migrate", "rewrite", "optimize", "optimise", "debug", "plan",
                                          "design", "architecture", "project", "repo", "repository", "codebase",
                                          "build", "compile", "run", "install", "commit", "push", "merge", "click",
                                          "open", "point", "generate", "make", "review"};
    for (const char* word : kClaude)
        if (hasWord(text, word))
            return {AgentRoute::Claude, "a change or a larger task"};
    if (has(text, "all files") || has(text, "every file") || has(text, "across"))
        return {AgentRoute::Claude, "several files"};

    // A short question with none of the above.
    const bool question = !text.empty() && (text.back() == '?' || hasWord(text, "what") || hasWord(text, "why") ||
                                            hasWord(text, "how") || hasWord(text, "which") || hasWord(text, "where") ||
                                            hasWord(text, "who") || hasWord(text, "when") || hasWord(text, "is") ||
                                            hasWord(text, "does") || hasWord(text, "can"));
    if (question && text.size() <= 240)
        return {AgentRoute::Local, "a short question"};
    return {AgentRoute::Claude, "not clearly a quick question"};
}

} // namespace enp::model
