// Workspace files of other editors, read to open the same folder here: VS Code's
// .code-workspace (JSON with comments) and Visual Studio's solutions (.sln, and the XML .slnx).
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace enp::model {

struct WorkspaceImport {
    enum class Kind { CodeWorkspace, Solution };
    Kind kind = Kind::CodeWorkspace;
    std::string name;                 // the file's name without its extension
    std::string root;                 // the folder to open: absolute, '/' separated
    std::vector<std::string> folders; // a multi-root workspace's folders, `root` first (absolute)
    std::vector<std::string> projects; // a solution's project files (absolute)
    // VS Code settings of the workspace that apply here.
    std::optional<int> tabSize;       // editor.tabSize
    std::optional<bool> insertSpaces; // editor.insertSpaces
};

// .code-workspace, .sln or .slnx (by extension, any case).
bool isWorkspaceFile(std::string_view path);

// `path` is the workspace file's absolute path ('/' or '\' separated); relative folders and
// projects are resolved against its folder. Nullopt (and `error`) when the text is not one.
std::optional<WorkspaceImport> parseWorkspaceFile(const std::string& path, std::string_view text, std::string* error = nullptr);

// Helpers, exposed for tests.
std::string stripJsonComments(std::string_view text); // removes // and /* */ comments and trailing commas
std::string normalizePath(const std::string& path);   // '/' separators, "." and ".." resolved
std::string commonFolder(const std::vector<std::string>& folders); // the deepest folder holding them all

} // namespace enp::model
