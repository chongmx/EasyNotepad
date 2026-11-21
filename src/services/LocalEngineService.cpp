#include "enp/services/LocalEngineService.h"

#include "enp/model/Hash.h"
#include "enp/os/Dispatch.h"
#include "enp/os/Memory.h"
#include "enp/os/Download.h"
#include "enp/os/Fs.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#include <sys/sysctl.h>
#endif
#if defined(__ANDROID__)
#include <sys/auxv.h>
#include <sys/system_properties.h>
#endif
#if defined(__x86_64__) || defined(_M_X64)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif
#endif

#include <csignal>
#include <cstdlib>
#if defined(__linux__) && !defined(__ANDROID__)
#include <sys/prctl.h>
#endif

#include "enp/engine/EngineJson.h"
#include "enp/engine/EnpEngine.h"
#include "enp/os/Library.h"
#include "enp/services/EngineHelper.h"
#if ENP_HAS_LOCAL_ENGINE
#include "enp/engine/LlamaRunner.h"
#endif

#include <condition_variable>
#include <deque>
#include <iostream>

namespace enp::services {

using interfaces::DownloadStatus;
using interfaces::EngineDevice;
using interfaces::EngineInfo;
using interfaces::EngineRunResult;
using interfaces::EngineRunSettings;

namespace {

// ---- What the device is ------------------------------------------------------------------------

#if !defined(_WIN32) && !defined(__APPLE__) // Linux and Android read /proc
std::string readText(const char* path)
{
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// "key : value" lines (/proc/cpuinfo, /proc/meminfo): the first value for `key`.
std::string procField(const std::string& text, const std::string& key)
{
    std::istringstream lines(text);
    for (std::string line; std::getline(lines, line);) {
        if (line.compare(0, key.size(), key) != 0)
            continue;
        const size_t colon = line.find(':');
        if (colon == std::string::npos)
            continue;
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        return value;
    }
    return {};
}
#endif

#if defined(__ANDROID__)
std::string property(const char* name)
{
    char value[PROP_VALUE_MAX] = {};
    __system_property_get(name, value);
    return value;
}
#endif

std::string cpuName()
{
#if defined(_WIN32)
    char name[128] = {};
    DWORD size = sizeof(name);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, name, &size) == ERROR_SUCCESS)
        return name;
    return {};
#elif defined(__APPLE__)
    char name[128] = {};
    size_t size = sizeof(name);
    if (sysctlbyname("machdep.cpu.brand_string", name, &size, nullptr, 0) == 0)
        return name;
    return {};
#else
    const std::string info = readText("/proc/cpuinfo");
    for (const char* key : {"model name", "Hardware", "Processor"})
        if (std::string v = procField(info, key); !v.empty())
            return v;
    return {};
#endif
}

void memoryInfo(uint64_t& total, uint64_t& available)
{
#if defined(_WIN32)
    MEMORYSTATUSEX m{};
    m.dwLength = sizeof(m);
    if (GlobalMemoryStatusEx(&m)) {
        total = m.ullTotalPhys;
        available = m.ullAvailPhys;
    }
#elif defined(__APPLE__)
    uint64_t size = 0;
    size_t len = sizeof(size);
    if (sysctlbyname("hw.memsize", &size, &len, nullptr, 0) == 0)
        total = size;
#else
    const std::string info = readText("/proc/meminfo");
    total = std::strtoull(procField(info, "MemTotal").c_str(), nullptr, 10) * 1024;
    available = std::strtoull(procField(info, "MemAvailable").c_str(), nullptr, 10) * 1024;
#endif
}

// The process's peak resident memory so far (bytes), or 0.
uint64_t peakMemory()
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
        return pmc.PeakWorkingSetSize;
    return 0;
#elif defined(__APPLE__)
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return uint64_t(usage.ru_maxrss);
#else
    return std::strtoull(procField(readText("/proc/self/status"), "VmHWM").c_str(), nullptr, 10) * 1024;
#endif
}

