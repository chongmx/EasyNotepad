// Layers: the code as a tree of nested parts you zoom into, each with an AI summary.
// File scope: the active document's regions. Project scope: folders, then files, then regions.
// Logic scope: the functional hierarchy (system, modules, components, functions, steps) of the
// logical map, or its features.
// The view shows the children of the focused layer; zooming in/out moves the focus.
#pragma once

#include "enp/viewmodel/Observable.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace enp::viewmodel {

class NotepadViewModel;

enum class LayerKind { Project, Folder, File, Namespace, Type, Function, Block, Section, Step, Module, Component, Feature };
enum class LayerScope { File, Project, Logic };
enum class LayerDensity { Compact, Summary, Detailed };

struct LayerNode {
    std::string id;      // stable: "project", "dir:src", "file:src/main.cpp", "region:src/main.cpp:20"
    LayerKind kind = LayerKind::Block;
    std::string title;     // short name
    std::string signature; // full header line (regions), shown in Detailed density
    std::string summary; // AI summary (sample data until the summarizer CLI is connected)
    std::string path;    // file the layer belongs to (empty for the project and folders)
    int startLine = -1;  // regions only, 0-based
    int endLine = -1;
    int size = 0;        // lines for files and regions, files for folders and the project
    int parent = -1;
    std::vector<int> children;
    bool expandable = false; // project scope: a file whose parts load when zoomed into
};

class LayersViewModel {
public:
    explicit LayersViewModel(NotepadViewModel& owner);

    Property<LayerScope> scope{LayerScope::File};
    Property<LayerDensity> density{LayerDensity::Summary};

    static const char* kindName(LayerKind kind);
    static const char* densityName(LayerDensity density);

    // The tree for the current scope, rebuilt when documents change. Empty when there is nothing
    // to show (no document in file scope).
    const std::vector<LayerNode>& nodes();
    int focus();                   // index of the zoomed-in layer
    std::vector<int> breadcrumb(); // root .. focus
    // Child of the focus that contains the caret (file scope) or the active file (project scope).
    int current();

    void zoomIn(int index);  // focus a layer that has children
    void zoomOut();          // focus the parent
    void zoomTo(int index);  // any layer on the breadcrumb (or elsewhere)
    // A folder was opened or closed: nothing read from the previous one applies (facts are keyed
    // by workspace-relative path, so they would otherwise describe the wrong files, and grow).
    void workspaceChanged();
    // Shows the layer in the editor: opens its file, expands and reveals a region.
    void open(int index);

    // Bumps on every focus change; the sign of the last change (+1 in, -1 out) drives the animation.
    int zoomSerial() const { return zoomSerial_; }
    int zoomDirection() const { return zoomDirection_; }

private:
    void rebuild();
    std::string cacheKey() const;
    int addFile(int parent, const std::string& path, const std::string& title, bool withRegions);
    void setFocus(int index, int direction);
    // Project scope: line counts and header comments for the files of the focused folder, read on
    // demand so a large repository never gets parsed as a whole.
    void describeFocusedFiles();

    struct FileFacts {
        int lines = 0;
        std::string summary;
    };
    std::map<std::string, FileFacts> facts_;

    NotepadViewModel& owner_;
    std::vector<LayerNode> nodes_;
    std::string key_;
    std::string focusId_[3]; // per scope
    std::vector<std::string> loadedFiles_; // project scope: files whose parts are loaded
    int zoomSerial_ = 0;
    int zoomDirection_ = 0;
};

} // namespace enp::viewmodel
