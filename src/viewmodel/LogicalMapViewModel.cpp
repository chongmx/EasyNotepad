#include "enp/viewmodel/LogicalMapViewModel.h"

#include "enp/viewmodel/NotepadViewModel.h"

#include <algorithm>

namespace enp::viewmodel {

namespace {

constexpr size_t kMaxSourceFiles = 1500;

bool isVendorFolder(const std::string& name)
{
    static const char* const vendor[] = {"3rdparty", "third_party", "thirdparty", "vendor", "external", "extern", "deps"};
    for (const char* v : vendor)
        if (name == v)
            return true;
    return false;
}

bool isSource(const std::string& name)
{
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    const std::string ext = name.substr(dot);
    for (const char* e : {".cpp", ".cc", ".cxx", ".c", ".h", ".hpp", ".hh", ".py", ".js", ".mjs", ".ts"})
        if (ext == e)
            return true;
    return false;
}

} // namespace

LogicalMapViewModel::LogicalMapViewModel(NotepadViewModel& owner, interfaces::IAgentCliService* agent)
    : owner_(owner), agent_(agent)
{
}

std::string LogicalMapViewModel::root() const
{
    return lens.get() == MapLens::Features ? model::LogicalMap::kFeaturesRoot : model::LogicalMap::kArchitectureRoot;
}

std::vector<model::SourceFile> LogicalMapViewModel::collectSources()
{
    std::vector<model::SourceFile> files;
    auto walk = [&](auto&& self, const ExplorerItem& item) -> void {
        for (const ExplorerItem& child : item.children) {
            if (files.size() >= kMaxSourceFiles)
                return;
            if (child.isDirectory) {
                if (!isVendorFolder(child.name))
                    self(self, child);
            } else if (isSource(child.name)) {
                files.push_back({child.path, owner_.readWorkspaceFile(child.path)});
            }
        }
    };
    walk(walk, owner_.explorer());
    return files;
}

void LogicalMapViewModel::ensure()
{
    const std::string key = owner_.workspaceRoot() + "|" + owner_.explorer().name;
    if (key == key_)
        return;
    key_ = key;
    focus_[0].clear();
    focus_[1].clear();
    highlight_.clear();
    // A previous agent dissection of this folder is reused.
    if (!owner_.workspaceRoot().empty()) {
        const std::string cached = owner_.readWorkspaceFile(kCachePath);
        if (const std::optional<model::Json> json = model::Json::parse(cached))
            if (std::optional<model::LogicalMap> map = model::LogicalMap::fromJson(*json)) {
                map_ = std::move(*map);
                return;
            }
    }
    std::string readme;
    for (const ExplorerItem& item : owner_.explorer().children)
        if (!item.isDirectory && (item.name == "README.md" || item.name == "readme.md"))
            readme = owner_.readWorkspaceFile(item.path);
    map_ = model::dissect(owner_.explorer().name, collectSources(), readme);
}

const model::LogicalMap& LogicalMapViewModel::map()
{
    ensure();
    return map_;
}

bool LogicalMapViewModel::hasFeatures()
{
    return map().find(model::LogicalMap::kFeaturesRoot) != nullptr;
}

const char* LogicalMapViewModel::sourceName()
{
    return map().source == "agent" ? "agent dissection" : "local dissection";
}

std::string LogicalMapViewModel::focus()
{
    ensure();
    std::string& f = focus_[int(lens.get())];
    if (f.empty() || !map_.find(f))
        f = root();
    return map_.find(f) ? f : std::string();
}

std::vector<std::string> LogicalMapViewModel::breadcrumb()
{
    return map_.pathTo(focus());
}

void LogicalMapViewModel::setFocus(const std::string& id, int direction)
{
    std::string& f = focus_[int(lens.get())];
    if (f == id || !map_.find(id))
        return;
    f = id;
    ++zoomSerial_;
    zoomDirection_ = direction;
}

void LogicalMapViewModel::zoomTo(const std::string& id)
{
    const std::vector<std::string> path = breadcrumb();
    const bool ancestor = std::find(path.begin(), path.end(), id) != path.end();
    setFocus(id, ancestor ? -1 : +1);
}

void LogicalMapViewModel::zoomIn(const std::string& id)
{
    ensure();
    const model::LogicalNode* n = map_.find(id);
    if (!n)
        return;
    if (n->children.empty()) {
        // Feature steps lead into the component they run in.
        if (!n->target.empty() && map_.find(n->target)) {
            lens.set(MapLens::Architecture);
            const model::LogicalNode* target = map_.find(n->target);
            setFocus(target->children.empty() ? target->parent : target->id, +1);
            highlight_ = target->id;
        }
        return;
    }
    setFocus(id, +1);
}

void LogicalMapViewModel::zoomOut()
{
    const model::LogicalNode* n = map_.find(focus());
    if (n && !n->parent.empty())
        setFocus(n->parent, -1);
}

void LogicalMapViewModel::open(const std::string& id)
{
    ensure();
    const model::LogicalNode* n = map_.find(id);
    if (!n || n->code.empty())
        return;
    const model::CodeRef ref = n->code.front(); // copy: opening a file can touch the map
    owner_.openFile(ref.path);
    if (ref.startLine >= 0) {
        owner_.setFolded(ref.startLine, false);
        owner_.goToLine(ref.startLine);
    }
    owner_.editorMode.set(EditorMode::Code);
}

void LogicalMapViewModel::revealCaret()
{
    ensure();
    const model::Document* doc = owner_.activeDocument();
    if (!doc)
        return;
    const std::string id = map_.nodeAt(doc->path.empty() ? doc->title : doc->path, doc->caret.line);
    const model::LogicalNode* n = map_.find(id);
    if (!n)
        return;
    lens.set(MapLens::Architecture);
    highlight_ = id;
    // Show the node among its siblings.
    setFocus(n->parent.empty() ? id : n->parent, +1);
}

void LogicalMapViewModel::refresh()
{
    key_.clear();
    if (!owner_.workspaceRoot().empty())
        owner_.writeWorkspaceFile(kCachePath, ""); // forget the agent's version
    ensure();
    statusMessage.emit("Map rebuilt by local dissection: " + std::to_string(map_.nodes.size()) + " parts");
}

bool LogicalMapViewModel::agentAvailable() const
{
    return agent_ && agent_->available();
}

std::string LogicalMapViewModel::agentCommand() const
{
    return agent_ ? agent_->command() : std::string();
}

void LogicalMapViewModel::dissectWithAgent()
{
    if (analyzing.get())
        return;
    if (!agent_) {
        statusMessage.emit("No agent service on this platform");
        return;
    }
    if (!agent_->available()) {
        statusMessage.emit("Agent CLI '" + agent_->command() +
                           "' not found: install Claude Code, or set EASYNOTEPAD_AGENT_CLI to your agent's command");
        return;
    }
    ensure();
    analyzing.set(true);
    statusMessage.emit("The agent is dissecting the project (" + agent_->command() + ")...");
    std::weak_ptr<bool> alive = alive_;
    agent_->run(model::agentPrompt(map_), owner_.workspaceRoot(), [this, alive](bool ok, const std::string& output) {
        if (alive.expired())
            return;
        analyzing.set(false);
        if (!ok) {
            statusMessage.emit(output == "stopped" ? std::string("Agent dissection stopped")
                                                   : "Agent dissection failed: " + output.substr(0, 300));
            return;
        }
        std::string error;
        if (!model::mergeAgentAnalysis(map_, output, &error)) {
            statusMessage.emit("Agent dissection unusable: " + error);
            return;
        }
        if (!owner_.workspaceRoot().empty())
            owner_.writeWorkspaceFile(kCachePath, map_.toJson().dump());
        const model::LogicalNode* features = map_.find(model::LogicalMap::kFeaturesRoot);
        statusMessage.emit("Agent dissection merged: " + std::to_string(features ? features->children.size() : 0) +
                           " features, refined summaries");
        if (features)
            lens.set(MapLens::Features);
        ++zoomSerial_;
    });
}

} // namespace enp::viewmodel