// Starts counting the peak again (Linux and Android allow it; elsewhere it is the process's peak).
void resetPeakMemory()
{
#if defined(__linux__)
    if (FILE* f = std::fopen("/proc/self/clear_refs", "w")) {
        std::fputs("5", f);
        std::fclose(f);
    }
#endif
}

// The engine is built for one CPU level (see CMakeLists.txt): AVX2 + FMA + F16C on x86-64,
// dot product + fp16 arithmetic on Android ARM64. Running it on less would crash the app.
bool cpuSupported(std::string& features, std::string& missing)
{
#if defined(__x86_64__) || defined(_M_X64)
    unsigned r1[4] = {}, r7[4] = {};
#if defined(_MSC_VER)
    int a[4];
    __cpuid(a, 1);
    std::memcpy(r1, a, sizeof(r1));
    __cpuidex(a, 7, 0);
    std::memcpy(r7, a, sizeof(r7));
#else
    __get_cpuid(1, &r1[0], &r1[1], &r1[2], &r1[3]);
    __get_cpuid_count(7, 0, &r7[0], &r7[1], &r7[2], &r7[3]);
#endif
    bool osAvx = false;
    if (r1[2] & (1u << 27)) { // OSXSAVE: the OS saves the AVX registers
#if defined(_MSC_VER)
        osAvx = (_xgetbv(0) & 6) == 6;
#else
        unsigned lo = 0, hi = 0;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        osAvx = (lo & 6) == 6;
#endif
    }
    const bool fma = r1[2] & (1u << 12), f16c = r1[2] & (1u << 29);
    const bool avx2 = osAvx && (r7[1] & (1u << 5)), bmi2 = r7[1] & (1u << 8);
    const bool avx512 = osAvx && (r7[1] & (1u << 16)), vnni = r7[2] & (1u << 11);
    for (auto [on, name] : {std::pair{avx2, "AVX2"}, {fma, "FMA"}, {f16c, "F16C"}, {bmi2, "BMI2"}, {avx512, "AVX-512"},
                            {vnni, "VNNI"}})
        if (on)
            features += features.empty() ? name : std::string(" ") + name;
    if (!avx2 || !fma || !f16c || !bmi2)
        missing = "AVX2, FMA, F16C and BMI2 (2013 or newer processors)";
    return missing.empty();
#elif defined(__ANDROID__) && defined(__aarch64__)
    const unsigned long hw = getauxval(AT_HWCAP), hw2 = getauxval(AT_HWCAP2);
    const bool dot = hw & (1ul << 20), fp16 = hw & (1ul << 10), sve = hw & (1ul << 22);
    const bool i8mm = hw2 & (1ul << 13);
    for (auto [on, name] : {std::pair{dot, "dotprod"}, {fp16, "fp16"}, {i8mm, "i8mm"}, {sve, "SVE"}})
        if (on)
            features += features.empty() ? name : std::string(" ") + name;
    if (!dot || !fp16)
        missing = "ARMv8.2 dot product and fp16 (most phones from 2019 on)";
    return missing.empty();
#else
    features = "NEON";
    return true;
#endif
}

// The faster engine library looked for next to the app (plan 21). Desktop builds with an engine.
constexpr const char* kAcceleratorBase = "enp-engine-cuda";

// ---- The helper process (`--local-engine <library>`) --------------------------------------------

#if !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
// stdio that also works for a Windows GUI-subsystem executable started with redirected handles.
bool readInputLine(std::string& line)
{
#if defined(_WIN32)
    static std::string buffer;
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    for (;;) {
        const size_t newline = buffer.find('\n');
        if (newline != std::string::npos) {
            line = buffer.substr(0, newline);
            if (!line.empty() && line.back() == '\r')
                line.pop_back();
            buffer.erase(0, newline + 1);
            return true;
        }
        char chunk[4096];
        DWORD read = 0;
        if (!in || in == INVALID_HANDLE_VALUE || !ReadFile(in, chunk, sizeof(chunk), &read, nullptr) || read == 0)
            return false;
        buffer.append(chunk, read);
    }
#else
    return bool(std::getline(std::cin, line));
#endif
}

