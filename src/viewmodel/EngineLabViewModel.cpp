#include "enp/viewmodel/EngineLabViewModel.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace enp::viewmodel {

using interfaces::DownloadStatus;
using interfaces::EngineRunResult;
using interfaces::EngineRunSettings;

namespace {

// Pinned by commit and checked by SHA-256 (plan 23): nothing is fetched by name alone.
const EngineLabViewModel::CatalogFile kModel{
    "Gemma 4 E2B (Google QAT, Q4_0)", "gemma-4-E2B_q4_0-it.gguf",
    "https://huggingface.co/google/gemma-4-E2B-it-qat-q4_0-gguf/resolve/675cff42a74c774d6cb76f76d8eacb49b48c9b93/"
    "gemma-4-E2B_q4_0-it.gguf",
    "fa401b55b07ee70a54c6dae3903c783a6e65064312529ea57175cb5f8dec6634", 3349516256ull,
    "google/gemma-4-E2B-it-qat-q4_0-gguf"};
const EngineLabViewModel::CatalogFile kDrafter{
    "MTP drafter (Google QAT assistant, Q4_0)", "gemma-4-E2B-qat-mtp-q4_0.gguf",
    "https://huggingface.co/StartaBafras/gemma-4-E2B-it-qat-q4_0-assistant-GGUF/resolve/"
    "92b9c2ad5766eee7ed32eacdb73d0f04307438b4/gemma4-e2b-qat-mtp_Q4_0.gguf",
    "17e0a45021dc3a429fa2bab3e4b4aa0b7769278dbbf0a7e993fa177c052bbcb7", 76537376ull,
    "community conversion (StartaBafras) of google/gemma-4-E2B-it-qat-q4_0-unquantized-assistant"};

// Room kept free beyond the download itself.
constexpr uint64_t kSpareBytes = 512ull << 20;

std::string nowText()
{
    const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buf[32] = {};
    if (const std::tm* tm = std::localtime(&t))
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", tm);
    return buf;
}

std::string gigabytes(uint64_t bytes)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.2f GB", double(bytes) / 1e9);
    return buf;
}

model::Json settingsJson(const EngineRunSettings& s)
{
    return model::Json::object()
        .set("drafter", !s.drafterPath.empty())
        .set("gpu", s.useGpu)
        .set("threads", s.threads)
        .set("prompt", s.promptTokens)
        .set("generate", s.generateTokens);
}

model::Json resultJson(const EngineRunResult& r)
{
    return model::Json::object()
        .set("ok", r.ok)
        .set("error", r.error)
        .set("backend", r.backend)
        .set("load", r.loadSeconds)
        .set("pp", r.promptTokensPerSecond)
        .set("tg", r.generateTokensPerSecond)
        .set("ttft", r.firstTokenSeconds)
        .set("generated", r.generatedTokens)
        .set("drafted", r.drafted)
        .set("accepted", r.accepted)
        .set("peak", double(r.peakMemory))
        .set("sample", r.sample);
}

} // namespace

const EngineLabViewModel::CatalogFile& EngineLabViewModel::modelFile() { return kModel; }
const EngineLabViewModel::CatalogFile& EngineLabViewModel::drafterFile() { return kDrafter; }

EngineLabViewModel::EngineLabViewModel(interfaces::ILocalEngineService* engine) : engine_(engine)
{
    // The choices are remembered.
    for (Property<std::string>* p : {&customModelPath, &customDrafterPath})
        subscriptions_.push_back(p->subscribe([this](const std::string&) { changed.emit(); }));
    for (Property<bool>* p : {&useGpu, &allowMobileData})
        subscriptions_.push_back(p->subscribe([this](const bool&) { changed.emit(); }));
    subscriptions_.push_back(threads.subscribe([this](const int&) { changed.emit(); }));
}

const interfaces::EngineInfo& EngineLabViewModel::info()
{
    if (!infoLoaded_ && engine_) {
        info_ = engine_->info();
        infoLoaded_ = true;
        if (!threadsChosen_) {
            // Phones: the big cores (usually 4) beat all eight; desktops: half the threads.
            const int n = std::max(1, info_.cpuThreads);
            threads.set(info_.socName.empty() ? std::max(1, n / 2) : std::min(4, n));
        }
    }
    return info_;
}

