// The logical (functional) map of the workspace: built locally by dissecting the code, refined by
// an agent CLI on request (cached in .easynotepad/logical-map.json), navigated by zooming.
#pragma once

#include "enp/interfaces/IAgentCliService.h"
#include "enp/model/LogicalMap.h"
#include "enp/viewmodel/Observable.h"

#include <memory>
#include <string>
#include <vector>

namespace enp::viewmodel {

class NotepadViewModel;

enum class MapLens { Architecture, Features };

class LogicalMapViewModel {
public:
    static constexpr const char* kCachePath = ".easynotepad/logical-map.json";

    LogicalMapViewModel(NotepadViewModel& owner, interfaces::IAgentCliService* agent);

    Property<bool> analyzing{false};
    Property<MapLens> lens{MapLens::Architecture};
    Event<std::string> statusMessage;

    const model::LogicalMap& map();
    bool hasFeatures();
    const char* sourceName(); // "local dissection" / "agent dissection"

    // Navigation (per lens).
    std::string focus();
    std::vector<std::string> breadcrumb();
    void zoomTo(const std::string& id);
    void zoomIn(const std::string& id); // only nodes with children
    void zoomOut();
    // Shows the node's code in the editor (switching the editor to code).
    void open(const std::string& id);
    // Focuses the map on the code under the caret and highlights it.
    void revealCaret();
    const std::string& highlight() const { return highlight_; }
    int zoomSerial() const { return zoomSerial_; }
    int zoomDirection() const { return zoomDirection_; }

    void refresh();          // dissect locally again (drops agent results)
    void dissectWithAgent(); // ask the agent CLI to refine the map
    bool agentAvailable() const;
    std::string agentCommand() const;

private:
    void ensure();
    std::vector<model::SourceFile> collectSources();
    std::string root() const;
    void setFocus(const std::string& id, int direction);

    NotepadViewModel& owner_;
    interfaces::IAgentCliService* agent_;
    model::LogicalMap map_;
    std::string key_;
    std::string focus_[2]; // per lens
    std::string highlight_;
    int zoomSerial_ = 0;
    int zoomDirection_ = 0;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
};

} // namespace enp::viewmodel
