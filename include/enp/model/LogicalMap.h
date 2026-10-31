// The program as a logical (functional) hierarchy rather than a file tree:
//   System -> Modules (functional layers) -> Components (header + implementation) -> Functions -> Steps
// with "uses" relations between them. A second root holds the user-facing Features of the program,
// each a flow of steps through components (provided by agent dissection).
// dissect() builds the architecture locally from code structure, includes and the authors' comments;
// an agent can refine it (summaries, responsibilities, features) through mergeAgentAnalysis().
#pragma once

#include "enp/model/Json.h"

#include <optional>
#include <string>
#include <vector>

namespace enp::model {

enum class LogicalKind { System, Module, Component, Function, Step, Feature };

const char* logicalKindName(LogicalKind kind);

struct CodeRef {
    std::string path;  // workspace-relative
    int startLine = -1; // 0-based; -1: the whole file
    int endLine = -1;
};

struct LogicalNode {
    std::string id;
    LogicalKind kind = LogicalKind::Module;
    std::string name;
    std::string summary;
    std::vector<std::string> responsibilities; // short bullet points (agent)
    std::vector<CodeRef> code;                 // where it lives
    std::string target;                        // feature steps: the component they run in
    std::string parent;
    std::vector<std::string> children;
};

struct LogicalEdge {
    std::string from;
    std::string to;
    std::string label; // "uses", "then"
    int weight = 1;
};

struct LogicalMap {
    static constexpr const char* kArchitectureRoot = "system";
    static constexpr const char* kFeaturesRoot = "features";

    std::vector<LogicalNode> nodes;
    std::vector<LogicalEdge> edges;
    std::string source; // "local" or "agent"

    const LogicalNode* find(const std::string& id) const;
    LogicalNode* find(const std::string& id);
    bool empty() const { return nodes.empty(); }

    // Relations between the children of `parent`, lifted from relations between their descendants.
    std::vector<LogicalEdge> edgesAmongChildren(const std::string& parent) const;
    // The deepest node whose code contains `line` of `path` (a component when no function matches).
    std::string nodeAt(const std::string& path, int line) const;
    std::vector<std::string> pathTo(const std::string& id) const; // root .. id

    Json toJson() const;
    static std::optional<LogicalMap> fromJson(const Json& json);
};

struct SourceFile {
    std::string path; // workspace-relative, '/' separated
    std::string text;
};

// Builds the architecture locally (no agent): modules from directories, components from
// header/implementation pairs, relations from includes/imports, summaries from comments and the
// README's architecture table.
LogicalMap dissect(const std::string& projectName, const std::vector<SourceFile>& files, const std::string& readme);

// The skeleton sent to an agent: modules and components with their files and current summaries.
std::string agentPrompt(const LogicalMap& map);
// Applies the agent's JSON answer (see agentPrompt for the schema); returns false when the
// answer cannot be used, leaving the map unchanged.
bool mergeAgentAnalysis(LogicalMap& map, const std::string& answer, std::string* error = nullptr);

} // namespace enp::model
