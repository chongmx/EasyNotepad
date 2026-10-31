// In-memory sample workspace used while the view-models run without services.
// Everything here is placeholder content for exercising the UI.
#pragma once

#include "enp/model/Agent.h"
#include "enp/model/Outline.h"
#include "enp/model/Review.h"
#include "enp/viewmodel/ExplorerItem.h"

#include <string>
#include <vector>

namespace enp::viewmodel::dummy {

inline constexpr const char* kWorkspaceName = "EasyDemo";
inline constexpr const char* kWelcomePath = "Welcome.md";

ExplorerItem workspace();
// Contents of a sample file; files that are not part of the sample get a placeholder text.
std::string fileContent(const std::string& path);
// 0-based line of the first line of `path` containing `marker`, or 0.
int lineOf(const std::string& path, const std::string& marker);

std::vector<model::ReviewFinding> findings(model::ReviewTool tool);
std::vector<model::ReviewComment> comments();
std::vector<model::ChecklistItem> checklist();

std::vector<model::ChatMessage> conversation();
std::vector<model::AgentTask> tasks();
std::vector<model::FileChange> changes();
// One-line "AI" summary of an outline region: canned for the sample files, heuristic otherwise.
// Stands in for the summarizer CLI.
// `parts`: names of the region's direct sub-regions (used for namespaces and types).
std::string summarize(const std::string& path, const model::OutlineNode& node, const model::TextBuffer& buffer,
                      const std::vector<std::string>& parts = {});
std::string summarizeFile(const std::string& path, const std::vector<model::OutlineNode>& outline);
// The comment at the top of a file (or a Markdown file's first paragraph), or the canned sample
// summary; empty when there is none. Cheap: no parsing.
std::string fileHeaderSummary(const std::string& path, const model::TextBuffer& buffer);
std::string summarizeFolder(const std::string& name, const std::vector<std::string>& children);

// Canned agent answer for the n-th prompt of the session.
std::string agentReply(int n, const std::string& prompt);

} // namespace enp::viewmodel::dummy
