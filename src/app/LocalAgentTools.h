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
    // `engine` transcribes recordings (null: transcribe says it cannot).
    LocalAgentTools(viewmodel::NotepadViewModel& vm, std::function<view::MainView*()> view,
                    interfaces::ILocalEngineService* engine = nullptr);
    // Where its memories are kept (a JSON file in the app's data folder; tests point elsewhere).
    void setMemoryPath(std::string path) { memoryPath_ = std::move(path); }
    const std::string& memoryPath() const { return memoryPath_; }
    // How a tool is called, with an example of each argument: told back when a call is wrong.
    std::string usage(const std::string& name) const;
    // Where tools put the pictures they make (temp/agent-images next to the app): kept a day. A tool
    // that makes pictures (a diffusion model, a segmenter...) saves them here and lists them in
    // ToolOutput::shown, and the chat shows them.
    static std::string imagesDirectory();
    // Saves an RGB picture there as PNG; its path, or empty when it could not be written.
    static std::string savePicture(const interfaces::LocalMedia& rgb, const std::string& stem);
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

    // The part of a picture or page a look asks for (its region, zoom and at), as fractions x0, y0,
    // x1, y1; false with why when the region cannot be read.
    static bool lookBox(const model::Json& args, float box[4], std::string& why);
    // The tokens a look spends on its picture: its detail, at least the user's minimum.
    int pictureTokens(const std::string& detail) const;

private:
    struct Look {
        Done done;
        std::string what;   // "page 3 of report.pdf at 200%"
        int page = -1;      // the page looked at (from 0), and the part of it: fractions x0, y0, x1, y1
        float box[4] = {0, 0, 1, 1};
        int documentId = -1;
        int tokens = 280;   // what the picture costs the model (its detail)
        int frames = 0;
    };
    viewmodel::NotepadViewModel& vm_;
    std::function<view::MainView*()> view_;
    std::vector<Look> looks_;
    interfaces::ILocalEngineService* engine_ = nullptr;
    std::string memoryPath_;
    std::vector<std::string> notes_; // working notes, of conversation notesConversation_
    int notesConversation_ = -1;
    std::string session_; // this run of the app: temporary memories belong to it
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true); // background reads answer only while it lives

    // The absolute path of `path` (workspace-relative or absolute) when it is inside the open folder
    // or is an open document; empty (and `error` set) when the model may not go there.
    std::string resolve(const std::string& path, std::string& error) const;
    std::string fileText(const std::string& absolute, bool& open) const; // an open document's text first

    void listFiles(const model::Json& args, Done done);
    void readFile(const model::Json& args, Done done);
    void searchFiles(const model::Json& args, Done done);
    void readDocument(const model::Json& args, Done done);
    // Many documents: search inside them all, ask one question of each (a fresh pass per document,
    // long ones part by part), and working notes that outlast the conversation's length.
    void searchDocuments(const model::Json& args, Done done);
    void extractFromDocuments(const model::Json& args, Done done);
    void note(const model::Json& args, Done done);
    void readNotes(const model::Json& args, Done done);
    std::vector<std::string>& notes(); // this conversation's (a new chat or folder starts them again)
    struct ExtractJob;
    void extractPass(std::shared_ptr<ExtractJob> job, Done done); // one part of one document, then the next
    bool documentFiles(const model::Json& args, const std::string& root, std::vector<std::string>& files, std::string& why) const;
    // Looking for something with the local model: a long document in overlapping windows, a picture
    // in overlapping tiles that are cut finer where something was seen (coarse to fine).
    void scanDocument(const model::Json& args, Done done);
    void scanImage(const model::Json& args, Done done);
    struct ScanJob;
    void scanPass(std::shared_ptr<ScanJob> job, Done done);
    interfaces::LocalChatRequest passRequest(int maxTokens) const; // the exchange's model, for one short pass
    void lookAtPage(const model::Json& args, Done done);
    void lookAtImage(const model::Json& args, Done done);
    void listenTo(const model::Json& args, Done done);
    void openFile(const model::Json& args, Done done);
    void editFile(const model::Json& args, Done done);
    void writeFile(const model::Json& args, Done done);
    void readSkill(const model::Json& args, Done done);
    void showImage(const model::Json& args, Done done);
    // Memory: what it learnt about the user and the work, kept between conversations.
    void remember(const model::Json& args, Done done);
    void recall(const model::Json& args, Done done);
    void amendMemory(const model::Json& args, Done done);
    void forgetMemory(const model::Json& args, Done done);
    void rewriteMemories(const model::Json& args, Done done);
    model::Json loadMemories() const; // expired notes and other sessions' temporary ones left out
    bool saveMemories(const model::Json& list) const;
    std::string memorySummary() const; // for the guide
    // Work tools.
    void calculate(const model::Json& args, Done done);
    void queryTable(const model::Json& args, Done done);
    void inspectBinary(const model::Json& args, Done done);
    void readBytes(const model::Json& args, Done done);
    void transcribe(const model::Json& args, Done done);
};

} // namespace enp::app
