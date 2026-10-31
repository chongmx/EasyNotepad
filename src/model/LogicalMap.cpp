#include "enp/model/LogicalMap.h"

#include "enp/model/Outline.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>

namespace enp::model {

namespace {

std::string trim(const std::string& s)
{
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b])))
        ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1])))
        --e;
    return s.substr(b, e - b);
}

bool startsWith(const std::string& s, const char* prefix)
{
    return s.rfind(prefix, 0) == 0;
}

std::string clip(std::string s, size_t max = 220)
{
    if (s.size() > max)
        s = s.substr(0, max - 3) + "...";
    return s;
}

bool isComment(const std::string& t, Language language)
{
    const LanguageSpec& spec = languageSpec(language);
    if (spec.lineComment == "#")
        return startsWith(t, "#") && !startsWith(t, "#!");
    if (!spec.lineComment.empty() && spec.lineComment != "//")
        return startsWith(t, spec.lineComment.c_str());
    return startsWith(t, "//") || startsWith(t, "/*") || startsWith(t, "* ") || t == "*";
}

std::string stripComment(std::string t)
{
    for (const char* prefix : {"///", "//", "/*", "#", "*"})
        if (startsWith(t, prefix)) {
            t = t.substr(std::string(prefix).size());
            break;
        }
    if (t.size() >= 2 && t.compare(t.size() - 2, 2, "*/") == 0)
        t.resize(t.size() - 2);
    return trim(t);
}

// The comment that opens a file (after includes / pragmas), or a Python docstring.
std::string leadingComment(const TextBuffer& buffer, Language language)
{
    std::string text;
    for (int l = 0; l < buffer.lineCount() && l < 60; ++l) {
        const std::string t = trim(buffer.line(l));
        if (language == Language::Python && (startsWith(t, "\"\"\"") || startsWith(t, "'''"))) {
            std::string doc = t.substr(3);
            const size_t end = doc.find(t.substr(0, 3));
            return clip(trim(end == std::string::npos ? doc : doc.substr(0, end)));
        }
        if (isComment(t, language)) {
            const std::string c = stripComment(t);
            if (c.find("----") == std::string::npos && !c.empty())
                text += (text.empty() ? "" : " ") + c;
            continue;
        }
        if (!text.empty())
            break;
        if (t.empty() || startsWith(t, "#include") || startsWith(t, "#pragma") || startsWith(t, "#!") ||
            startsWith(t, "import ") || startsWith(t, "from "))
            continue;
        break; // code before any comment
    }
    return clip(text);
}

// The comment directly above `line`.
std::string commentAbove(const TextBuffer& buffer, int line, Language language)
{
    std::vector<std::string> parts;
    for (int l = line - 1; l >= 0 && l >= line - 10; --l) {
        const std::string t = trim(buffer.line(l));
        if (!isComment(t, language))
            break;
        const std::string c = stripComment(t);
        if (c.find("----") == std::string::npos)
            parts.insert(parts.begin(), c);
    }
    std::string text;
    for (const std::string& p : parts)
        if (!p.empty())
            text += (text.empty() ? "" : " ") + p;
    return clip(text);
}

