// The app's side of an engine library (plan 21): the library is loaded only in a helper process,
// `EasyNotepad --local-engine <library>`, which this class starts, talks to over its stdin and
// stdout (one JSON object a line) and ends. A fault in the library or its driver (a crash, an abort,
// a hang) ends the helper, never the app; the caller then answers on its own CPU engine.
#pragma once

#include "enp/interfaces/ILocalEngineService.h"
#include "enp/model/Json.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace enp::services {

class EngineHelper {
public:
    struct Options {
        std::string library;              // the engine library, e.g. <app folder>/enp-engine-cuda.dll
        std::vector<std::string> command; // the helper: {<app>, "--local-engine", library}
        int startSeconds = 60;            // to load the library and say what it can use
        int silenceSeconds = 180;         // the longest wait for any sign of progress in a request
        int cancelSeconds = 15;           // for the answer after a cancel
    };
    explicit EngineHelper(Options options);
    ~EngineHelper(); // ends the helper
    EngineHelper(const EngineHelper&) = delete;
    EngineHelper& operator=(const EngineHelper&) = delete;

    const Options& options() const { return options_; }

    // What the library said it is: its name, version and devices.
    struct Description {
        std::string name;
        std::string version;
        std::vector<interfaces::EngineDevice> devices;
    };

    // Starts the helper unless it runs, and waits until it is ready. Blocks: call from a worker.
    // False, with `reason` in words, when the library cannot be used.
    bool start(std::string& reason, Description* description = nullptr);

    using Report = std::function<void(const std::string&)>;
    // Sends `request` and waits for its result, passing on stages and text as they come. Blocks.
    // False, with `reason`, when the helper ended, went silent or did not stop when asked (it has
    // been ended then); the next request starts a new one.
    bool request(const model::Json& request, const Report& stage, const Report& text, const std::function<bool()>& cancelled,
                 model::Json& result, std::string& reason);

    // Ends the helper (its memory back to the system), at once; the next request starts another.
    void stop();
    bool running() const;

private:
    struct Session;
    Options options_;
    mutable std::mutex mutex_;
    std::shared_ptr<Session> session_;
};

} // namespace enp::services