void writeOutputLine(const std::string& text)
{
    const std::string data = text + "\n";
#if defined(_WIN32)
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written = 0;
    if (out && out != INVALID_HANDLE_VALUE)
        WriteFile(out, data.data(), DWORD(data.size()), &written, nullptr);
#else
    std::fwrite(data.data(), 1, data.size(), stdout);
    std::fflush(stdout);
#endif
}
#endif

} // namespace

int runLocalEngineHelper(const std::string& library)
{
#if defined(__ANDROID__) || defined(__EMSCRIPTEN__)
    (void)library;
    return 2; // no helper processes there
#else
#if defined(__linux__) && !defined(__ANDROID__)
    prctl(PR_SET_PDEATHSIG, SIGKILL); // the app gone, the helper goes too
#endif
#if defined(_WIN32)
    // A crash or abort ends the helper at once: no error dialog (it would wait, unseen, for a
    // click) and no fault report holding the process, so the app falls back to the CPU at once.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
#if defined(_MSC_VER) || defined(_UCRT) // (MinGW's msvcrt has no such call; the SIGABRT handler does it)
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
#endif
    std::signal(SIGABRT, [](int) { std::_Exit(3); });
    using model::Json;
    auto refuse = [](const std::string& why) {
        writeOutputLine(Json::object().set("event", "ready").set("ok", false).set("error", why).dump());
        return 2;
    };
    std::string error;
    void* lib = os::lib::open(library, &error);
    if (!lib)
        return refuse("the engine library could not be loaded: " + error);
    const auto abi = reinterpret_cast<enp_engine_abi_fn>(os::lib::symbol(lib, "enp_engine_abi"));
    const auto describe = reinterpret_cast<enp_engine_describe_fn>(os::lib::symbol(lib, "enp_engine_describe"));
    const auto create = reinterpret_cast<enp_engine_create_fn>(os::lib::symbol(lib, "enp_engine_create"));
    const auto request = reinterpret_cast<enp_engine_request_fn>(os::lib::symbol(lib, "enp_engine_request"));
    const auto unload = reinterpret_cast<enp_engine_unload_fn>(os::lib::symbol(lib, "enp_engine_unload"));
    const auto destroy = reinterpret_cast<enp_engine_destroy_fn>(os::lib::symbol(lib, "enp_engine_destroy"));
    if (!abi || !describe || !create || !request || !unload || !destroy)
        return refuse("not an engine library (an enp_engine function is missing)");
    if (abi() != ENP_ENGINE_ABI)
        return refuse("the engine library is for another version of EasyNotepad (interface " + std::to_string(abi()) +
                      ", this one needs " + std::to_string(ENP_ENGINE_ABI) + ")");
    const char* text = describe();
    std::optional<Json> description = Json::parse(text ? text : "");
    if (!description || !description->isObject())
        return refuse("the engine library did not say what it can use");
    if (const std::string why = description->getString("error"); !why.empty())
        return refuse(why);
    enp_engine* engine = create();
    if (!engine)
        return refuse("the engine could not start");
    writeOutputLine(Json::object().set("event", "ready").set("ok", true).set("engine", *description).dump());

    // Requests are read on their own thread, so a cancel reaches the request running.
    struct Inbox {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Json> requests;
        std::atomic<bool> cancel{false};
        bool end = false;
    } inbox;
    std::thread reader([&inbox]() {
        std::string line;
        while (readInputLine(line)) {
            std::optional<Json> j = Json::parse(line);
            if (!j || !j->isObject())
                continue;
            std::lock_guard<std::mutex> lock(inbox.mutex);
            if (j->getString("op") == "cancel") {
                inbox.cancel = true;
                continue;
            }
            inbox.cancel = false; // a new request: a cancel from now on is for it
            inbox.requests.push_back(std::move(*j));
            inbox.ready.notify_one();
        }
        std::lock_guard<std::mutex> lock(inbox.mutex); // the app closed our input, or is gone
        inbox.end = true;
        inbox.cancel = true;
        inbox.ready.notify_one();
    });
    for (;;) {
        Json next;
        {
            std::unique_lock<std::mutex> lock(inbox.mutex);
            inbox.ready.wait(lock, [&]() { return inbox.end || !inbox.requests.empty(); });
            if (inbox.end)
                break;
            next = std::move(inbox.requests.front());
            inbox.requests.pop_front();
        }
        if (next.getString("op") == "unload") {
            unload(engine);
            continue;
        }
        resetPeakMemory();
        const char* answer = request(
            engine, next.dump().c_str(),
            [](void*, const char* kind, const char* piece) {
                writeOutputLine(Json::object().set("event", kind ? kind : "").set("text", piece ? piece : "").dump());
            },
            [](void* user) { return static_cast<Inbox*>(user)->cancel.load() ? 1 : 0; }, &inbox);
        std::optional<Json> result = Json::parse(answer ? answer : "");
        if (!result || !result->isObject())
            result = Json::object().set("ok", false).set("error", "the engine gave no result");
        if (next.getString("op") == "run")
            result->set("peakMemory", double(peakMemory()));
        writeOutputLine(Json::object().set("event", "done").set("result", std::move(*result)).dump());
    }
    destroy(engine); // the model and the device's memory given back before the process ends
    reader.join();
    return 0; // the library stays loaded: a GPU runtime is not made to be unloaded
#endif
}

