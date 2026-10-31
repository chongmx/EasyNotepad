#include "enp/viewmodel/LayersViewModel.h"

#include "enp/model/Outline.h"
#include "enp/viewmodel/DummyData.h"
#include "enp/viewmodel/NotepadViewModel.h"

#include <algorithm>

namespace enp::viewmodel {

namespace {

LayerKind kindOf(model::OutlineKind kind)
{
    switch (kind) {
    case model::OutlineKind::Namespace: return LayerKind::Namespace;
    case model::OutlineKind::Type: return LayerKind::Type;
    case model::OutlineKind::Function: return LayerKind::Function;
    case model::OutlineKind::Block: return LayerKind::Block;
    case model::OutlineKind::Section: return LayerKind::Section;
    case model::OutlineKind::Step: return LayerKind::Step;
    }
    return LayerKind::Block;
}

LayerKind kindOf(model::LogicalKind kind)
{
    switch (kind) {
    case model::LogicalKind::System: return LayerKind::Project;
    case model::LogicalKind::Module: return LayerKind::Module;
    case model::LogicalKind::Component: return LayerKind::Component;
    case model::LogicalKind::Function: return LayerKind::Function;
    case model::LogicalKind::Step: return LayerKind::Step;
    case model::LogicalKind::Feature: return LayerKind::Feature;
    }
    return LayerKind::Block;
}

} // namespace

LayersViewModel::LayersViewModel(NotepadViewModel& owner) : owner_(owner) {}

const char* LayersViewModel::kindName(LayerKind kind)
{
    switch (kind) {
    case LayerKind::Project: return "Project";
    case LayerKind::Folder: return "Folder";
    case LayerKind::File: return "File";
    case LayerKind::Namespace: return "Namespace";
    case LayerKind::Type: return "Type";
    case LayerKind::Function: return "Function";
    case LayerKind::Block: return "Block";
    case LayerKind::Section: return "Section";
    case LayerKind::Step: return "Step";
    case LayerKind::Module: return "Module";
    case LayerKind::Component: return "Component";
    case LayerKind::Feature: return "Feature";
    }
    return "";
}

const char* LayersViewModel::densityName(LayerDensity density)
{
    switch (density) {
    case LayerDensity::Compact: return "Compact";
    case LayerDensity::Summary: return "Summary";
    case LayerDensity::Detailed: return "Detailed";
    }
    return "";
}

std::string LayersViewModel::cacheKey() const
{
    std::string key = scope.get() == LayerScope::File ? "file:" : scope.get() == LayerScope::Project ? "project:" : "logic:";
    key += std::to_string(int(owner_.foldingMode.get()));
    if (scope.get() == LayerScope::Logic) {
        const model::LogicalMap& map = owner_.logicalMap().map();
        return key + map.source + "/" + std::to_string(map.nodes.size()) + "/" +
               std::to_string(int(owner_.logicalMap().lens.get())) + "/" + owner_.workspaceRoot();
    }
    if (const model::Document* doc = owner_.activeDocument())
        key += std::to_string(doc->id) + "@" + std::to_string(doc->revision) + "/" + std::to_string(int(doc->language));
    if (scope.get() == LayerScope::Project) {
        key += "|" + owner_.workspaceRoot() + "|" + std::to_string(loadedFiles_.size());
        for (const auto& doc : owner_.documents())
            key += "|" + doc->path + "@" + std::to_string(doc->revision);
    }
    return key;
}

int LayersViewModel::addFile(int parent, const std::string& path, const std::string& title, bool withRegions)
{
    model::Language language = model::languageFromPath(path);
    for (const auto& doc : owner_.documents())
        if ((doc->path.empty() ? doc->title : doc->path) == path)
            language = doc->language;
    if (!withRegions) {
        // Project scope keeps big folders cheap: a file's parts load when it is zoomed into.
        const int index = int(nodes_.size());
        LayerNode file;
        file.id = "file:" + path;
        file.kind = LayerKind::File;
        file.title = title;
        file.path = path;
        file.parent = parent;
        file.expandable = model::languageSpec(language).fold != model::FoldStyle::None;
        file.summary = model::languageName(language);
        if (const auto it = facts_.find(path); it != facts_.end()) {
            file.size = it->second.lines;
            file.summary = it->second.summary;
        }
        nodes_.push_back(std::move(file));
        if (parent >= 0)
            nodes_[size_t(parent)].children.push_back(index);
        return index;
    }
    const model::TextBuffer buffer(owner_.readWorkspaceFile(path));
    const std::vector<model::OutlineNode> outline = model::buildOutline(buffer, language, owner_.foldingMode.get());

    const int fileIndex = int(nodes_.size());
    LayerNode file;
    file.id = "file:" + path;
    file.kind = LayerKind::File;
    file.title = title;
    file.path = path;
    file.size = buffer.lineCount();
    file.parent = parent;
    file.summary = dummy::summarizeFile(path, outline);
    nodes_.push_back(std::move(file));
    if (parent >= 0)
        nodes_[size_t(parent)].children.push_back(fileIndex);

    std::vector<std::vector<std::string>> parts(outline.size());
    for (const model::OutlineNode& n : outline)
        if (n.parent >= 0)
            parts[size_t(n.parent)].push_back(n.name);
    std::vector<int> indexOf(outline.size(), -1);
    for (size_t i = 0; i < outline.size(); ++i) {
        const model::OutlineNode& n = outline[i];
        LayerNode region;
        region.id = "region:" + path + ":" + std::to_string(n.startLine + 1);
        region.kind = kindOf(n.kind);
        region.title = n.name;
        region.signature = n.title;
        region.path = path;
        region.startLine = n.startLine;
        region.endLine = n.endLine;
        region.size = n.lineCount();
        region.parent = n.parent >= 0 ? indexOf[size_t(n.parent)] : fileIndex;
        region.summary = dummy::summarize(path, n, buffer, parts[i]);
        indexOf[i] = int(nodes_.size());
        nodes_[size_t(region.parent)].children.push_back(indexOf[i]);
        nodes_.push_back(std::move(region));
    }
    return fileIndex;
}

void LayersViewModel::rebuild()
{
    const std::string key = cacheKey();
    if (key == key_)
        return;
    key_ = key;
    nodes_.clear();
    if (scope.get() == LayerScope::Logic) {
        // The logical map as layers: same ids, so the map view and the layers stay in step.
        const model::LogicalMap& map = owner_.logicalMap().map();
        const std::string rootId = owner_.logicalMap().lens.get() == MapLens::Features ? model::LogicalMap::kFeaturesRoot
                                                                                       : model::LogicalMap::kArchitectureRoot;
        auto add = [&](auto&& self, const std::string& id, int parent) -> void {
            const model::LogicalNode* n = map.find(id);
            if (!n)
                return;
            const int index = int(nodes_.size());
            LayerNode layer;
            layer.id = n->id;
            layer.kind = kindOf(n->kind);
            layer.title = n->name;
            layer.summary = n->summary;
            if (!n->code.empty()) {
                layer.path = n->code.front().path;
                layer.startLine = n->code.front().startLine;
                layer.endLine = n->code.front().endLine;
            }
            layer.size = layer.startLine >= 0 ? layer.endLine - layer.startLine + 1 : int(n->children.size());
            layer.parent = parent;
            nodes_.push_back(std::move(layer));
            if (parent >= 0)
                nodes_[size_t(parent)].children.push_back(index);
            for (const std::string& child : n->children)
                self(self, child, index);
        };
        add(add, rootId, -1);
    } else if (scope.get() == LayerScope::File) {
        if (const model::Document* doc = owner_.activeDocument())
            addFile(-1, doc->path.empty() ? doc->title : doc->path, doc->title, true);
    } else {
        const ExplorerItem& root = owner_.explorer();
        LayerNode project;
        project.id = "project";
        project.kind = LayerKind::Project;
        project.title = root.name;
        nodes_.push_back(project);
        // Depth-first over the workspace tree; folder sizes count their files.
        auto addItem = [&](auto&& self, const ExplorerItem& item, int parent) -> int {
            if (!item.isDirectory) {
                const bool loaded = std::find(loadedFiles_.begin(), loadedFiles_.end(), item.path) != loadedFiles_.end();
                addFile(parent, item.path, item.name, loaded);
                return 1;
            }
            const int index = int(nodes_.size());
            LayerNode folder;
            folder.id = "dir:" + item.path;
            folder.kind = LayerKind::Folder;
            folder.title = item.name;
            folder.parent = parent;
            nodes_.push_back(folder);
            nodes_[size_t(parent)].children.push_back(index);
            int files = 0;
            std::vector<std::string> names;
            for (const ExplorerItem& child : item.children) {
                files += self(self, child, index);
                names.push_back(child.name);
            }
            nodes_[size_t(index)].size = files;
            nodes_[size_t(index)].summary = dummy::summarizeFolder(item.name, names);
            return files;
        };
        int files = 0;
        std::vector<std::string> names;
        for (const ExplorerItem& child : root.children) {
            files += addItem(addItem, child, 0);
            names.push_back(child.name);
        }
        nodes_[0].size = files;
        nodes_[0].summary = dummy::summarizeFolder(root.name, names);
    }
    // Sub-layers in document / tree order are already sorted by construction.
}

const std::vector<LayerNode>& LayersViewModel::nodes()
{
    rebuild();
    describeFocusedFiles();
    return nodes_;
}

void LayersViewModel::describeFocusedFiles()
{
    if (scope.get() != LayerScope::Project || nodes_.empty())
        return;
    int f = 0;
    for (int i = 0; i < int(nodes_.size()); ++i)
        if (nodes_[size_t(i)].id == focusId_[int(LayerScope::Project)])
            f = i;
    for (int child : nodes_[size_t(f)].children) {
        LayerNode& n = nodes_[size_t(child)];
        if (n.kind != LayerKind::File || !n.children.empty() || n.size > 0)
            continue;
        auto it = facts_.find(n.path);
        if (it == facts_.end()) {
            const model::TextBuffer buffer(owner_.readWorkspaceFile(n.path));
            FileFacts facts;
            facts.lines = buffer.lineCount();
            facts.summary = dummy::fileHeaderSummary(n.path, buffer);
            // A .cpp usually documents itself in its header: borrow the header's leading comment.
            if (facts.summary.empty() && n.path.size() > 4 && n.path.compare(n.path.size() - 4, 4, ".cpp") == 0) {
                const std::string header = n.title.substr(0, n.title.size() - 4) + ".h";
                auto find = [&](auto&& self, const ExplorerItem& item) -> std::string {
                    if (!item.isDirectory)
                        return item.name == header ? item.path : std::string();
                    for (const ExplorerItem& child : item.children)
                        if (std::string found = self(self, child); !found.empty())
                            return found;
                    return {};
                };
                if (const std::string headerPath = find(find, owner_.explorer()); !headerPath.empty())
                    facts.summary = dummy::fileHeaderSummary(headerPath, model::TextBuffer(owner_.readWorkspaceFile(headerPath)));
            }
            if (facts.summary.empty())
                facts.summary = n.summary;
            it = facts_.emplace(n.path, std::move(facts)).first;
        }
        n.size = it->second.lines;
        n.summary = it->second.summary;
    }
}

int LayersViewModel::focus()
{
    rebuild();
    if (nodes_.empty())
        return -1;
    const std::string& id = focusId_[int(scope.get())];
    for (int i = 0; i < int(nodes_.size()); ++i)
        if (nodes_[size_t(i)].id == id)
            return i;
    return 0; // the root, when the remembered layer is gone (other file, edited away)
}

std::vector<int> LayersViewModel::breadcrumb()
{
    std::vector<int> path;
    for (int i = focus(); i >= 0; i = nodes_[size_t(i)].parent)
        path.push_back(i);
    std::reverse(path.begin(), path.end());
    return path;
}

int LayersViewModel::current()
{
    const int f = focus();
    if (f < 0)
        return -1;
    if (scope.get() == LayerScope::Logic) {
        // The child of the focus that contains the map's highlighted node (or the caret's).
        const model::Document* d = owner_.activeDocument();
        const model::LogicalMap& map = owner_.logicalMap().map();
        std::string target = d ? map.nodeAt(d->path.empty() ? d->title : d->path, d->caret.line) : std::string();
        for (const std::string& id : map.pathTo(target))
            for (int child : nodes_[size_t(f)].children)
                if (nodes_[size_t(child)].id == id)
                    return child;
        return -1;
    }
    const model::Document* doc = owner_.activeDocument();
    if (!doc)
        return -1;
    const std::string activePath = doc->path.empty() ? doc->title : doc->path;
    for (int child : nodes_[size_t(f)].children) {
        const LayerNode& n = nodes_[size_t(child)];
        if (n.startLine >= 0) {
            if (n.path == activePath && doc->caret.line >= n.startLine && doc->caret.line <= n.endLine)
                return child;
        } else if (n.kind == LayerKind::File ? n.path == activePath
                                             : activePath.rfind(n.id.substr(4) + "/", 0) == 0) { // folder prefix
            return child;
        }
    }
    return -1;
}

void LayersViewModel::setFocus(int index, int direction)
{
    if (index < 0 || index >= int(nodes_.size()))
        return;
    std::string& id = focusId_[int(scope.get())];
    if (id == nodes_[size_t(index)].id)
        return;
    id = nodes_[size_t(index)].id;
    ++zoomSerial_;
    zoomDirection_ = direction;
}

void LayersViewModel::zoomIn(int index)
{
    rebuild();
    if (index < 0 || index >= int(nodes_.size()))
        return;
    if (nodes_[size_t(index)].expandable && nodes_[size_t(index)].children.empty()) {
        // Load the file's parts, then focus it in the rebuilt tree.
        const std::string id = nodes_[size_t(index)].id;
        loadedFiles_.push_back(nodes_[size_t(index)].path);
        rebuild();
        for (int i = 0; i < int(nodes_.size()); ++i)
            if (nodes_[size_t(i)].id == id)
                index = i;
    }
    if (!nodes_[size_t(index)].children.empty())
        setFocus(index, +1);
}

void LayersViewModel::zoomOut()
{
    const int f = focus();
    if (f >= 0 && nodes_[size_t(f)].parent >= 0)
        setFocus(nodes_[size_t(f)].parent, -1);
}

void LayersViewModel::zoomTo(int index)
{
    const int f = focus();
    rebuild();
    // Zooming to an ancestor reads as "out", anything else as "in".
    int direction = +1;
    for (int i = f; i >= 0; i = nodes_[size_t(i)].parent)
        if (i == index)
            direction = -1;
    setFocus(index, direction);
}

void LayersViewModel::open(int index)
{
    rebuild();
    if (index < 0 || index >= int(nodes_.size()))
        return;
    const LayerNode n = nodes_[size_t(index)]; // copy: opening a file can rebuild the tree
    if (scope.get() == LayerScope::Logic) {
        if (n.path.empty() || n.kind == LayerKind::Feature)
            zoomIn(index);
        else
            owner_.logicalMap().open(n.id);
        return;
    }
    if (n.kind == LayerKind::Project || n.kind == LayerKind::Folder) {
        zoomIn(index);
        return;
    }
    if (n.kind == LayerKind::File || scope.get() == LayerScope::Project)
        owner_.openFile(n.path);
    if (n.startLine >= 0) {
        owner_.setFolded(n.startLine, false); // the user wants the details
        owner_.goToLine(n.startLine);
    }
}

} // namespace enp::viewmodel
