#include "enp/model/WorkspaceFile.h"

#include "enp/model/Json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>

namespace enp::model {

namespace {

std::string lower(std::string_view s)
{
    std::string out(s);
    for (char& c : out)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool endsWith(std::string_view s, std::string_view suffix)
{
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool isAbsolute(const std::string& path)
{
    return (!path.empty() && (path[0] == '/' || path[0] == '\\')) ||
           (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':');
}

std::string parentOf(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return {};
    if (slash == 0)
        return "/";
    if (slash == 2 && path[1] == ':')
        return path.substr(0, 3); // "C:/"
    return path.substr(0, slash);
}

std::string fileName(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string stem(const std::string& path)
{
    std::string name = fileName(path);
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

std::string resolve(const std::string& base, std::string path)
{
    // A file: URI as VS Code writes some folders ("file:///home/me/src").
    if (path.compare(0, 7, "file://") == 0) {
        path = path.substr(7);
        if (path.size() >= 3 && path[0] == '/' && std::isalpha(static_cast<unsigned char>(path[1])) && path[2] == ':')
            path = path.substr(1); // file:///C:/src
        std::string decoded; // %20 and friends
        for (size_t i = 0; i < path.size(); ++i) {
            if (path[i] == '%' && i + 2 < path.size() && std::isxdigit(static_cast<unsigned char>(path[i + 1])) &&
                std::isxdigit(static_cast<unsigned char>(path[i + 2]))) {
                decoded += char(std::stoi(path.substr(i + 1, 2), nullptr, 16));
                i += 2;
            } else {
                decoded += path[i];
            }
        }
        path = decoded;
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (isAbsolute(path) || base.empty())
        return normalizePath(path);
    return normalizePath(base + "/" + path);
}

std::optional<WorkspaceImport> parseCodeWorkspace(const std::string& path, std::string_view text, std::string* error)
{
    std::string parseError;
    const std::optional<Json> json = Json::parse(stripJsonComments(text), &parseError);
    if (!json || !json->isObject()) {
        if (error)
            *error = "Not a VS Code workspace: " + (parseError.empty() ? std::string("not a JSON object") : parseError);
        return std::nullopt;
    }
    WorkspaceImport w;
    w.kind = WorkspaceImport::Kind::CodeWorkspace;
    w.name = stem(path);
    const std::string base = parentOf(path);
    if (const Json* folders = json->find("folders"); folders && folders->isArray())
        for (const Json& f : folders->items()) {
            std::string p = f.getString("path");
            if (p.empty())
                p = f.getString("uri");
            const bool remote = p.find("://") != std::string::npos && p.compare(0, 7, "file://") != 0; // vscode-remote:
            if (!p.empty() && !remote)
                w.folders.push_back(resolve(base, p));
        }
    if (w.folders.empty()) {
        if (error)
            *error = "The workspace " + fileName(path) + " lists no local folder";
        return std::nullopt;
    }
    w.root = w.folders.front();
    if (const Json* settings = json->find("settings"); settings && settings->isObject()) {
        if (const Json* tab = settings->find("editor.tabSize"); tab && tab->isNumber())
            w.tabSize = std::clamp(int(std::lround(tab->asNumber())), 1, 16);
        if (const Json* spaces = settings->find("editor.insertSpaces"); spaces && spaces->isBool())
            w.insertSpaces = spaces->asBool();
    }
    return w;
}

// The quoted fields of a .sln "Project(...)" line: type GUID, name, path, project GUID.
std::vector<std::string> quoted(std::string_view line)
{
    std::vector<std::string> out;
    size_t pos = 0;
    while ((pos = line.find('"', pos)) != std::string_view::npos) {
        const size_t end = line.find('"', pos + 1);
        if (end == std::string_view::npos)
            break;
        out.emplace_back(line.substr(pos + 1, end - pos - 1));
        pos = end + 1;
    }
    return out;
}

// A path to a project file (not a solution folder, which only groups projects, nor a web site URL).
bool isProjectPath(const std::string& p)
{
    if (p.empty() || p.find("://") != std::string::npos)
        return false;
    const size_t slash = p.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? p : p.substr(slash + 1);
    return name.find('.') != std::string::npos;
}

std::optional<WorkspaceImport> parseSolution(const std::string& path, std::string_view text, bool xml, std::string* error)
{
    WorkspaceImport w;
    w.kind = WorkspaceImport::Kind::Solution;
    w.name = stem(path);
    const std::string base = parentOf(path);
    bool recognised = false;
    if (xml) { // .slnx: <Project Path="src/App/App.csproj" />
        recognised = text.find("<Solution") != std::string_view::npos;
        size_t pos = 0;
        while ((pos = text.find("<Project", pos)) != std::string_view::npos) {
            const size_t end = text.find('>', pos);
            if (end == std::string_view::npos)
                break;
            const std::string_view element = text.substr(pos, end - pos);
            const size_t attribute = element.find("Path=\"");
            if (attribute != std::string_view::npos) {
                const size_t close = element.find('"', attribute + 6);
                if (close != std::string_view::npos) {
                    const std::string p(element.substr(attribute + 6, close - attribute - 6));
                    if (isProjectPath(p))
                        w.projects.push_back(resolve(base, p));
                }
            }
            pos = end;
        }
    } else { // Project("{type}") = "Name", "path\to\Name.vcxproj", "{guid}"
        recognised = text.find("Microsoft Visual Studio Solution File") != std::string_view::npos;
        size_t begin = 0;
        while (begin < text.size()) {
            size_t end = text.find('\n', begin);
            if (end == std::string_view::npos)
                end = text.size();
            std::string_view line = text.substr(begin, end - begin);
            begin = end + 1;
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '\xEF' ||
                                     line.front() == '\xBB' || line.front() == '\xBF'))
                line.remove_prefix(1);
            if (line.compare(0, 8, "Project(") != 0)
                continue;
            recognised = true;
            const std::vector<std::string> fields = quoted(line);
            if (fields.size() >= 3 && isProjectPath(fields[2]))
                w.projects.push_back(resolve(base, fields[2]));
        }
    }
    if (!recognised) {
        if (error)
            *error = fileName(path) + " is not a Visual Studio solution";
        return std::nullopt;
    }
    // The folder holding the solution and every project, unless the projects are spread so far
    // apart that it would be a drive's root: then the solution's own folder.
    std::vector<std::string> folders{base};
    for (const std::string& p : w.projects)
        folders.push_back(parentOf(p));
    std::string root = commonFolder(folders);
    const bool driveRoot = root.empty() || root == "/" || (root.size() <= 3 && root.size() >= 2 && root[1] == ':');
    w.root = driveRoot ? base : root;
    w.folders.push_back(w.root);
    return w;
}

} // namespace

bool isWorkspaceFile(std::string_view path)
{
    const std::string p = lower(path);
    return endsWith(p, ".code-workspace") || endsWith(p, ".sln") || endsWith(p, ".slnx");
}

std::optional<WorkspaceImport> parseWorkspaceFile(const std::string& path, std::string_view text, std::string* error)
{
    std::string file = path;
    std::replace(file.begin(), file.end(), '\\', '/');
    file = normalizePath(file);
    const std::string p = lower(file);
    if (endsWith(p, ".code-workspace"))
        return parseCodeWorkspace(file, text, error);
    if (endsWith(p, ".sln"))
        return parseSolution(file, text, false, error);
    if (endsWith(p, ".slnx"))
        return parseSolution(file, text, true, error);
    if (error)
        *error = fileName(file) + " is not a workspace file (.code-workspace, .sln, .slnx)";
    return std::nullopt;
}

std::string stripJsonComments(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool inString = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < text.size())
                out += text[++i];
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out += c;
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '/') {
            while (i < text.size() && text[i] != '\n')
                ++i;
            out += '\n';
        } else if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 1;
            out += ' ';
        } else if (c == ']' || c == '}') {
            // A trailing comma before the closer.
            size_t k = out.size();
            while (k > 0 && std::isspace(static_cast<unsigned char>(out[k - 1])))
                --k;
            if (k > 0 && out[k - 1] == ',')
                out.erase(k - 1, 1);
            out += c;
        } else {
            out += c;
        }
    }
    return out;
}

