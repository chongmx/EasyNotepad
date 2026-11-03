// Integrated terminals: one TerminalScreen per shell session, the active one, and keyboard/paste
// input. Works without a terminal service (supported() is false: the view explains why).
#pragma once

#include "enp/interfaces/ITerminalService.h"
#include "enp/model/TerminalScreen.h"
#include "enp/viewmodel/Observable.h"

#include <memory>
#include <string>
#include <vector>

namespace enp::viewmodel {

struct TerminalInstance {
    int id = 0;
    std::string name; // shell profile name
    model::TerminalScreen screen;
    std::unique_ptr<interfaces::ITerminalSession> session;
    bool exited = false;
    int exitCode = 0;
};

class TerminalViewModel {
public:
    explicit TerminalViewModel(interfaces::ITerminalService* service);
    ~TerminalViewModel();
    TerminalViewModel(const TerminalViewModel&) = delete;
    TerminalViewModel& operator=(const TerminalViewModel&) = delete;

    Property<int> activeIndex{-1};
    Property<std::string> lastError;
    Event<> focusRequested;
    Event<std::string> statusMessage;

    bool supported() const { return service_ && service_->isSupported(); }
    const std::vector<interfaces::ShellProfile>& shells() const;
    const std::vector<std::unique_ptr<TerminalInstance>>& terminals() const { return terminals_; }
    TerminalInstance* active();
    const TerminalInstance* active() const;

    // Starts a shell (index into shells()); returns false and sets lastError on failure.
    bool newTerminal(int shellIndex = 0);
    // Creates the first terminal on demand (when the terminal panel is first shown).
    void ensureTerminal();
    void activate(int index);
    void close(int index);
    void killActive();
    void clearActive();

    // Raw bytes for the active session (already encoded by model::encodeKey & co.).
    void sendInput(std::string_view bytes);
    void paste(std::string_view text);
    // Size of the visible grid; forwarded to the active session when it changes.
    void resize(int columns, int rows);

    // Directory new shells start in; empty means the application's working directory.
    // Where new terminals start: the open folder, else the home directory (NotepadViewModel keeps
    // it current). Terminals already running keep theirs.
    // Swapped in, not assigned: assigning a short path into the string that held a long one keeps
    // the long one's heap storage (docs/MEMORY.md rule 5), the closed folder's path among it.
    void setWorkingDirectory(std::string directory) { workingDirectory_.swap(directory); }
    const std::string& workingDirectory() const { return workingDirectory_; }

private:
    TerminalInstance* find(int id);
    void flushResponses(TerminalInstance& terminal);

    interfaces::ITerminalService* service_;
    std::vector<std::unique_ptr<TerminalInstance>> terminals_;
    std::string workingDirectory_;
    int columns_ = 80;
    int rows_ = 24;
    int nextId_ = 1;
};

} // namespace enp::viewmodel