// The agent's model, kept loaded between questions; and the faster engine when there is one.
struct LocalEngineService::ChatModel {
    std::mutex mutex; // held while a chat or a run uses or (un)loads the model
#if ENP_HAS_LOCAL_ENGINE
    engine::Runner runner;
#endif
    bool isLoaded() const
    {
#if ENP_HAS_LOCAL_ENGINE
        return runner.loaded();
#else
        return false;
#endif
    }
    std::chrono::steady_clock::time_point lastUse() const
    {
#if ENP_HAS_LOCAL_ENGINE
        return runner.lastUse();
#else
        return {};
#endif
    }
    void unload()
    {
#if ENP_HAS_LOCAL_ENGINE
        runner.unload();
#endif
    }

    // The engine library (enp-engine-cuda) through its helper process.
    std::unique_ptr<EngineHelper> helper;
    std::mutex acceleratorMutex; // guards what follows
    enum class Accel { None, Unknown, Probing, Ready, Failed } accel = Accel::None;
    std::string accelName = "CUDA";
    std::string accelReason;
    std::vector<EngineDevice> accelDevices;
    std::chrono::steady_clock::time_point helperLastUse;

    // Whether to try the library now (it is there and has not failed).
    bool acceleratorWanted()
    {
        if (!helper)
            return false;
        std::lock_guard<std::mutex> lock(acceleratorMutex);
        if (accel == Accel::Failed)
            return false;
        if (os::fs::stat(helper->options().library).size == 0) {
            accel = Accel::Unknown; // dropped next to the app later: tried then
            return false;
        }
        return true;
    }
    // Starts the helper; false (and the reason kept) when the library cannot be used.
    bool startAccelerator()
    {
        std::string reason;
        EngineHelper::Description d;
        const bool ok = helper->start(reason, &d);
        std::lock_guard<std::mutex> lock(acceleratorMutex);
        if (ok) {
            accel = Accel::Ready;
            if (!d.name.empty())
                accelName = d.name;
            accelDevices.clear();
            for (EngineDevice& e : d.devices)
                if (e.kind != "CPU")
                    accelDevices.push_back(std::move(e));
        } else {
            accel = Accel::Failed;
            accelReason = reason;
        }
        return ok;
    }
    void acceleratorFailed(const std::string& reason)
    {
        helper->stop();
        std::lock_guard<std::mutex> lock(acceleratorMutex);
        accel = Accel::Failed;
        accelReason = reason;
    }
};

