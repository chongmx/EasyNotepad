#include "enp/services/EngineHelper.h"

#include "enp/engine/EngineJson.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Process.h"

#include <chrono>
#include <condition_variable>
#include <deque>

namespace enp::services {

using model::Json;
using Clock = std::chrono::steady_clock;

// One helper process: its input, its events as they arrive, and whether it ended.
struct EngineHelper::Session {
    std::shared_ptr<os::process::InputStream> input = std::make_shared<os::process::InputStream>();
    os::process::CancelFlag kill = std::make_shared<std::atomic<bool>>(false);
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<Json> events;
    std::string partial; // output after the last newline
    bool ready = false;
    bool ended = false;
    std::string endedWhy;
    Description description;

    void output(const char* data, size_t size)
    {
        std::lock_guard<std::mutex> lock(mutex);
        partial.append(data, size);
        size_t start = 0;
        for (size_t nl; (nl = partial.find('\n', start)) != std::string::npos; start = nl + 1) {
            std::string_view line(partial.data() + start, nl - start);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            // Anything that is not one of our objects (a library printing to stdout) is skipped.
            if (std::optional<Json> j = Json::parse(line); j && j->isObject() && j->find("event"))
                events.push_back(std::move(*j));
        }
        partial.erase(0, start);
        if (partial.size() > (1u << 20))
            partial.clear(); // a runaway line without an end: not ours
        changed.notify_all();
    }

    void end(const os::process::Result& r)
    {
        std::lock_guard<std::mutex> lock(mutex);
        ended = true;
        if (!r.started)
            endedWhy = r.error.empty() ? "the helper could not start" : r.error;
        else if (kill->load())
            endedWhy = "ended";
        else {
            endedWhy = "the helper stopped (exit code " + std::to_string(r.exitCode) + ")";
            std::string err = r.errorOutput;
            while (!err.empty() && (err.back() == '\n' || err.back() == '\r'))
                err.pop_back();
            if (!err.empty())
                endedWhy += ": " + (err.size() > 300 ? "..." + err.substr(err.size() - 300) : err);
        }
        events.clear();
        changed.notify_all();
    }
};

EngineHelper::EngineHelper(Options options) : options_(std::move(options)) {}

EngineHelper::~EngineHelper() { stop(); }

bool EngineHelper::running() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!session_)
        return false;
    std::lock_guard<std::mutex> s(session_->mutex);
    return !session_->ended;
}

void EngineHelper::stop()
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s.swap(session_);
    }
    if (s) {
        s->kill->store(true); // the process (and all it started) ends within a fraction of a second
        s->input->close();
    }
}

bool EngineHelper::start(std::string& reason, Description* description)
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (session_) {
            std::lock_guard<std::mutex> sl(session_->mutex);
            if (!session_->ended && session_->ready) {
                if (description)
                    *description = session_->description;
                return true;
            }
            session_->kill->store(true); // one that never got ready: ended before the next
            session_->input->close();
        }
        s = std::make_shared<Session>();
        session_ = s;
    }
    os::process::Command command;
    command.arguments = options_.command;
    command.inputStream = s->input;
    command.cancel = s->kill;
    command.timeoutSeconds = 1 << 30; // it lives until it is ended
    command.onOutput = [s](const char* data, size_t size) { s->output(data, size); };
    os::dispatch::runAsync([s, command]() { s->end(os::process::run(command)); });

    std::unique_lock<std::mutex> lock(s->mutex);
    const auto deadline = Clock::now() + std::chrono::seconds(options_.startSeconds);
    for (;;) {
        while (!s->events.empty()) {
            Json e = std::move(s->events.front());
            s->events.pop_front();
            if (e.getString("event") != "ready")
                continue;
            if (!e.getBool("ok")) {
                reason = e.getString("error", "the engine library cannot run here");
                lock.unlock();
                stop();
                return false;
            }
            if (const Json* engine = e.find("engine")) {
                s->description.name = engine->getString("name");
                s->description.version = engine->getString("version");
                if (const Json* devices = engine->find("devices"))
                    for (const Json& d : devices->items())
                        s->description.devices.push_back(engine::deviceFrom(d));
            }
            s->ready = true;
            if (description)
                *description = s->description;
            return true;
        }
        if (s->ended) {
            reason = s->endedWhy;
            lock.unlock();
            stop();
            return false;
        }
        if (s->changed.wait_until(lock, deadline) == std::cv_status::timeout && s->events.empty() && !s->ended) {
            reason = "the engine did not start within " + std::to_string(options_.startSeconds) + " s";
            lock.unlock();
            stop();
            return false;
        }
    }
}

bool EngineHelper::request(const Json& request, const Report& stage, const Report& text, const std::function<bool()>& cancelled,
                           Json& result, std::string& reason)
{
    std::shared_ptr<Session> s;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        s = session_;
    }
    if (!s) {
        reason = "the engine is not running";
        return false;
    }
    s->input->write(request.dump() + "\n");
    auto lastSign = Clock::now();
    bool cancelSent = false;
    Clock::time_point cancelAt;
    std::unique_lock<std::mutex> lock(s->mutex);
    for (;;) {
        while (!s->events.empty()) {
            Json e = std::move(s->events.front());
            s->events.pop_front();
            lastSign = Clock::now();
            const std::string kind = e.getString("event");
            if (kind == "done") {
                if (const Json* r = e.find("result"))
                    result = *r;
                return true;
            }
            const std::string piece = e.getString("text");
            lock.unlock(); // the callbacks post to the main thread; no lock held meanwhile
            if (kind == "stage" && stage)
                stage(piece);
            else if (kind == "text" && text)
                text(piece);
            lock.lock();
        }
        if (s->ended) {
            reason = s->endedWhy;
            lock.unlock();
            stop();
            return false;
        }
        const auto now = Clock::now();
        if (!cancelSent && cancelled && cancelled()) {
            cancelSent = true;
            cancelAt = now;
            s->input->write("{\"op\":\"cancel\"}\n");
        }
        if (cancelSent && now - cancelAt > std::chrono::seconds(options_.cancelSeconds)) {
            reason = "the engine did not stop when asked";
            lock.unlock();
            stop();
            return false;
        }
        if (now - lastSign > std::chrono::seconds(options_.silenceSeconds)) {
            reason = "the engine stopped responding";
            lock.unlock();
            stop();
            return false;
        }
        s->changed.wait_for(lock, std::chrono::milliseconds(100));
    }
}

} // namespace enp::services
