#include "enp/model/Review.h"

#include <cctype>

#include <algorithm>

namespace enp::model {

const char* severityName(Severity severity)
{
    switch (severity) {
    case Severity::Info: return "Info";
    case Severity::Warning: return "Warning";
    case Severity::Error: return "Error";
    }
    return "";
}

const char* reviewToolName(ReviewTool tool)
{
    switch (tool) {
    case ReviewTool::Lint: return "Lint";
    case ReviewTool::StaticAnalysis: return "Static Analysis";
    case ReviewTool::Security: return "Security";
    case ReviewTool::Style: return "Style";
    case ReviewTool::Complexity: return "Complexity";
    case ReviewTool::AgentReview: return "Agent Review";
    }
    return "";
}

const char* reviewToolDescription(ReviewTool tool)
{
    switch (tool) {
    case ReviewTool::Lint: return "Lint checks for suspicious constructs (clang-tidy, ruff, eslint)";
    case ReviewTool::StaticAnalysis: return "Data-flow and bug-pattern analysis (cppcheck, clang analyzer)";
    case ReviewTool::Security: return "Security scan for unsafe APIs, secrets and injection risks";
    case ReviewTool::Style: return "Formatting and naming conventions (clang-format, black)";
    case ReviewTool::Complexity: return "Cyclomatic complexity, function length and nesting depth";
    case ReviewTool::AgentReview: return "AI review of the change: logic, naming, tests and edge cases";
    }
    return "";
}

std::vector<ReviewFinding> parseToolOutput(const std::string& output, ReviewTool tool, const std::string& workspaceRoot)
{
    std::vector<ReviewFinding> findings;
    std::string root = workspaceRoot;
    std::replace(root.begin(), root.end(), '\\', '/');
    size_t start = 0;
    while (start < output.size()) {
        size_t end = output.find('\n', start);
        if (end == std::string::npos)
            end = output.size();
        std::string line = output.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        // path:line[:col]: rest -- the path may contain a drive letter ("C:/x.cpp:3:1: ...").
        const size_t from = line.size() > 2 && line[1] == ':' ? 2 : 0;
        size_t c1 = line.find(':', from);
        if (c1 == std::string::npos || c1 == 0)
            continue;
        size_t c2 = c1 + 1;
        while (c2 < line.size() && std::isdigit(static_cast<unsigned char>(line[c2])))
            ++c2;
        if (c2 == c1 + 1 || c2 >= line.size() || line[c2] != ':')
            continue; // no line number
        ReviewFinding f;
        f.tool = tool;
        f.file = line.substr(0, c1);
        f.line = std::max(0, std::stoi(line.substr(c1 + 1, c2 - c1 - 1)) - 1);
        size_t rest = c2 + 1;
        size_t c3 = rest;
        while (c3 < line.size() && std::isdigit(static_cast<unsigned char>(line[c3])))
            ++c3;
        if (c3 > rest && c3 < line.size() && line[c3] == ':')
            rest = c3 + 1; // column
        std::string message = line.substr(rest);
        message.erase(0, message.find_first_not_of(' '));
        // Severity word, if present.
        f.severity = Severity::Warning;
        for (const auto& [word, severity] : {std::pair<const char*, Severity>{"fatal error:", Severity::Error},
                                             {"error:", Severity::Error},
                                             {"warning:", Severity::Warning},
                                             {"note:", Severity::Info},
                                             {"info:", Severity::Info},
                                             {"style:", Severity::Info}}) {
            const std::string w = word;
            if (message.compare(0, w.size(), w) == 0) {
                f.severity = severity;
                message = message.substr(w.size());
                message.erase(0, message.find_first_not_of(' '));
                break;
            }
        }
        // "[rule-name]" at the end (clang-tidy, gcc -W flags).
        if (!message.empty() && message.back() == ']') {
            const size_t open = message.rfind(" [");
            if (open != std::string::npos) {
                f.rule = message.substr(open + 2, message.size() - open - 3);
                message.resize(open);
            }
        }
        f.message = message;
        std::replace(f.file.begin(), f.file.end(), '\\', '/');
        if (!root.empty() && f.file.compare(0, root.size() + 1, root + "/") == 0)
            f.file = f.file.substr(root.size() + 1);
        if (f.file.compare(0, 2, "./") == 0)
            f.file = f.file.substr(2);
        if (!f.message.empty())
            findings.push_back(std::move(f));
    }
    return findings;
}

} // namespace enp::model