LocalEngineService::LocalEngineService() : LocalEngineService(defaultAccelerator()) {}

LocalEngineService::LocalEngineService(std::optional<Accelerator> accelerator) : chat_(std::make_shared<ChatModel>())
{
#if ENP_HAS_LOCAL_ENGINE
    if (accelerator && !accelerator->library.empty() && !accelerator->helper.empty()) {
        EngineHelper::Options o;
        o.library = accelerator->library;
        o.command = accelerator->helper;
        o.startSeconds = accelerator->startSeconds;
        o.silenceSeconds = accelerator->silenceSeconds;
        chat_->helper = std::make_unique<EngineHelper>(std::move(o));
        chat_->accel = ChatModel::Accel::Unknown;
    }
#else
    (void)accelerator;
#endif
}

std::optional<LocalEngineService::Accelerator> LocalEngineService::defaultAccelerator()
{
#if ENP_HAS_LOCAL_ENGINE && !defined(__ANDROID__) && !defined(__EMSCRIPTEN__)
    const std::string exe = os::fs::executablePath();
    if (exe.empty())
        return std::nullopt;
    Accelerator a;
    a.library = os::fs::path::join(os::fs::path::parent(exe), os::lib::fileName(kAcceleratorBase));
    a.helper = {exe, "--local-engine", a.library};
    return a;
#else
    return std::nullopt;
#endif
}

LocalEngineService::~LocalEngineService()
{
    alive_->store(false);
    cancel_->store(true);
    if (chat_->helper)
        chat_->helper->stop();
}

EngineInfo LocalEngineService::info()
{
    if (!info_) {
        EngineInfo i;
        i.cpuName = cpuName();
        i.cpuThreads = int(std::max(1u, std::thread::hardware_concurrency()));
        memoryInfo(i.memoryTotal, i.memoryAvailable);
#if defined(__ANDROID__)
        const std::string soc = property("ro.soc.manufacturer") + " " + property("ro.soc.model");
        i.socName = soc == " " ? property("ro.board.platform") : soc;
        if (i.cpuName.empty())
            i.cpuName = property("ro.product.model");
#endif
        std::string missing;
        const bool cpuOk = cpuSupported(i.cpuFeatures, missing);
#if ENP_HAS_LOCAL_ENGINE
        i.engineVersion = engine::version();
        if (!cpuOk) {
            i.unavailableReason = "This processor lacks " + missing + ", which this build of the engine needs.";
            if (chat_->helper) {
                std::lock_guard<std::mutex> lock(chat_->acceleratorMutex);
                chat_->accel = ChatModel::Accel::None; // its CPU part needs the same
            }
        } else {
            {
                os::mem::Scope scope("local-engine"); // llama.cpp's log ring and backend registry live for the process
                engine::initBackend();
            }
            i.available = true;
            i.devices = engine::devices();
        }
#else
        (void)cpuOk;
        i.unavailableReason = "This build has no inference engine (32-bit and web builds leave it out).";
#endif
        info_ = i;
    }
    EngineInfo i = *info_;
    if (!chat_->helper)
        return i;
    // The engine library: looked at once in the background (its helper started, asked, ended).
    const bool there = os::fs::stat(chat_->helper->options().library).size > 0;
    std::lock_guard<std::mutex> lock(chat_->acceleratorMutex);
    using A = ChatModel::Accel;
    const std::string& name = chat_->accelName;
    switch (chat_->accel) {
    case A::None: break;
    case A::Unknown:
        if (!there) {
            i.accelerator = name + ": not installed (" + os::lib::fileName(kAcceleratorBase) + " is not next to EasyNotepad)";
            break;
        }
        chat_->accel = A::Probing;
        {
            auto model = chat_;
            os::dispatch::runAsync([model]() {
                std::lock_guard<std::mutex> busy(model->mutex);
                if (model->startAccelerator())
                    model->helper->stop(); // known now; started again by the first question
            });
        }
        [[fallthrough]];
    case A::Probing: i.accelerator = name + ": starting"; break;
    case A::Ready: {
        std::string devices;
        for (const EngineDevice& d : chat_->accelDevices) {
            char memory[32];
            std::snprintf(memory, sizeof(memory), " (%.1f GB)", double(d.memoryTotal) / (1024.0 * 1024.0 * 1024.0));
            devices += (devices.empty() ? "" : ", ") + d.description + (d.memoryTotal ? memory : "");
        }
        i.accelerator = name + ": " + (devices.empty() ? "ready" : devices);
        i.acceleratorReady = true;
        for (const EngineDevice& d : chat_->accelDevices)
            i.devices.push_back(d);
        break;
    }
    case A::Failed: i.accelerator = name + ": not used (" + chat_->accelReason + "); the CPU answers"; break;
    }
    return i;
}

