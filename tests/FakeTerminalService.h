// In-process ITerminalService for tests: a "shell" that echoes input like a tty, answers Enter
// with a line of output and a coloured prompt, and delivers everything when pump() is called
// (as the real service does on the main thread).
#pragma once

#include "enp/interfaces/ITerminalService.h"

#include <functional>
#include <string>
#include <vector>

namespace enp::tests {

class FakeTerminalService final : public interfaces::ITerminalService {
public:
    static constexpr const char* kPrompt = "\x1b[32muser@demo\x1b[0m:\x1b[34m~\x1b[0m$ ";

    struct Session final : interfaces::ITerminalSession {
        FakeTerminalService* service = nullptr;
        Callbacks callbacks;
        std::string line;
        std::string written; // everything the terminal sent to the "process"
        int columns = 0, rows = 0;
        bool* alive = nullptr;

        ~Session() override
        {
            if (alive)
                *alive = false;
        }
        void write(std::string_view bytes) override
        {
            written += bytes;
            for (char c : bytes) {
                if (c == '\r') {
                    const std::string command = line;
                    line.clear();
                    service->queue(this, "\r\nran: " + command + "\r\n" + kPrompt);
                } else if (c == '\x7f') {
                    if (!line.empty()) {
                        line.pop_back();
                        service->queue(this, "\b \b");
                    }
                } else if (static_cast<unsigned char>(c) >= 0x20) {
                    line += c;
                    service->queue(this, std::string(1, c));
                }
            }
        }
        void resize(int c, int r) override
        {
            columns = c;
            rows = r;
        }
        void terminate() override { service->queueExit(this, 1); }
    };

    bool supported = true;
    bool failStart = false;
    std::vector<Session*> sessions; // alive sessions, in creation order

    bool isSupported() const override { return supported; }
    const std::vector<interfaces::ShellProfile>& shells() const override { return shells_; }

    std::unique_ptr<interfaces::ITerminalSession> start(const interfaces::ShellProfile&, const std::string&, int columns,
                                                        int rows, Callbacks callbacks, std::string* error) override
    {
        if (failStart) {
            if (error)
                *error = "fake start failure";
            return nullptr;
        }
        auto session = std::make_unique<Session>();
        session->service = this;
        session->callbacks = std::move(callbacks);
        session->columns = columns;
        session->rows = rows;
        aliveFlags_.push_back(std::make_unique<bool>(true));
        session->alive = aliveFlags_.back().get();
        sessions.push_back(session.get());
        queue(session.get(), std::string("Fake shell\r\n") + kPrompt);
        return session;
    }

    void queue(Session* s, std::string bytes)
    {
        bool* alive = s->alive;
        pending_.push_back([s, alive, bytes = std::move(bytes)]() {
            if (*alive)
                s->callbacks.output(bytes);
        });
    }
    void queueExit(Session* s, int code)
    {
        bool* alive = s->alive;
        pending_.push_back([s, alive, code]() {
            if (*alive)
                s->callbacks.exited(code);
        });
    }
    // Delivers queued output, like os::dispatch::pump() does for the real service.
    void pump()
    {
        std::vector<std::function<void()>> tasks;
        tasks.swap(pending_);
        for (auto& task : tasks)
            task();
    }

private:
    std::vector<interfaces::ShellProfile> shells_{{"fakesh", "fakesh"}, {"othersh", "othersh"}};
    std::vector<std::function<void()>> pending_;
    std::vector<std::unique_ptr<bool>> aliveFlags_;
};

} // namespace enp::tests
