// The local model's tools: EasyNotepad's own facilities (the open folder, the document readers,
// the viewer, the agent's review of changes) for the agent panel's local model, and the guide and
// skills (resources/skills) that teach it to use them.
//
// Calls arrive on the main thread and are answered there; reading a document runs in the
// background, and a look at a page waits for the page to be drawn and the window captured.
#pragma once

#include "enp/interfaces/ILocalEngineService.h"
#include "enp/model/Json.h"
#include "enp/viewmodel/AgentViewModel.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace enp::viewmodel {
class NotepadViewModel;
}
namespace enp::view {
class MainView;
}

namespace enp::app {

class LocalAgentTools {
public:
    using Done = std::function<void(viewmodel::AgentViewModel::ToolOutput)>;

    // `view` gives the window's view (null before it exists: looks at pages then fail).
    LocalAgentTools(viewmodel::NotepadViewModel& vm, std::function<view::MainView*()> view);
    ~LocalAgentTools();

    // Hands the tools, the runner and the guide to the agent panel.
    void attach();

    std::vector<interfaces::LocalTool> tools(bool canEdit) const;
    void run(const interfaces::LocalToolCall& call, Done done);
    // Told to the model with every question: where it works, what it can do, its skills.
    std::string guide() const;

    // Once a frame: a look at a page waits some frames for the page to be read and drawn.
    void tick();
    // A look is ready to be captured: the main loop captures this frame and hands the pixels over.
    bool captureWanted() const;
    void deliverCapture(bool ok, int width, int height, const std::vector<unsigned char>& rgba);

private:
    struct Look {
        Done done;
        std::string what;   // "page 3 of report.pdf at 200%"
        std::string region; // "all", "top", "bottom-left", ...
        int frames = 0;
    };
    viewmodel::NotepadViewModel& vm_;
    std::function<view::MainView*()> view_;
    std::vector<Look> looks_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // background reads answer only while it lives

    // The absolute path of `path` (workspace-relative or absolute) when it is inside the open folder
    // or is an open document; empty (and `error` set) when the model may not go there.
    std::string resolve(const std::string& path, std::string& error) const;
    std::string fileText(const std::string& absolute, bool& open) const; // an open document's text first

    void listFiles(const model::Json& args, Done done);
    void readFile(const model::Json& args, Done done);
    void searchFiles(const model::Json& args, Done done);
    void readDocument(const model::Json& args, Done done);
    void lookAtPage(const model::Json& args, Done done);
    void lookAtImage(const model::Json& args, Done done);
    void listenTo(const model::Json& args, Done done);
    void openFile(const model::Json& args, Done done);
    void editFile(const model::Json& args, Done done);
    void writeFile(const model::Json& args, Done done);
    void readSkill(const model::Json& args, Done done);
};

} // namespace enp::app