std::string EngineLabViewModel::pathFor(Which which) const
{
    const std::string& custom = which == Which::Model ? customModelPath.get() : customDrafterPath.get();
    if (!custom.empty())
        return custom;
    if (!engine_)
        return {};
    const std::string dir = engine_->modelsDirectory();
    return dir.empty() ? std::string() : dir + "/" + (which == Which::Model ? kModel : kDrafter).fileName;
}

EngineLabViewModel::FileStatus EngineLabViewModel::fileStatus(Which which) const
{
    FileStatus s;
    const CatalogFile& file = which == Which::Model ? kModel : kDrafter;
    s.path = pathFor(which);
    s.total = file.size;
    if (!engine_ || s.path.empty())
        return s;
    if (downloading_ == int(which)) {
        s.state = FileState::Downloading;
        s.have = lastDownload_.received;
        if (lastDownload_.total)
            s.total = lastDownload_.total;
        s.note = lastDownload_.error;
        return s;
    }
    if (verifying_ == int(which)) {
        s.state = FileState::Verifying;
        s.verifyFraction = verifyFraction_;
        return s;
    }
    s.have = engine_->fileSize(s.path);
    const bool custom = !(which == Which::Model ? customModelPath.get() : customDrafterPath.get()).empty();
    if (s.have == 0) {
        s.state = FileState::Missing;
    } else if (custom) {
        s.state = FileState::Custom; // not the catalogue's file: nothing to check it against
        s.total = s.have;
    } else if (std::find(mismatched_.begin(), mismatched_.end(), s.path) != mismatched_.end()) {
        s.state = FileState::Mismatch;
    } else if (s.have < file.size) {
        s.state = FileState::Partial;
    } else {
        const auto it = std::find_if(verified_.begin(), verified_.end(), [&](const auto& v) { return v.first == s.path; });
        s.state = it != verified_.end() && it->second == file.sha256 ? FileState::Ready : FileState::Unverified;
    }
    return s;
}

bool EngineLabViewModel::fileUsable(Which which) const
{
    const FileState state = fileStatus(which).state;
    return state == FileState::Ready || state == FileState::Custom || state == FileState::Unverified;
}

void EngineLabViewModel::download(Which which)
{
    if (!engine_ || downloading_ >= 0)
        return;
    const CatalogFile& file = which == Which::Model ? kModel : kDrafter;
    // Always into the models folder: a custom path is the user's own file.
    const std::string dir = engine_->modelsDirectory();
    if (dir.empty()) {
        statusMessage.emit("No folder to keep models in on this device.");
        return;
    }
    const std::string path = dir + "/" + file.fileName;
    const uint64_t have = engine_->fileSize(path);
    const uint64_t need = file.size > have ? file.size - have : 0;
    const uint64_t free = engine_->freeSpace(path);
    if (free && free < need + kSpareBytes) {
        statusMessage.emit("Not enough space: " + gigabytes(need) + " needed plus " + gigabytes(kSpareBytes) +
                           " spare, " + gigabytes(free) + " free.");
        return;
    }
    if (which == Which::Model)
        customModelPath.set({});
    else
        customDrafterPath.set({});
    mismatched_.erase(std::remove(mismatched_.begin(), mismatched_.end(), path), mismatched_.end());
    std::string error;
    if (!engine_->startDownload(file.url, path, allowMobileData.get(), &error)) {
        statusMessage.emit("Download failed to start: " + error);
        return;
    }
    downloading_ = int(which);
    lastDownload_ = {};
    lastDownload_.state = DownloadStatus::State::Running;
    lastDownload_.received = have;
    statusMessage.emit("Downloading " + std::string(file.label) + "...");
}

void EngineLabViewModel::cancelDownload()
{
    if (!engine_ || downloading_ < 0)
        return;
    engine_->cancelDownload();
    downloading_ = -1;
    statusMessage.emit("Download stopped. Downloading again resumes it where possible.");
}

