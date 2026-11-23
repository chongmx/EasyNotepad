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
// The model's eyes and ears: its vision and audio encoders with their projectors (mtmd), from the
// model's own repository at the same commit.
const EngineLabViewModel::CatalogFile kProjector{
    "Eyes and ears (Google, vision and audio)", "gemma-4-E2B-it-mmproj.gguf",
    "https://huggingface.co/google/gemma-4-E2B-it-qat-q4_0-gguf/resolve/675cff42a74c774d6cb76f76d8eacb49b48c9b93/"
    "gemma-4-E2B-it-mmproj.gguf",
    "021059cce659fe7f9170d5599761d7bbaf644b798dab9503aca30dc43e6beb14", 986833664ull,
    "google/gemma-4-E2B-it-qat-q4_0-gguf"};

// The bigger models, from the same Google series (QAT, Q4_0), pinned the same way.
const EngineLabViewModel::CatalogFile kModelE4B{
    "Gemma 4 E4B (Google QAT, Q4_0)", "gemma-4-E4B_q4_0-it.gguf",
    "https://huggingface.co/google/gemma-4-E4B-it-qat-q4_0-gguf/resolve/4b4a2c1d584be7264f87aac328a1bc739ce81b6c/"
    "gemma-4-E4B_q4_0-it.gguf",
    "676c35070db6dbe52f93e9c864ee0fba4eddea94b9c875d9cb10daff453fbaee", 5154941280ull,
    "google/gemma-4-E4B-it-qat-q4_0-gguf"};
const EngineLabViewModel::CatalogFile kProjectorE4B{
    "Eyes and ears for E4B (Google, vision and audio)", "gemma-4-E4B-it-mmproj.gguf",
    "https://huggingface.co/google/gemma-4-E4B-it-qat-q4_0-gguf/resolve/4b4a2c1d584be7264f87aac328a1bc739ce81b6c/"
    "gemma-4-E4B-it-mmproj.gguf",
    "7498a37cb619e55f2fcf87eb931f56e99389ed6d432e4c5c66110694c0d65578", 991552256ull,
    "google/gemma-4-E4B-it-qat-q4_0-gguf"};
const EngineLabViewModel::CatalogFile kModel12B{
    "Gemma 4 12B (Google QAT, Q4_0)", "gemma-4-12b-it-qat-q4_0.gguf",
    "https://huggingface.co/google/gemma-4-12B-it-qat-q4_0-gguf/resolve/29d097773436b69ff9feafd636ab4cf873786537/"
    "gemma-4-12b-it-qat-q4_0.gguf",
    "93567e57a8fe10b23569b9d9ec38cd005deedf71e29477c421a4b83f418a538b", 6975879296ull,
    "google/gemma-4-12B-it-qat-q4_0-gguf"};
const EngineLabViewModel::CatalogFile kProjector12B{
    "Eyes for 12B (Google)", "mmproj-gemma-4-12b-it-qat-q4_0.gguf",
    "https://huggingface.co/google/gemma-4-12B-it-qat-q4_0-gguf/resolve/29d097773436b69ff9feafd636ab4cf873786537/"
    "mmproj-gemma-4-12b-it-qat-q4_0.gguf",
    "cb018338a7538a9814d994bfe54644c71eb7ed54e31eae2f721e45fd3c260da7", 175115616ull,
    "google/gemma-4-12B-it-qat-q4_0-gguf"};

// Their MTP assistants: Google publishes them as unquantized QAT weights only, so these are GGUF
// conversions of Google's own checkpoints (as E2B's drafter is), pinned the same way.
const EngineLabViewModel::CatalogFile kDrafterE4B{
    "MTP drafter for E4B (Google QAT assistant, Q4_0)", "gemma-4-E4B-it-qat-assistant-q4_0.gguf",
    "https://huggingface.co/cascade-tech/gemma-4-E4B-it-qat-q4_0-unquantized-assistant-gguf/resolve/"
    "3fd2bebe459bbbfc4161c32e91cf4cef4b568b93/gemma-4-E4B-it-qat-assistant-q4_0.gguf",
    "866800a8c56e77cc788340e47d79c03e4368451467d757985666dcdbf8c06e90", 76978624ull,
    "community conversion (cascade-tech) of google/gemma-4-E4B-it-qat-q4_0-unquantized-assistant"};