std::string stemOf(const std::string& path)
{
    std::string name = path.substr(path.find_last_of('/') + 1);
    const size_t dot = name.find('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

// The module is the first folder below the source roots: src/os/win32/OsWin32.cpp and
// include/enp/os/Pty.h both belong to "os"; tests/TestMain.cpp to "tests".
std::string moduleOf(const std::string& path)
{
    std::vector<std::string> parts;
    for (size_t start = 0, slash; (slash = path.find('/', start)) != std::string::npos; start = slash + 1)
        parts.push_back(path.substr(start, slash - start));
    size_t i = 0;
    if (i < parts.size() && (parts[i] == "src" || parts[i] == "source" || parts[i] == "lib"))
        ++i;
    else if (i < parts.size() && parts[i] == "include")
        i += parts.size() - i > 2 ? 2 : 1; // include/<project>/<module>/...
    return i < parts.size() ? parts[i] : "root";
}

// "| **view** | viewmodel, ImGui | immediate-mode rendering and input |" -> last cell.
std::string readmeRow(const std::string& readme, const std::string& module)
{
    const TextBuffer buffer(readme);
    const std::string key = "**" + module + "**";
    for (int l = 0; l < buffer.lineCount(); ++l) {
        const std::string t = trim(buffer.line(l));
        if (!startsWith(t, "|") || t.find(key) == std::string::npos)
            continue;
        std::vector<std::string> cells;
        size_t start = 1;
        for (size_t p = t.find('|', 1); p != std::string::npos; start = p + 1, p = t.find('|', start))
            cells.push_back(trim(t.substr(start, p - start)));
        if (cells.size() >= 2) {
            std::string cell = cells.back();
            cell.erase(std::remove(cell.begin(), cell.end(), '`'), cell.end());
            if (!cell.empty())
                cell[0] = char(std::toupper(static_cast<unsigned char>(cell[0])));
            return cell + ".";
        }
    }
    return {};
}

std::string readmeIntro(const std::string& readme)
{
    const TextBuffer buffer(readme);
    std::string text;
    for (int l = 0; l < buffer.lineCount(); ++l) {
        const std::string t = trim(buffer.line(l));
        if (t.empty()) {
            if (!text.empty())
                break;
            continue;
        }
        if (startsWith(t, "#") || startsWith(t, ">") || startsWith(t, "|") || startsWith(t, "```") || startsWith(t, "!["))
            continue;
        text += (text.empty() ? "" : " ") + t;
    }
    text.erase(std::remove(text.begin(), text.end(), '*'), text.end());
    return clip(text, 300);
}

Json stringArray(const std::vector<std::string>& v)
{
    Json a = Json::array();
    for (const std::string& s : v)
        a.push(s);
    return a;
}

} // namespace

const char* logicalKindName(LogicalKind kind)
{
    switch (kind) {
    case LogicalKind::System: return "System";
    case LogicalKind::Module: return "Module";
    case LogicalKind::Component: return "Component";
    case LogicalKind::Function: return "Function";
    case LogicalKind::Step: return "Step";
    case LogicalKind::Feature: return "Feature";
    }
    return "";
}

// ---- Queries --------------------------------------------------------------------------------------

const LogicalNode* LogicalMap::find(const std::string& id) const
{
    for (const LogicalNode& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

LogicalNode* LogicalMap::find(const std::string& id)
{
    for (LogicalNode& n : nodes)
        if (n.id == id)
            return &n;
    return nullptr;
}

std::vector<LogicalEdge> LogicalMap::edgesAmongChildren(const std::string& parent) const
{
    std::map<std::string, const LogicalNode*> byId;
    for (const LogicalNode& n : nodes)
        byId[n.id] = &n;
    auto lift = [&](std::string id) -> std::string {
        for (int guard = 0; guard < 64; ++guard) {
            auto it = byId.find(id);
            if (it == byId.end())
                return {};
            if (it->second->parent == parent)
                return id;
            id = it->second->parent;
        }
        return {};
    };
    std::map<std::pair<std::string, std::string>, LogicalEdge> lifted;
    for (const LogicalEdge& e : edges) {
        const std::string a = lift(e.from), b = lift(e.to);
        if (a.empty() || b.empty() || a == b)
            continue;
        auto [it, inserted] = lifted.try_emplace({a, b}, LogicalEdge{a, b, e.label, e.weight});
        if (!inserted)
            it->second.weight += e.weight;
    }
    std::vector<LogicalEdge> result;
    for (auto& [key, e] : lifted)
        result.push_back(e);
    return result;
}

std::string LogicalMap::nodeAt(const std::string& path, int line) const
{
    const LogicalNode* best = nullptr;
    int bestSpan = 1 << 30;
    for (const LogicalNode& n : nodes) {
        for (const CodeRef& ref : n.code) {
            if (ref.path != path)
                continue;
            const bool whole = ref.startLine < 0;
            if (!whole && (line < ref.startLine || line > ref.endLine))
                continue;
            const int span = whole ? (1 << 29) : ref.endLine - ref.startLine;
            if (span < bestSpan) {
                bestSpan = span;
                best = &n;
            }
        }
    }
    return best ? best->id : std::string();
}

std::vector<std::string> LogicalMap::pathTo(const std::string& id) const
{
    std::vector<std::string> path;
    for (std::string cur = id; !cur.empty() && path.size() < 64;) {
        const LogicalNode* n = find(cur);
        if (!n)
            break;
        path.insert(path.begin(), cur);
        cur = n->parent;
    }
    return path;
}

// ---- JSON -----------------------------------------------------------------------------------------

Json LogicalMap::toJson() const
{
    Json jn = Json::array();
    for (const LogicalNode& n : nodes) {
        Json code = Json::array();
        for (const CodeRef& r : n.code)
            code.push(Json::object().set("path", r.path).set("start", r.startLine).set("end", r.endLine));
        jn.push(Json::object()
                    .set("id", n.id)
                    .set("kind", logicalKindName(n.kind))
                    .set("name", n.name)
                    .set("summary", n.summary)
                    .set("responsibilities", stringArray(n.responsibilities))
                    .set("code", std::move(code))
                    .set("target", n.target)
                    .set("parent", n.parent)
                    .set("children", stringArray(n.children)));
    }
    Json je = Json::array();
    for (const LogicalEdge& e : edges)
        je.push(Json::object().set("from", e.from).set("to", e.to).set("label", e.label).set("weight", e.weight));
    return Json::object().set("version", 1).set("source", source).set("nodes", std::move(jn)).set("edges", std::move(je));
}

std::optional<LogicalMap> LogicalMap::fromJson(const Json& json)
{
    if (!json.isObject() || json.getNumber("version") != 1 || !json.find("nodes") || !json.find("nodes")->isArray())
        return std::nullopt;
    LogicalMap map;
    map.source = json.getString("source", "local");
    for (const Json& jn : json.find("nodes")->items()) {
        LogicalNode n;
        n.id = jn.getString("id");
        const std::string kind = jn.getString("kind");
        for (LogicalKind k : {LogicalKind::System, LogicalKind::Module, LogicalKind::Component, LogicalKind::Function,
                              LogicalKind::Step, LogicalKind::Feature})
            if (kind == logicalKindName(k))
                n.kind = k;
        n.name = jn.getString("name");
        n.summary = jn.getString("summary");
        if (const Json* r = jn.find("responsibilities"))
            for (const Json& s : r->items())
                n.responsibilities.push_back(s.asString());
        if (const Json* c = jn.find("code"))
            for (const Json& r : c->items())
                n.code.push_back({r.getString("path"), int(r.getNumber("start", -1)), int(r.getNumber("end", -1))});
        n.target = jn.getString("target");
        n.parent = jn.getString("parent");
        if (const Json* c = jn.find("children"))
            for (const Json& s : c->items())
                n.children.push_back(s.asString());
        if (n.id.empty())
            return std::nullopt;
        map.nodes.push_back(std::move(n));
    }
    if (const Json* je = json.find("edges"))
        for (const Json& e : je->items())
            map.edges.push_back({e.getString("from"), e.getString("to"), e.getString("label"), int(e.getNumber("weight", 1))});
    if (!map.find(kArchitectureRoot))
        return std::nullopt;
    return map;
}

// ---- Local dissection ---------------------------------------------------------------------------

namespace {

// "zoomIn(…)" / "Engine::step()" -> "zoomIn" / "step".
std::string callName(const std::string& name)
{
    std::string base = name.substr(0, name.find('('));
    const size_t scope = base.rfind("::");
    if (scope != std::string::npos)
        base = base.substr(scope + 2);
    while (!base.empty() && base.back() == ' ')
        base.pop_back();
    return base;
}

} // namespace

LogicalMap dissect(const std::string& projectName, const std::vector<SourceFile>& files, const std::string& readme)
{
    LogicalMap map;
    map.source = "local";
    LogicalNode system;
    system.id = LogicalMap::kArchitectureRoot;
    system.kind = LogicalKind::System;
    system.name = projectName;
    system.summary = readmeIntro(readme);
    map.nodes.push_back(system);

    // Group files: module (directory) -> component (file stem, header and implementation together).
    std::map<std::string, std::map<std::string, std::vector<const SourceFile*>>> modules;
    for (const SourceFile& f : files) {
        const Language language = languageFromPath(f.path);
        if (!languageSpec(language).program)
            continue;
        modules[moduleOf(f.path)][stemOf(f.path)].push_back(&f);
    }

    std::map<std::string, std::vector<std::string>> componentsByStem; // for resolving includes
    std::vector<std::pair<std::string, const SourceFile*>> fileOwners;
    for (auto& [moduleName, components] : modules) {
        LogicalNode module;
        module.id = "module:" + moduleName;
        module.kind = LogicalKind::Module;
        module.name = moduleName;
        module.parent = system.id;
        std::vector<std::string> componentNames;
        for (auto& [stem, sources] : components) {
            LogicalNode component;
            component.id = "component:" + moduleName + "/" + stem;
            component.kind = LogicalKind::Component;
            component.name = stem;
            component.parent = module.id;
            componentNames.push_back(stem);
            componentsByStem[stem].push_back(component.id);
            // Headers first: they usually carry the description.
            std::stable_sort(sources.begin(), sources.end(), [](const SourceFile* a, const SourceFile* b) {
                auto rank = [](const std::string& p) { return p.size() > 2 && p.compare(p.size() - 2, 2, ".h") == 0 ? 0 : 1; };
                return rank(a->path) < rank(b->path);
            });
            std::vector<LogicalNode> functions;
            std::vector<std::string> bodies; // per function, for the calls between them
            for (const SourceFile* f : sources) {
                component.code.push_back({f->path, -1, -1});
                fileOwners.emplace_back(component.id, f);
                const Language language = languageFromPath(f->path);
                const TextBuffer buffer(f->text);
                if (component.summary.empty())
                    component.summary = leadingComment(buffer, language);
                const std::vector<OutlineNode> outline = buildOutline(buffer, language, OutlineMode::Logical);
                for (size_t i = 0; i < outline.size(); ++i) {
                    const OutlineNode& n = outline[i];
                    if (n.kind != OutlineKind::Function)
                        continue;
                    bool nested = false;
                    for (int p = n.parent; p >= 0; p = outline[size_t(p)].parent)
                        nested = nested || outline[size_t(p)].kind == OutlineKind::Function;
                    if (nested)
                        continue;
                    LogicalNode fn;
                    fn.id = "function:" + f->path + ":" + std::to_string(n.startLine + 1);
                    fn.kind = LogicalKind::Function;
                    fn.name = n.name;
                    fn.summary = commentAbove(buffer, n.startLine, language);
                    fn.code.push_back({f->path, n.startLine, n.endLine});
                    fn.parent = component.id;
                    std::vector<LogicalNode> steps;
                    for (size_t k = i + 1; k < outline.size(); ++k) {
                        const OutlineNode& s = outline[k];
                        if (s.startLine > n.endLine)
                            break;
                        if (s.kind != OutlineKind::Step)
                            continue;
                        LogicalNode step;
                        step.id = "step:" + f->path + ":" + std::to_string(s.startLine + 1);
                        step.kind = LogicalKind::Step;
                        step.name = s.hint.empty() ? s.title : s.hint;
                        // The title shows a short comment whole; a long one continues in the summary.
                        const std::string size = std::to_string(s.endLine - s.startLine + 1) + " lines.";
                        step.summary = s.hint.size() > 48 ? s.hint : size;
                        step.code.push_back({f->path, s.startLine, s.endLine});
                        step.parent = fn.id;
                        steps.push_back(std::move(step));
                    }
                    if (steps.size() >= 2) { // a single step says nothing the function doesn't
                        for (LogicalNode& step : steps) {
                            fn.children.push_back(step.id);
                            map.nodes.push_back(std::move(step));
                        }
                    }
                    if (fn.summary.empty())
                        fn.summary = std::to_string(n.endLine - n.startLine + 1) + " lines" +
                                     (fn.children.empty() ? "." : ", " + std::to_string(fn.children.size()) + " steps.");
                    std::string body;
                    for (int line = n.startLine + 1; line <= n.endLine && line < buffer.lineCount(); ++line)
                        body += buffer.line(line) + "\n";
                    bodies.push_back(std::move(body));
                    component.children.push_back(fn.id);
                    functions.push_back(std::move(fn));
                }
            }
            if (component.summary.empty() && !functions.empty()) {
                component.summary = "Functions: ";
                for (size_t k = 0; k < functions.size() && k < 4; ++k)
                    component.summary += (k ? ", " : "") + functions[k].name;
                component.summary += functions.size() > 4 ? ", ..." : ".";
            }
            // Calls between the component's functions: "name(" in a body, as a whole word.
            for (size_t a = 0; a < functions.size(); ++a)
                for (size_t b = 0; b < functions.size(); ++b) {
                    const std::string callee = callName(functions[b].name);
                    if (a == b || callee.size() < 3 || callName(functions[a].name) == callee)
                        continue;
                    int calls = 0;
                    for (size_t at = bodies[a].find(callee); at != std::string::npos; at = bodies[a].find(callee, at + 1)) {
                        const size_t end = at + callee.size();
                        const bool wordStart = at == 0 || !(std::isalnum(static_cast<unsigned char>(bodies[a][at - 1])) || bodies[a][at - 1] == '_');
                        size_t paren = end;
                        while (paren < bodies[a].size() && bodies[a][paren] == ' ')
                            ++paren;
                        if (wordStart && paren < bodies[a].size() && bodies[a][paren] == '(')
                            ++calls;
                    }
                    if (calls > 0)
                        map.edges.push_back({functions[a].id, functions[b].id, "calls", calls});
                }
            for (LogicalNode& fn : functions)
                map.nodes.push_back(std::move(fn));
            module.children.push_back(component.id);
            map.nodes.push_back(std::move(component));
        }
        module.summary = readmeRow(readme, moduleName);
        if (module.summary.empty()) {
            module.summary = std::to_string(componentNames.size()) + (componentNames.size() == 1 ? " component: " : " components: ");
            for (size_t k = 0; k < componentNames.size() && k < 4; ++k)
                module.summary += (k ? ", " : "") + componentNames[k];
            module.summary += componentNames.size() > 4 ? ", ..." : ".";
        }
        map.find(system.id)->children.push_back(module.id);
        map.nodes.push_back(std::move(module));
    }

    // Relations from #include "..." and Python imports.
    std::map<std::pair<std::string, std::string>, int> uses;
    for (const auto& [owner, f] : fileOwners) {
        const TextBuffer buffer(f->text);
        for (int l = 0; l < buffer.lineCount(); ++l) {
            const std::string t = trim(buffer.line(l));
            std::string target;
            if (startsWith(t, "#include \"")) {
                const size_t end = t.find('"', 10);
                if (end != std::string::npos)
                    target = t.substr(10, end - 10);
            } else if (startsWith(t, "from ") || startsWith(t, "import ")) {
                std::string what = trim(t.substr(t[0] == 'f' ? 5 : 7));
                what = what.substr(0, what.find(' '));
                std::replace(what.begin(), what.end(), '.', '/');
                target = what;
            }
            if (target.empty())
                continue;
            const auto it = componentsByStem.find(stemOf(target));
            if (it == componentsByStem.end())
                continue;
            // Prefer the component whose module appears in the include path.
            std::string to = it->second.front();
            for (const std::string& candidate : it->second) {
                const std::string module = candidate.substr(10, candidate.find('/') - 10);
                if (target.find(module + "/") != std::string::npos)
                    to = candidate;
            }
            if (to != owner)
                ++uses[{owner, to}];
        }
    }
    for (const auto& [key, count] : uses)
        map.edges.push_back({key.first, key.second, "uses", count});
    return map;
}

// ---- Agent refinement -------------------------------------------------------------------------------

std::string agentPrompt(const LogicalMap& map)
{
    Json modules = Json::array();
    const LogicalNode* root = map.find(LogicalMap::kArchitectureRoot);
    if (root)
        for (const std::string& mid : root->children) {
            const LogicalNode* m = map.find(mid);
            if (!m)
                continue;
            Json components = Json::array();
            for (const std::string& cid : m->children) {
                const LogicalNode* c = map.find(cid);
                if (!c)
                    continue;
                Json files = Json::array(), functions = Json::array();
                for (const CodeRef& r : c->code)
                    files.push(r.path);
                for (size_t k = 0; k < c->children.size() && k < 15; ++k)
                    if (const LogicalNode* fn = map.find(c->children[k]))
                        functions.push(fn->name);
                components.push(Json::object()
                                    .set("id", c->id)
                                    .set("files", std::move(files))
                                    .set("summary", c->summary)
                                    .set("functions", std::move(functions)));
            }
            modules.push(Json::object().set("id", m->id).set("summary", m->summary).set("components", std::move(components)));
        }
    Json relations = Json::array();
    for (const LogicalEdge& e : map.edgesAmongChildren(LogicalMap::kArchitectureRoot))
        relations.push(e.from + " uses " + e.to);
    const Json skeleton = Json::object()
                              .set("project", root ? root->name : std::string())
                              .set("summary", root ? root->summary : std::string())
                              .set("modules", std::move(modules))
                              .set("moduleRelations", std::move(relations));
    return "You are dissecting a software project to build a functional map of it for a code reader. "
           "Read the project in the current directory as needed. The structural skeleton found so far is:\n\n" +
           skeleton.dump() +
           "\n\nAnswer with ONLY a JSON object (no prose, no code fences) of this shape:\n"
           "{\"system\":{\"summary\":\"what the program does, 1-2 sentences\"},"
           "\"modules\":[{\"id\":\"module:...\",\"summary\":\"its role\",\"responsibilities\":[\"short bullet\"]}],"
           "\"components\":[{\"id\":\"component:...\",\"summary\":\"what it does, one sentence\"}],"
           "\"features\":[{\"name\":\"user-facing feature\",\"summary\":\"what the user gets\","
           "\"steps\":[{\"component\":\"component:...\",\"action\":\"what happens in that component\"}]}]}\n"
           "Use only ids from the skeleton. List the program's main user-facing features (4-10), each as the "
           "flow of steps through components that implements it.";
}

bool mergeAgentAnalysis(LogicalMap& map, const std::string& answer, std::string* error)
{
    const size_t open = answer.find('{');
    const size_t close = answer.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        if (error)
            *error = "the agent did not answer with JSON";
        return false;
    }
    std::string parseError;
    const std::optional<Json> json = Json::parse(std::string_view(answer).substr(open, close - open + 1), &parseError);
    if (!json || !json->isObject()) {
        if (error)
            *error = "the agent's JSON is invalid: " + parseError;
        return false;
    }
    if (const Json* system = json->find("system"))
        if (LogicalNode* root = map.find(LogicalMap::kArchitectureRoot); root && !system->getString("summary").empty())
            root->summary = clip(system->getString("summary"), 400);
    for (const char* group : {"modules", "components"})
        if (const Json* list = json->find(group))
            for (const Json& item : list->items())
                if (LogicalNode* n = map.find(item.getString("id"))) {
                    if (!item.getString("summary").empty())
                        n->summary = clip(item.getString("summary"), 300);
                    if (const Json* r = item.find("responsibilities")) {
                        n->responsibilities.clear();
                        for (const Json& s : r->items())
                            if (s.isString())
                                n->responsibilities.push_back(clip(s.asString(), 120));
                    }
                }

    // Features: a second root; each feature is a flow of steps that point into components.
    map.nodes.erase(std::remove_if(map.nodes.begin(), map.nodes.end(),
                                   [](const LogicalNode& n) { return n.id.rfind("feature", 0) == 0; }),
                    map.nodes.end());
    map.edges.erase(std::remove_if(map.edges.begin(), map.edges.end(),
                                   [](const LogicalEdge& e) { return e.from.rfind("feature", 0) == 0; }),
                    map.edges.end());
    if (const Json* features = json->find("features"); features && features->isArray() && !features->items().empty()) {
        LogicalNode root;
        root.id = LogicalMap::kFeaturesRoot;
        root.kind = LogicalKind::System;
        root.name = "Features";
        root.summary = "What the program does for its user, each as a flow through the components.";
        int index = 0;
        std::vector<LogicalNode> added;
        for (const Json& f : features->items()) {
            LogicalNode feature;
            feature.id = "feature:" + std::to_string(++index);
            feature.kind = LogicalKind::Feature;
            feature.name = clip(f.getString("name"), 60);
            feature.summary = clip(f.getString("summary"), 300);
            feature.parent = root.id;
            std::string previous;
            int k = 0;
            if (const Json* steps = f.find("steps"))
                for (const Json& s : steps->items()) {
                    LogicalNode step;
                    step.id = feature.id + ":" + std::to_string(++k);
                    step.kind = LogicalKind::Step;
                    step.target = s.getString("component");
                    const LogicalNode* component = map.find(step.target);
                    step.name = component ? component->name : step.target;
                    step.summary = clip(s.getString("action"), 200);
                    if (component)
                        step.code = component->code;
                    step.parent = feature.id;
                    feature.children.push_back(step.id);
                    if (!previous.empty())
                        map.edges.push_back({previous, step.id, "then", 1});
                    previous = step.id;
                    added.push_back(std::move(step));
                }
            root.children.push_back(feature.id);
            added.push_back(std::move(feature));
        }
        map.nodes.push_back(std::move(root));
        for (LogicalNode& n : added)
            map.nodes.push_back(std::move(n));
    }
    map.source = "agent";
    return true;
}

} // namespace enp::model