void EngineLabViewModel::verify(Which which)
{
    if (!engine_ || verifying_ >= 0)
        return;
    const std::string path = pathFor(which);
    const CatalogFile& file = which == Which::Model ? kModel : kDrafter;
    verifying_ = int(which);
    verifyFraction_ = 0;
    engine_->hashFile(
        path, [this](double fraction) { verifyFraction_ = fraction; },
        [this, path, expected = std::string(file.sha256)](const std::string& sha) {
            verifying_ = -1;
            verified_.erase(std::remove_if(verified_.begin(), verified_.end(), [&](const auto& v) { return v.first == path; }),
                            verified_.end());
            mismatched_.erase(std::remove(mismatched_.begin(), mismatched_.end(), path), mismatched_.end());
            if (sha == expected) {
                verified_.emplace_back(path, sha);
                statusMessage.emit("Checksum matches.");
            } else {
                mismatched_.push_back(path);
                statusMessage.emit(sha.empty() ? "The file could not be read." : "Checksum does not match: download it again.");
            }
            changed.emit();
        });
}

void EngineLabViewModel::refresh()
{
    if (!engine_ || downloading_ < 0)
        return;
    lastDownload_ = engine_->downloadStatus();
    if (lastDownload_.state == DownloadStatus::State::Running)
        return;
    const Which which = Which(downloading_);
    downloading_ = -1;
    if (lastDownload_.state == DownloadStatus::State::Done) {
        statusMessage.emit("Downloaded; checking the checksum...");
        verify(which);
    } else if (lastDownload_.state == DownloadStatus::State::Failed) {
        statusMessage.emit("Download failed: " + lastDownload_.error);
    }
}

std::string EngineLabViewModel::deviceLabel()
{
    const interfaces::EngineInfo& i = info();
    std::string label = i.socName.empty() ? i.cpuName : i.socName + " (" + i.cpuName + ")";
    if (i.memoryTotal)
        label += ", " + gigabytes(i.memoryTotal) + " RAM";
    return label;
}

void EngineLabViewModel::run(bool withDrafter) { startRun(withDrafter, false); }

void EngineLabViewModel::runComparison() { startRun(false, fileUsable(Which::Drafter)); }

void EngineLabViewModel::startRun(bool withDrafter, bool thenWithDrafter)
{
    if (!engine_ || running_)
        return;
    if (!fileUsable(Which::Model)) {
        statusMessage.emit("Download the model first.");
        return;
    }
    if (withDrafter && !fileUsable(Which::Drafter)) {
        statusMessage.emit("Download the drafter first.");
        return;
    }
    EngineRunSettings settings;
    settings.modelPath = pathFor(Which::Model);
    settings.drafterPath = withDrafter ? pathFor(Which::Drafter) : std::string();
    settings.useGpu = useGpu.get();
    settings.threads = std::max(1, threads.get());
    settings.promptTokens = std::clamp(promptTokens.get(), 16, 4096);
    settings.generateTokens = std::clamp(generateTokens.get(), 8, 1024);
    running_ = true;
    stage_ = "Starting";
    engine_->run(
        settings, [this](const std::string& stage) { stage_ = stage; },
        [this, settings, thenWithDrafter](const EngineRunResult& result) {
            running_ = false;
            stage_.clear();
            history_.insert(history_.begin(), Record{nowText(), deviceLabel(), settings, result});
            if (history_.size() > 100)
                history_.resize(100);
            changed.emit();
            if (!result.ok) {
                statusMessage.emit("Run failed: " + result.error);
                return;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.1f tokens/s generating, %.0f tokens/s reading the prompt",
                          result.generateTokensPerSecond, result.promptTokensPerSecond);
            statusMessage.emit(buf);
            if (thenWithDrafter)
                startRun(true, false);
        });
}

void EngineLabViewModel::cancelRun()
{
    if (engine_ && running_)
        engine_->cancel();
}

void EngineLabViewModel::clearHistory()
{
    history_.clear();
    changed.emit();
}

