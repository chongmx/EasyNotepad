#include "enp/viewmodel/TerminalViewModel.h"

#include <algorithm>

namespace enp::viewmodel {

TerminalViewModel::TerminalViewModel(interfaces::ITerminalService* service) : service_(service) {}

TerminalViewModel::~TerminalViewModel() = default;

const std::vector<interfaces::ShellProfile>& TerminalViewModel::shells() const
{
    static const std::vector<interfaces::ShellProfile> none;
    return service_ ? service_->shells() : none;
}

TerminalInstance* TerminalViewModel::active()
{
    const int index = activeIndex.get();
    return index >= 0 && index < int(terminals_.size()) ? terminals_[size_t(index)].get() : nullptr;
}

const TerminalInstance* TerminalViewModel::active() const
{
    const int index = activeIndex.get();
    return index >= 0 && index < int(terminals_.size()) ? terminals_[size_t(index)].get() : nullptr;
}

TerminalInstance* TerminalViewModel::find(int id)
{
    for (auto& t : terminals_)
        if (t->id == id)
            return t.get();
    return nullptr;
}

bool TerminalViewModel::newTerminal(int shellIndex)
{
    if (!supported()) {
        lastError.set("Terminals are not available on this platform");
        return false;
    }
    const auto& profiles = shells();
    if (profiles.empty()) {
        lastError.set("No shell was found");
        return false;
    }
    const interfaces::ShellProfile& shell = profiles[size_t(std::clamp(shellIndex, 0, int(profiles.size()) - 1))];

    auto terminal = std::make_unique<TerminalInstance>();
    terminal->id = nextId_++;
    terminal->name = shell.name;
    terminal->screen.resize(columns_, rows_);
    const int id = terminal->id;

    interfaces::ITerminalService::Callbacks callbacks;
    callbacks.output = [this, id](std::string_view bytes) {
        if (TerminalInstance* t = find(id)) {
            t->screen.feed(bytes);
            flushResponses(*t);
        }
    };
    callbacks.exited = [this, id](int code) {
        if (TerminalInstance* t = find(id)) {
            t->exited = true;
            t->exitCode = code;
            t->screen.feed("\r\n\x1b[0m\x1b[90m[Process exited with code " + std::to_string(code) +
                           ". Press any key to close.]\x1b[0m");
        }
    };
    std::string error;
    terminal->session = service_->start(shell, workingDirectory_, columns_, rows_, std::move(callbacks), &error);
    if (!terminal->session) {
        lastError.set(error.empty() ? "Could not start " + shell.name : error);
        statusMessage.emit(lastError.get());
        return false;
    }
    lastError.set({});
    terminals_.push_back(std::move(terminal));
    activeIndex.set(int(terminals_.size()) - 1);
    focusRequested.emit();
    return true;
}

void TerminalViewModel::ensureTerminal()
{
    if (terminals_.empty() && supported() && lastError.get().empty())
        newTerminal(0);
}

void TerminalViewModel::activate(int index)
{
    if (index < 0 || index >= int(terminals_.size()))
        return;
    activeIndex.set(index);
    // The grid size may have changed while this terminal was hidden.
    TerminalInstance* t = active();
    if (t->screen.columns() != columns_ || t->screen.rows() != rows_) {
        t->screen.resize(columns_, rows_);
        if (t->session && !t->exited)
            t->session->resize(columns_, rows_);
    }
    focusRequested.emit();
}

void TerminalViewModel::close(int index)
{
    if (index < 0 || index >= int(terminals_.size()))
        return;
    terminals_.erase(terminals_.begin() + index); // destroying the session ends the process
    const int active = activeIndex.get();
    activeIndex.set(-1);
    if (!terminals_.empty())
        activate(std::clamp(index <= active ? active - 1 : active, 0, int(terminals_.size()) - 1));
}

void TerminalViewModel::killActive()
{
    close(activeIndex.get());
}

void TerminalViewModel::clearActive()
{
    if (TerminalInstance* t = active())
        t->screen.clearKeepingCursorLine();
}

void TerminalViewModel::sendInput(std::string_view bytes)
{
    TerminalInstance* t = active();
    if (!t || bytes.empty())
        return;
    if (t->exited) { // "Press any key to close."
        close(activeIndex.get());
        return;
    }
    t->session->write(bytes);
}

void TerminalViewModel::paste(std::string_view text)
{
    if (const TerminalInstance* t = active())
        sendInput(model::encodePaste(text, t->screen.bracketedPaste()));
}

void TerminalViewModel::resize(int columns, int rows)
{
    columns = std::max(2, columns);
    rows = std::max(1, rows);
    if (columns == columns_ && rows == rows_)
        return;
    columns_ = columns;
    rows_ = rows;
    if (TerminalInstance* t = active()) {
        t->screen.resize(columns, rows);
        if (t->session && !t->exited)
            t->session->resize(columns, rows);
    }
}

void TerminalViewModel::flushResponses(TerminalInstance& terminal)
{
    const std::string responses = terminal.screen.takeResponses();
    if (!responses.empty() && terminal.session && !terminal.exited)
        terminal.session->write(responses);
}

} // namespace enp::viewmodel
