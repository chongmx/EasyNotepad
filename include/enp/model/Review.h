// Code review data: findings from review tools, inline comments and the review checklist.
#pragma once

#include <array>
#include <string>
#include <vector>

namespace enp::model {

enum class Severity { Info, Warning, Error };

enum class ReviewTool { Lint, StaticAnalysis, Security, Style, Complexity, AgentReview };
inline constexpr std::array<ReviewTool, 6> kAllReviewTools = {ReviewTool::Lint,  ReviewTool::StaticAnalysis,
                                                              ReviewTool::Security, ReviewTool::Style,
                                                              ReviewTool::Complexity, ReviewTool::AgentReview};

const char* severityName(Severity severity);
const char* reviewToolName(ReviewTool tool);        // "Lint"
const char* reviewToolDescription(ReviewTool tool); // one line for tooltips

struct ReviewFinding {
    ReviewTool tool = ReviewTool::Lint;
    Severity severity = Severity::Warning;
    std::string file;
    int line = 0; // 0-based
    std::string rule; // e.g. "modernize-use-nullptr"
    std::string message;
    bool resolved = false;
};

struct ReviewComment {
    std::string author;
    std::string file;
    int line = 0; // 0-based
    std::string text;
    bool resolved = false;
};

struct ChecklistItem {
    std::string text;
    bool done = false;
};

// Findings from compiler-style tool output: "path:line[:column]: [severity:] message [rule]" per
// line (gcc, clang, clang-tidy, flake8, ruff, eslint --format unix, shellcheck -f gcc, ...).
// Absolute paths under `workspaceRoot` become workspace-relative. Lines that do not match are
// ignored.
std::vector<ReviewFinding> parseToolOutput(const std::string& output, ReviewTool tool, const std::string& workspaceRoot);

} // namespace enp::model