const EngineLabViewModel::CatalogFile kDrafter12B{
    "MTP drafter for 12B (Google QAT assistant, Q4_0)", "gemma-4-12B-it-qat-q4_0-unquantized-assistant-q4_0.gguf",
    "https://huggingface.co/Disya/gemma-4-12B-it-qat-q4_0-unquantized-assistant-gguf/resolve/"
    "f9a535af9adbb8a2d8ab9c94ca683696c346595e/gemma-4-12B-it-qat-q4_0-unquantized-assistant-q4_0.gguf",
    "b0e6a56d3078fb76e7ad950e85e478091a8ae1925fae8489ba4e58908068f0f3", 253707904ull,
    "community conversion (Disya) of google/gemma-4-12B-it-qat-q4_0-unquantized-assistant"};

// Another family: Qwen 3.5 9B (Alibaba, Apache-2.0), quantized by Unsloth from Qwen's own weights,
// with its vision projector from the same commit. It sees pictures but has no ears (no audio), and
// no drafter here.
const EngineLabViewModel::CatalogFile kModelQwen9B{
    "Qwen 3.5 9B (Unsloth, Q4_K_M)", "Qwen3.5-9B-Q4_K_M.gguf",
    "https://huggingface.co/unsloth/Qwen3.5-9B-GGUF/resolve/3885219b6810b007914f3a7950a8d1b469d598a5/"
    "Qwen3.5-9B-Q4_K_M.gguf",
    "03b74727a860a56338e042c4420bb3f04b2fec5734175f4cb9fa853daf52b7e8", 5680522464ull,
    "unsloth/Qwen3.5-9B-GGUF (of Qwen/Qwen3.5-9B)"};
const EngineLabViewModel::CatalogFile kProjectorQwen9B{
    "Eyes for Qwen 3.5 9B (vision only)", "Qwen3.5-9B-mmproj-F16.gguf",
    "https://huggingface.co/unsloth/Qwen3.5-9B-GGUF/resolve/3885219b6810b007914f3a7950a8d1b469d598a5/mmproj-F16.gguf",
    "f70dc3509053962b0d0d3ee8a7eacebf5d60aa560cad78254ae8698516ae029f", 918166080ull,
    "unsloth/Qwen3.5-9B-GGUF (of Qwen/Qwen3.5-9B)"};

// Qwen 3.5's ears: Qwen3-ASR 0.6B (Alibaba, Apache-2.0), a speech model on Qwen3-Omni's audio
// encoder (30 languages and 22 Chinese dialects), converted by ggml-org; Q8_0, pinned the same way.
const EngineLabViewModel::CatalogFile kEarsQwen{
    "Ears for Qwen (Qwen3-ASR 0.6B, Q8_0)", "Qwen3-ASR-0.6B-Q8_0.gguf",
    "https://huggingface.co/ggml-org/Qwen3-ASR-0.6B-GGUF/resolve/b98bf4381df3b400b929dcaff0dc83b6dafe221b/"
    "Qwen3-ASR-0.6B-Q8_0.gguf",
    "05c9e10cecb37f6fc0cac0e786195dba82f12c15021f0b8a09473dbf3b6f090d", 804749536ull,
    "ggml-org/Qwen3-ASR-0.6B-GGUF (of Qwen/Qwen3-ASR-0.6B)"};
const EngineLabViewModel::CatalogFile kEarsProjectorQwen{
    "Audio encoder for Qwen3-ASR 0.6B (Q8_0)", "mmproj-Qwen3-ASR-0.6B-Q8_0.gguf",
    "https://huggingface.co/ggml-org/Qwen3-ASR-0.6B-GGUF/resolve/b98bf4381df3b400b929dcaff0dc83b6dafe221b/"
    "mmproj-Qwen3-ASR-0.6B-Q8_0.gguf",
    "3b5e371a03e7d14bc1b5a66f38ff3c5eec3a99b3f96deb72b3814da9adafb16d", 214392544ull,
    "ggml-org/Qwen3-ASR-0.6B-GGUF (of Qwen/Qwen3-ASR-0.6B)"};

