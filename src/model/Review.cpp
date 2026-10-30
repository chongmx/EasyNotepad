#include "enp/model/Review.h"

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

} // namespace enp::model
