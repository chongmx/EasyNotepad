// Desktop and web downloads through curl (see Download.h). The web build has no processes, so
// start() reports that it cannot download.
#include "enp/os/Download.h"

#include "enp/os/Dispatch.h"
#include "enp/os/Fs.h"
#include "enp/os/Process.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace enp::os::download {

namespace {

struct Job {
    std::mutex mutex;
    Status status;
    std::string path;
    uint64_t resumedFrom = 0;
    process::CancelFlag cancel;
};

std::shared_ptr<Job>& current()
{
    static std::shared_ptr<Job> job;
    return job;
}

// The size of the file on disk is the progress (curl's own meter is not parsed).
uint64_t fileSize(const std::string& path) { return fs::stat(path).size; }

} // namespace

std::string directory()
{
    static const std::string dir = []() {
        const std::string base = fs::appDataDirectory();
        if (base.empty())
            return std::string();
        std::string d = fs::path::join(base, "models");
        fs::createDirectories(d);
        return d;
    }();
    return dir;
}

bool start(const std::string& url, const std::string& path, bool /*allowMetered*/, std::string* error)
{
    if (current() && status().state == Status::State::Running) {
        if (error)
            *error = "a download is already running";
        return false;
    }
    const std::string curl = process::findExecutable("curl");
    if (curl.empty()) {
        if (error)
            *error = "curl was not found on PATH";
        return false;
    }
    auto job = std::make_shared<Job>();
    job->path = path;
    job->resumedFrom = fileSize(path);
    job->cancel = std::make_shared<std::atomic<bool>>(false);
    job->status.state = Status::State::Running;
    job->status.received = job->resumedFrom;
    current() = job;

    dispatch::runAsync([job, curl, url, path]() {
        process::Command command;
        // --continue-at - resumes; --fail turns HTTP errors into a failure. The caller knows the
        // total size from its catalogue.
        command.arguments = {curl, "--location", "--fail", "--silent", "--show-error", "--retry", "5",
                             "--continue-at", "-", "--output", path, url};
        command.timeoutSeconds = 24 * 3600;
        command.cancel = job->cancel;
        const process::Result result = process::run(command);
        std::lock_guard<std::mutex> lock(job->mutex);
        if (result.cancelled) {
            job->status.state = Status::State::Idle;
        } else if (!result.started || result.exitCode != 0) {
            job->status.state = Status::State::Failed;
            job->status.error = !result.error.empty() ? result.error : result.errorOutput;
            if (job->status.error.empty())
                job->status.error = "curl exited with code " + std::to_string(result.exitCode);
        } else {
            job->status.state = Status::State::Done;
        }
    });
    return true;
}

Status status()
{
    const std::shared_ptr<Job> job = current();
    if (!job)
        return {};
    std::lock_guard<std::mutex> lock(job->mutex);
    Status s = job->status;
    s.received = fileSize(job->path);
    return s;
}

bool meteredNetwork() { return false; }

bool canImport() { return false; }

bool importFile(const std::string&, std::string* error)
{
    if (error)
        *error = "not needed here: enter the file's path instead";
    return false;
}

void cancel()
{
    if (const std::shared_ptr<Job> job = current())
        job->cancel->store(true);
}

} // namespace enp::os::download