std::string EngineLabViewModel::report() const
{
    std::string out;
    char line[512];
    for (const Record& r : history_) {
        const auto& s = r.settings;
        const auto& x = r.result;
        std::snprintf(line, sizeof(line), "%s | %s | %s%s | ", r.when.c_str(), r.device.c_str(),
                      x.backend.empty() ? (s.useGpu ? "GPU" : "CPU") : x.backend.c_str(), s.drafterPath.empty() ? "" : " + drafter");
        out += line;
        if (!x.ok) {
            out += "failed: " + x.error + "\n";
            continue;
        }
        std::snprintf(line, sizeof(line),
                      "prompt %.0f tok/s | generate %.1f tok/s | first token %.2f s | load %.1f s | peak %.2f GB",
                      x.promptTokensPerSecond, x.generateTokensPerSecond, x.firstTokenSeconds, x.loadSeconds,
                      double(x.peakMemory) / 1e9);
        out += line;
        if (x.drafted > 0) {
            std::snprintf(line, sizeof(line), " | drafted %d, accepted %d (%.0f%%)", x.drafted, x.accepted,
                          100.0 * x.accepted / x.drafted);
            out += line;
        }
        out += "\n";
    }
    return out;
}

// {"customModel": "...", "customDrafter": "...", "threads": 4, "gpu": false, "mobileData": false,
//  "verified": [{"path": "...", "sha256": "..."}], "history": [{"when", "device", "settings", "result"}]}
model::Json EngineLabViewModel::save() const
{
    model::Json verified = model::Json::array();
    for (const auto& [path, sha] : verified_)
        verified.push(model::Json::object().set("path", path).set("sha256", sha));
    model::Json history = model::Json::array();
    for (const Record& r : history_)
        history.push(model::Json::object()
                         .set("when", r.when)
                         .set("device", r.device)
                         .set("settings", settingsJson(r.settings))
                         .set("result", resultJson(r.result)));
    return model::Json::object()
        .set("customModel", customModelPath.get())
        .set("customDrafter", customDrafterPath.get())
        .set("threads", threads.get())
        .set("gpu", useGpu.get())
        .set("mobileData", allowMobileData.get())
        .set("verified", verified)
        .set("history", history);
}

void EngineLabViewModel::load(const model::Json& j)
{
    if (!j.isObject())
        return;
    customModelPath.set(j.getString("customModel"));
    customDrafterPath.set(j.getString("customDrafter"));
    if (const model::Json* t = j.find("threads"); t && t->isNumber()) {
        threads.set(std::max(1, int(t->asNumber())));
        threadsChosen_ = true;
    }
    useGpu.set(j.getBool("gpu"));
    allowMobileData.set(j.getBool("mobileData"));
    verified_.clear();
    if (const model::Json* v = j.find("verified"))
        for (const model::Json& e : v->items())
            verified_.emplace_back(e.getString("path"), e.getString("sha256"));
    history_.clear();
    if (const model::Json* h = j.find("history")) {
        for (const model::Json& e : h->items()) {
            Record r;
            r.when = e.getString("when");
            r.device = e.getString("device");
            if (const model::Json* s = e.find("settings")) {
                r.settings.drafterPath = s->getBool("drafter") ? "drafter" : "";
                r.settings.useGpu = s->getBool("gpu");
                r.settings.threads = int(s->getNumber("threads", 4));
                r.settings.promptTokens = int(s->getNumber("prompt", 512));
                r.settings.generateTokens = int(s->getNumber("generate", 128));
            }
            if (const model::Json* x = e.find("result")) {
                r.result.ok = x->getBool("ok");
                r.result.error = x->getString("error");
                r.result.backend = x->getString("backend");
                r.result.loadSeconds = x->getNumber("load");
                r.result.promptTokensPerSecond = x->getNumber("pp");
                r.result.generateTokensPerSecond = x->getNumber("tg");
                r.result.firstTokenSeconds = x->getNumber("ttft");
                r.result.generatedTokens = int(x->getNumber("generated"));
                r.result.drafted = int(x->getNumber("drafted"));
                r.result.accepted = int(x->getNumber("accepted"));
                r.result.peakMemory = uint64_t(x->getNumber("peak"));
                r.result.sample = x->getString("sample");
            }
            history_.push_back(std::move(r));
        }
    }
}

} // namespace enp::viewmodel