void LocalEngineService::run(const EngineRunSettings& settings, std::function<void(const std::string& stage)> progress,
                             std::function<void(const EngineRunResult& result)> done)
{
    if (running_->exchange(true)) {
        EngineRunResult r;
        r.error = "a run is already in progress";
        done(r);
        return;
    }
    cancel_->store(false);
    auto alive = alive_;
    auto cancel = cancel_;
    auto running = running_;
    auto report = [alive, progress](std::string stage) {
        os::dispatch::postToMain([alive, progress, stage = std::move(stage)]() {
            if (alive->load() && progress)
                progress(stage);
        });
    };
    auto finish = [alive, running, done](EngineRunResult result) {
        os::dispatch::postToMain([alive, running, done, result = std::move(result)]() {
            running->store(false);
            if (alive->load() && done)
                done(result);
        });
    };
#if !ENP_HAS_LOCAL_ENGINE
    (void)settings;
    (void)report;
    (void)cancel;
    EngineRunResult r;
    r.error = info().unavailableReason;
    finish(r);
#else
    if (!info().available) {
        EngineRunResult r;
        r.error = info().unavailableReason;
        finish(r);
        return;
    }
    // One model in memory at a time: the agent's, if loaded, makes room (a phone has little spare).
    if (chatting_->load()) {
        EngineRunResult r;
        r.error = "the local agent is answering; try again when it is done";
        finish(r);
        return;
    }
    auto model = chat_;
    os::dispatch::runAsync([settings, report, finish, cancel, model]() {
        std::lock_guard<std::mutex> lock(model->mutex);
        model->unload();
        const engine::Cancelled cancelled = [cancel]() { return cancel->load(); };
        std::string gpuFailed;
        // On the GPU: through the engine library, when there is one that works.
        if (settings.useGpu && model->acceleratorWanted()) {
            model::Json answer;
            std::string reason;
            if (model->startAccelerator()) {
                if (model->helper->request(engine::toJson(settings), report, nullptr, cancelled, answer, reason)) {
                    model->helper->stop(); // a run unloads after measuring, like the CPU's
                    EngineRunResult r = engine::runResultFrom(answer);
                    if (r.ok || cancel->load()) {
                        finish(r);
                        return;
                    }
                    reason = r.error;
                }
                model->acceleratorFailed(reason);
            }
            std::lock_guard<std::mutex> a(model->acceleratorMutex);
            gpuFailed = model->accelName + " failed: " + model->accelReason;
        }
        resetPeakMemory();
        EngineRunSettings cpu = settings;
        if (!gpuFailed.empty())
            cpu.useGpu = false;
        EngineRunResult r = model->runner.run(cpu, report, cancelled);
        r.peakMemory = peakMemory();
        if (!gpuFailed.empty())
            r.backend += " (" + gpuFailed + ")";
        finish(r);
    });
#endif
}