const EngineLabViewModel::ModelChoice kChoices[EngineLabViewModel::kModelChoices] = {
    {"Gemma 4 E2B", "the fastest: quick questions, phones and computers without a graphics card", &kModel, &kProjector, &kDrafter, true,
     5ull << 30},
    {"Gemma 4 E4B", "better at questions with several parts; about half as fast; best with a graphics card of 8 GB",
     &kModelE4B, &kProjectorE4B, &kDrafterE4B, true, 7ull << 30},
    {"Gemma 4 12B", "the most careful; slow, and needs a graphics card of 12 GB or more to be quick", &kModel12B,
     &kProjector12B, &kDrafter12B, false, 9ull << 30},
    {"Qwen 3.5 9B", "another family (Alibaba): careful with tables and documents; hears speech through Qwen3-ASR; needs a "
     "graphics card of 8 GB",
     &kModelQwen9B, &kProjectorQwen9B, nullptr, false, 8ull << 30, false, 32 * 32, &kEarsQwen, &kEarsProjectorQwen},
};

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
const EngineLabViewModel::CatalogFile& EngineLabViewModel::projectorFile() { return kProjector; }
const EngineLabViewModel::CatalogFile& EngineLabViewModel::catalogFile(Which which)
{
    return which == Which::Model ? kModel : which == Which::Drafter ? kDrafter : kProjector;
}

const EngineLabViewModel::ModelChoice& EngineLabViewModel::modelChoiceInfo(int index)
{
    return kChoices[std::clamp(index, 0, kModelChoices - 1)];
}

const EngineLabViewModel::CatalogFile& EngineLabViewModel::file(Which which) const
{
    // A model with no drafter (Qwen): a file of no name, never downloaded or used.
    static const CatalogFile none{"Not for this model", "", "", "", 0, ""};
    const ModelChoice& c = chosenModel();
    switch (which) {
    case Which::Model: return *c.model;
    case Which::Projector: return *c.projector;
    case Which::Drafter: return c.drafter ? *c.drafter : none;
    case Which::Ears: return c.ears ? *c.ears : none;
    case Which::EarsProjector: return c.earsProjector ? *c.earsProjector : none;
    }
    return none;
}

Property<std::string>& EngineLabViewModel::customPath(Which which)
{
    switch (which) {
    case Which::Model: return customModelPath;
    case Which::Drafter: return customDrafterPath;
    case Which::Ears: return customEarsPath;
    case Which::EarsProjector: return customEarsProjectorPath;
    case Which::Projector: break;
    }
    return customProjectorPath;
}

EngineLabViewModel::EngineLabViewModel(interfaces::ILocalEngineService* engine) : engine_(engine)
{
    // The choices are remembered.
    for (Property<std::string>* p : {&customModelPath, &customDrafterPath, &customProjectorPath})
        subscriptions_.push_back(p->subscribe([this](const std::string&) { changed.emit(); }));
    for (Property<bool>* p : {&useGpu, &allowMobileData})
        subscriptions_.push_back(p->subscribe([this](const bool&) { changed.emit(); }));
    subscriptions_.push_back(threads.subscribe([this](const int&) { changed.emit(); }));
    subscriptions_.push_back(modelChoice.subscribe([this](const int&) { changed.emit(); }));
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
    } else if (engine_ && (acceleratorStale_ || info_.accelerator.find(": starting") != std::string::npos)) {
        // The engine library is looked at in the background, and may stop working: asked again
        // while it starts, and after each run.
        acceleratorStale_ = false;
        interfaces::EngineInfo fresh = engine_->info();
        if (fresh.accelerator != info_.accelerator) {
            info_.accelerator = std::move(fresh.accelerator);
            info_.acceleratorReady = fresh.acceleratorReady;
            info_.devices = std::move(fresh.devices);
            changed.emit();
        }
    }
    return info_;
}

std::string EngineLabViewModel::pathFor(Which which) const
{
    const std::string& custom = const_cast<EngineLabViewModel*>(this)->customPath(which).get();
    if (!custom.empty())
        return custom;
    if (!engine_)
        return {};
    const std::string dir = engine_->modelsDirectory();
    const std::string name = file(which).fileName;
    return dir.empty() || name.empty() ? std::string() : dir + "/" + name;
}

EngineLabViewModel::FileStatus EngineLabViewModel::fileStatus(Which which) const
{
    FileStatus s;
    const CatalogFile& file = this->file(which);
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
    const bool custom = !(const_cast<EngineLabViewModel*>(this)->customPath(which).get()).empty();
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
    if (!engine_ || downloading_ >= 0 || !*file(which).url)
        return;
    if (engine_->meteredNetwork() && !allowMobileData.get()) {
        confirm_ = int(which); // the view asks first
        return;
    }
    startDownload(which, allowMobileData.get());
}

void EngineLabViewModel::confirmMobileDownload()
{
    if (confirm_ < 0)
        return;
    const Which which = Which(confirm_);
    confirm_ = -1;
    if (downloading_ == int(which)) // moving a download that waits for Wi-Fi: start it again
        cancelDownload();
    startDownload(which, true);
}

