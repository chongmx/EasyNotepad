// In-memory sample workspace used while the view-models run without services.
// Everything here is placeholder content for exercising the UI.
#pragma once

#include "enp/model/Agent.h"
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
// Canned agent answer for the n-th prompt of the session.
std::string agentReply(int n, const std::string& prompt);

} // namespace enp::viewmodel::dummy