void LocalEngineService::chat(const interfaces::LocalChatRequest& request, std::function<void(const std::string& stage)> stage,
                              std::function<void(const std::string& text)> text,
                              std::function<void(const interfaces::LocalChatResult& result)> done)
{
    auto alive = alive_;
    auto chatting = chatting_;
    auto finish = [this, alive, chatting, done](interfaces::LocalChatResult result) {
        os::dispatch::postToMain([this, alive, chatting, done, result = std::move(result)]() {
            chatting->store(false);
            if (!alive->load()) // the service is gone (cleared on the main thread, as this runs)
                return;
            if (done)
                done(result);
            startWaiting();
        });
    };
#if !ENP_HAS_LOCAL_ENGINE
    (void)request;
    (void)stage;
    (void)text;
    chatting_->store(true);
    interfaces::LocalChatResult r;
    r.error = info().unavailableReason;
    finish(r);
#else
    // Asked while a stopped question still winds down: waits for it (the latest question only).
    if (info().available && !running_->load() && chatting_->load() && cancel_->load()) {
        dropWaiting();
        waiting_ = std::make_unique<WaitingChat>(WaitingChat{request, std::move(stage), std::move(text), std::move(done)});
        return;
    }
    // Refused at once (done is called before returning): no engine, a lab run, or still answering.
    if (!info().available || running_->load() || chatting_->exchange(true)) {
        interfaces::LocalChatResult r;
        r.error = !info().available ? info().unavailableReason
                  : running_->load() ? "the Engine Lab is running a test" : "the local agent is still answering";
        if (done)
            done(r);
        return;
    }
    cancel_->store(false);
    auto cancel = cancel_;
    auto model = chat_;
    auto post = [alive](std::function<void(const std::string&)> to, std::string what) {
        os::dispatch::postToMain([alive, to = std::move(to), what = std::move(what)]() {
            if (alive->load() && to)
                to(what);
        });
    };
    os::dispatch::runAsync([request, stage, text, finish, cancel, model, post]() {
        std::lock_guard<std::mutex> lock(model->mutex);
        const engine::Cancelled cancelled = [cancel]() { return cancel->load(); };
        const engine::Report toStage = [&](const std::string& s) { post(stage, s); };
        bool wrote = false;
        const engine::Report toText = [&](const std::string& t) {
            wrote = true;
            post(text, t);
        };
        // The engine library first, when there is one that works; anything wrong, the CPU.
        if (model->acceleratorWanted()) {
            std::string reason;
            if (model->startAccelerator()) {
                model->runner.unload(); // one copy of the model in memory
                model::Json answer;
                if (model->helper->request(engine::toJson(request), toStage, toText, cancelled, answer, reason)) {
                    model->helperLastUse = std::chrono::steady_clock::now();
                    interfaces::LocalChatResult r = engine::chatResultFrom(answer);
                    // A question too long for the context it has is not the GPU failing: said as it is
                    // (the GPU stays in use; the CPU would take many minutes for the same).
                    if (r.ok || wrote || cancel->load() || r.error.find("too long for the local model") != std::string::npos) {
                        finish(r);
                        return;
                    }
                    reason = r.error; // nothing written: the CPU answers instead
                }
                model->acceleratorFailed(reason);
                if (wrote) { // half an answer shown: say so, and the next question goes to the CPU
                    interfaces::LocalChatResult r;
                    r.error = "the " + model->accelName + " engine stopped (" + reason + "); ask again: the CPU answers now";
                    finish(r);
                    return;
                }
            }
        }
        interfaces::LocalChatResult r = model->runner.chat(request, toStage, toText, cancelled);
        if (r.ok)
            r.backend = "CPU";
        finish(r);
    });
#endif
}