void EngineLabViewModel::importFile(Which which)
{
    if (!engine_ || downloading_ >= 0)
        return;
    const CatalogFile& file = this->file(which);
    const std::string dir = engine_->modelsDirectory();
    if (dir.empty()) {
        say("No folder to keep models in on this device.", true);
        return;
    }
    const std::string path = dir + "/" + file.fileName;
    const uint64_t free = engine_->freeSpace(path);
    if (free && free < file.size + kSpareBytes) {
        say("Not enough space: " + gigabytes(file.size) + " needed plus " + gigabytes(kSpareBytes) + " spare, " + gigabytes(free) + " free.", true);
        return;
    }
    std::string error;
    if (!engine_->importFile(path, &error)) {
        say("Import failed to start: " + error, true);
        downloadErrors_[int(which)] = "could not import: " + error;
        return;
    }
    customPath(which).set({});
    mismatched_.erase(std::remove(mismatched_.begin(), mismatched_.end(), path), mismatched_.end());
    downloadErrors_[int(which)].clear();
    downloading_ = int(which);
    importing_ = true;
    lastDownload_ = {};
    lastDownload_.state = DownloadStatus::State::Running;
    say(std::string("Choose ") + file.fileName + " (e.g. in Downloads); it is copied into the app.");
}

void EngineLabViewModel::useMobileData()
{
    if (downloading_ >= 0)
        confirm_ = downloading_;
}

bool EngineLabViewModel::waitingForWifi() const
{
    return downloading_ >= 0 && lastDownload_.error.find("Wi-Fi") != std::string::npos;
}

void EngineLabViewModel::startDownload(Which which, bool mobileData)
{
    if (!engine_ || downloading_ >= 0)
        return;
    const CatalogFile& file = this->file(which);
    // Always into the models folder: a custom path is the user's own file.
    const std::string dir = engine_->modelsDirectory();
    if (dir.empty()) {
        say("No folder to keep models in on this device.", true);
        return;
    }
    const std::string path = dir + "/" + file.fileName;
    const uint64_t have = engine_->fileSize(path);
    const uint64_t need = file.size > have ? file.size - have : 0;
    const uint64_t free = engine_->freeSpace(path);
    if (free && free < need + kSpareBytes) {
        say("Not enough space: " + gigabytes(need) + " needed plus " + gigabytes(kSpareBytes) +
                           " spare, " + gigabytes(free) + " free.", true);
        downloadErrors_[int(which)] = "not enough space: " + gigabytes(free) + " free";
        return;
    }
    customPath(which).set({});
    mismatched_.erase(std::remove(mismatched_.begin(), mismatched_.end(), path), mismatched_.end());
    std::string error;
    if (!engine_->startDownload(file.url, path, mobileData, &error)) {
        say("Download failed to start: " + error, true);
        downloadErrors_[int(which)] = "could not start: " + error;
        return;
    }
    downloadErrors_[int(which)].clear();
    downloading_ = int(which);
    importing_ = false;
    lastDownload_ = {};
    lastDownload_.state = DownloadStatus::State::Running;
    lastDownload_.received = have;
    say("Downloading " + std::string(file.label) + "...");
}

void EngineLabViewModel::say(const std::string& text, bool error)
{
    notice_ = text;
    noticeError_ = error;
    statusMessage.emit(text);
}

const std::string& EngineLabViewModel::downloadError(Which which) const { return downloadErrors_[int(which)]; }

void EngineLabViewModel::cancelDownload()
{
    if (!engine_ || downloading_ < 0)
        return;
    engine_->cancelDownload();
    downloading_ = -1;
    say("Download stopped. Downloading again resumes it where possible.");
}

void EngineLabViewModel::verify(Which which)
{
    if (!engine_ || verifying_ >= 0)
        return;
    const std::string path = pathFor(which);
    const CatalogFile& file = this->file(which);
    verifying_ = int(which);
    verifyFraction_ = 0;
    engine_->hashFile(
        path, [this](double fraction) { verifyFraction_ = fraction; },
        [this, which, path, expected = std::string(file.sha256)](const std::string& sha) {
            verifying_ = -1;
            verified_.erase(std::remove_if(verified_.begin(), verified_.end(), [&](const auto& v) { return v.first == path; }),
                            verified_.end());
            mismatched_.erase(std::remove(mismatched_.begin(), mismatched_.end(), path), mismatched_.end());
            if (sha == expected) {
                verified_.emplace_back(path, sha);
                downloadErrors_[int(which)].clear();
                say("Checksum matches.");
                // The drafter comes with the model, unless it is turned off (or already here).
                if (which == Which::Model && path == pathFor(Which::Model))
                    fetchDrafter();
                // The ears' audio encoder comes with them.
                if (which == Which::Ears && path == pathFor(Which::Ears))
                    fetchEarsProjector();
            } else {
                mismatched_.push_back(path);
                say(sha.empty() ? "The file could not be read." : "Checksum does not match: download it again.", true);
                downloadErrors_[int(which)] = sha.empty() ? "could not be read" : "checksum does not match";
            }
            changed.emit();
        });
}