std::string normalizePath(const std::string& input)
{
    std::string path = input;
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string prefix;
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        prefix = path.substr(0, 2);
        path = path.substr(2);
    }
    const bool absolute = !path.empty() && path[0] == '/';
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= path.size()) {
        size_t end = path.find('/', begin);
        if (end == std::string::npos)
            end = path.size();
        const std::string part = path.substr(begin, end - begin);
        begin = end + 1;
        if (part.empty() || part == ".")
            continue;
        if (part == ".." && !parts.empty() && parts.back() != "..")
            parts.pop_back();
        else if (part == ".." && absolute)
            continue; // above the root: stays at the root
        else
            parts.push_back(part);
    }
    std::string out = prefix + (absolute ? "/" : "");
    for (size_t i = 0; i < parts.size(); ++i)
        out += (i ? "/" : "") + parts[i];
    if (out.empty())
        out = ".";
    return out;
}

std::string commonFolder(const std::vector<std::string>& folders)
{
    if (folders.empty())
        return {};
    std::string common = folders.front();
    for (const std::string& f : folders) {
        // Shorten `common` until it is `f` or one of its parents.
        while (!common.empty() &&
               !(f == common || (f.size() > common.size() && f.compare(0, common.size(), common) == 0 &&
                                 (common.back() == '/' || f[common.size()] == '/')))) {
            const std::string parent = parentOf(common);
            if (parent == common)
                break;
            common = parent;
        }
    }
    return common;
}

} // namespace enp::model