void LocalEngineService::unloadIfIdle(double idleSeconds)
{
    if (chatting_->load() || running_->load())
        return;
    std::unique_lock<std::mutex> lock(chat_->mutex, std::try_to_lock);
    if (!lock.owns_lock())
        return;
    const auto now = std::chrono::steady_clock::now();
    if (chat_->isLoaded() && std::chrono::duration<double>(now - chat_->lastUse()).count() >= idleSeconds)
        chat_->unload();
    // The helper (and the GPU memory it holds) ends when idle too; the next question starts it.
    if (chat_->helper && chat_->helper->running() &&
        std::chrono::duration<double>(now - chat_->helperLastUse).count() >= idleSeconds)
        chat_->helper->stop();
}

void LocalEngineService::cancel()
{
    cancel_->store(true);
    dropWaiting();
}

void LocalEngineService::startWaiting()
{
    if (!waiting_ || chatting_->load())
        return;
    std::unique_ptr<WaitingChat> w = std::move(waiting_);
    chat(w->request, std::move(w->stage), std::move(w->text), std::move(w->done));
}

void LocalEngineService::dropWaiting()
{
    if (!waiting_)
        return;
    std::unique_ptr<WaitingChat> w = std::move(waiting_);
    if (!w->done)
        return;
    // Said on a later frame, as an answer would be (the caller may be in the middle of stopping it).
    auto alive = alive_;
    os::dispatch::postToMain([alive, done = std::move(w->done)]() {
        if (alive->load()) {
            interfaces::LocalChatResult r;
            r.error = "cancelled";
            done(r);
        }
    });
}

std::string LocalEngineService::modelsDirectory() { return os::download::directory(); }

uint64_t LocalEngineService::freeSpace(const std::string& path)
{
    std::error_code ec;
    std::filesystem::path p = std::filesystem::u8path(path);
    while (!p.empty() && !std::filesystem::exists(p, ec))
        p = p.parent_path();
    const auto space = std::filesystem::space(p, ec);
    return ec ? 0 : uint64_t(space.available);
}

uint64_t LocalEngineService::fileSize(const std::string& path) { return os::fs::stat(path).size; }

bool LocalEngineService::startDownload(const std::string& url, const std::string& path, bool allowMetered, std::string* error)
{
    return os::download::start(url, path, allowMetered, error);
}

DownloadStatus LocalEngineService::downloadStatus()
{
    const os::download::Status s = os::download::status();
    DownloadStatus d;
    d.state = DownloadStatus::State(int(s.state));
    d.received = s.received;
    d.total = s.total;
    d.error = s.error;
    return d;
}

void LocalEngineService::cancelDownload() { os::download::cancel(); }

bool LocalEngineService::meteredNetwork() { return os::download::meteredNetwork(); }

bool LocalEngineService::canImport() { return os::download::canImport(); }

bool LocalEngineService::importFile(const std::string& path, std::string* error) { return os::download::importFile(path, error); }

void LocalEngineService::hashFile(const std::string& path, std::function<void(double fraction)> progress,
                                  std::function<void(const std::string& sha256)> done)
{
    auto alive = alive_;
    os::dispatch::runAsync([alive, path, progress, done]() {
        std::ifstream in(std::filesystem::u8path(path), std::ios::binary);
        std::string digest;
        if (in) {
            const uint64_t total = os::fs::stat(path).size;
            model::Sha256 hash;
            std::vector<char> buffer(4 << 20);
            uint64_t read = 0, reported = 0;
            while (in) {
                in.read(buffer.data(), std::streamsize(buffer.size()));
                const auto n = size_t(in.gcount());
                if (n == 0)
                    break;
                hash.update(buffer.data(), n);
                read += n;
                if (read - reported >= (64u << 20) && total > 0) {
                    reported = read;
                    const double fraction = double(read) / double(total);
                    os::dispatch::postToMain([alive, progress, fraction]() {
                        if (alive->load() && progress)
                            progress(fraction);
                    });
                }
            }
            digest = hash.hexDigest();
        }
        os::dispatch::postToMain([alive, done, digest]() {
            if (alive->load() && done)
                done(digest);
        });
    });
}

} // namespace enp::services