bool EngineLabViewModel::earsUsable() const
{
    return chosenModel().ears && fileUsable(Which::Ears) && fileUsable(Which::EarsProjector);
}

void EngineLabViewModel::fetchEarsProjector()
{
    if (chosenModel().earsProjector && customEarsProjectorPath.get().empty() && fileUsable(Which::Ears) &&
        downloadErrors_[int(Which::EarsProjector)].empty() && fileStatus(Which::EarsProjector).state == FileState::Missing &&
        downloading_ < 0 && verifying_ < 0 && confirm_ < 0)
        download(Which::EarsProjector);
}

bool EngineLabViewModel::drafterOn() const
{
    if (!chosenModel().drafter)
        return false;
    const int setting = drafterSetting_[std::clamp(modelChoice.get(), 0, kModelChoices - 1)];
    return setting < 0 ? chosenModel().drafterByDefault : setting == 1;
}

void EngineLabViewModel::setDrafterOn(bool on)
{
    int& setting = drafterSetting_[std::clamp(modelChoice.get(), 0, kModelChoices - 1)];
    setting = on == chosenModel().drafterByDefault ? -1 : int(on); // back to the default: follows it
    changed.emit();
    fetchDrafter(); // on: fetched if it is not here
}

void EngineLabViewModel::fetchDrafter()
{
    if (drafterOn() && chosenModel().drafter && customDrafterPath.get().empty() && fileUsable(Which::Model) &&
        downloadErrors_[int(Which::Drafter)].empty() && // a failed one waits for the button, not a retry each frame
        fileStatus(Which::Drafter).state == FileState::Missing && downloading_ < 0 && verifying_ < 0 && confirm_ < 0)
        download(Which::Drafter);
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
        say(importing_ ? "Copied; checking the checksum..." : "Downloaded; checking the checksum...");
        verify(which);
    } else if (lastDownload_.state == DownloadStatus::State::Failed) {
        say((importing_ ? "Import failed: " : "Download failed: ") + lastDownload_.error, true);
        downloadErrors_[int(which)] = lastDownload_.error.empty() ? std::string("failed") : lastDownload_.error;
    } else {
        // Stopped without finishing (cancelled, or the app was restarted meanwhile).
        say(importing_ ? "The import stopped." : "The download stopped. Download again to resume it.", true);
        downloadErrors_[int(which)] = "stopped before it finished";
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
        say("Download the model first.", true);
        return;
    }
    if (withDrafter && !fileUsable(Which::Drafter)) {
        say("Download the drafter first.", true);
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
            acceleratorStale_ = true;
            history_.insert(history_.begin(), Record{nowText(), deviceLabel(), settings, result});
            if (history_.size() > 100)
                history_.resize(100);
            changed.emit();
            if (!result.ok) {
                say("Run failed: " + result.error, true);
                return;
            }
            char buf[96];
            std::snprintf(buf, sizeof(buf), "%.1f tokens/s generating, %.0f tokens/s reading the prompt",
                          result.generateTokensPerSecond, result.promptTokensPerSecond);
            say(buf);
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
        .set("model", modelChoice.get())
        .set("drafters", [this]() {
            model::Json d = model::Json::array();
            for (int setting : drafterSetting_)
                d.push(setting);
            return d;
        }())
        .set("customModel", customModelPath.get())
        .set("customDrafter", customDrafterPath.get())
        .set("customProjector", customProjectorPath.get())
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
    if (const model::Json* m = j.find("model"); m && m->isNumber())
        modelChoice.set(std::clamp(int(m->asNumber()), 0, kModelChoices - 1));
    if (const model::Json* d = j.find("drafters"); d && d->isArray())
        for (size_t i = 0; i < d->items().size() && i < size_t(kModelChoices); ++i)
            drafterSetting_[i] = std::clamp(int(d->items()[i].asNumber(-1)), -1, 1);
    customModelPath.set(j.getString("customModel"));
    customDrafterPath.set(j.getString("customDrafter"));
    customProjectorPath.set(j.getString("customProjector"));
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
