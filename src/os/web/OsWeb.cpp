// Web backend (Emscripten). Files live in the in-memory FS: bundled resources are preloaded
// at /assets, files picked by the user are copied to /uploads.
#include "../OsInternal.h"

#include <emscripten.h>

#include <cstdio>
#include <map>

namespace enp::os {

namespace {

constexpr const char* kAssetRoot = "/assets";
constexpr const char* kUploadRoot = "/uploads";

int g_nextDialogId = 1;
std::map<int, dialog::OpenFileCallback> g_pendingDialogs;

} // namespace

// clang-format off
EM_JS(void, enp_js_open_file_dialog, (int requestId, const char* acceptPtr, const char* uploadRootPtr), {
    const accept = UTF8ToString(acceptPtr);
    const uploadRoot = UTF8ToString(uploadRootPtr);
    const input = document.createElement('input');
    input.type = 'file';
    input.accept = accept;
    input.style.display = 'none';
    let done = false;
    const finish = (path) => {
        if (done) return;
        done = true;
        input.remove();
        Module.ccall('enp_web_file_dialog_result', null, ['number', 'string'], [requestId, path]);
    };
    input.addEventListener('cancel', () => finish(''));
    input.addEventListener('change', () => {
        const file = input.files && input.files[0];
        if (!file) { finish(''); return; }
        const reader = new FileReader();
        reader.onload = () => {
            const path = uploadRoot + '/' + file.name;
            FS.writeFile(path, new Uint8Array(reader.result));
            finish(path);
        };
        reader.onerror = () => finish('');
        reader.readAsArrayBuffer(file);
    });
    document.body.appendChild(input);
    input.click();
});

EM_JS(void, enp_js_open_window, (const char* urlPtr), {
    window.open(UTF8ToString(urlPtr), '_blank', 'noopener');
});
// clang-format on

extern "C" EMSCRIPTEN_KEEPALIVE void enp_web_file_dialog_result(int requestId, const char* path)
{
    auto it = g_pendingDialogs.find(requestId);
    if (it == g_pendingDialogs.end())
        return;
    dialog::OpenFileCallback callback = std::move(it->second);
    g_pendingDialogs.erase(it);
    std::optional<std::string> result;
    if (path && *path)
        result = path;
    dispatch::postToMain([callback = std::move(callback), result]() { callback(result); });
}

namespace detail {

bool platformInit(const InitParams&, std::vector<std::string>& outArguments)
{
    fs::createDirectories(kUploadRoot);
    // Expose "?doc=<url-or-asset-path>" as the first launch argument.
    const char* doc = emscripten_run_script_string(
        "(function(){try{return new URLSearchParams(window.location.search).get('doc')||'';}catch(e){return '';}})()");
    if (doc && *doc)
        outArguments.emplace_back(doc);
    return true;
}

void platformShutdown() {}

void writeLogSink(log::Level level, const char* message)
{
    static const char* const prefixes[] = {"[debug] ", "[info] ", "[warn] ", "[error] "};
    std::fprintf(level >= log::Level::Warning ? stderr : stdout, "%s%s\n", prefixes[int(level)], message);
}


} // namespace detail

namespace fs {

std::string executableDirectory() { return "/"; }

std::string appDataDirectory()
{
    std::string dir = "/home/web_user/.easynotepad";
    createDirectories(dir);
    return dir;
}

std::vector<std::string> rootDirectories() { return {kUploadRoot, kAssetRoot}; }

std::optional<std::string> readAsset(const std::string& relativePath)
{
    return readFile(path::join(kAssetRoot, relativePath));
}

std::optional<std::string> readAssetRange(const std::string& relativePath, std::uint64_t offset, size_t size, std::uint64_t* total)
{
    return readFileRange(path::join(kAssetRoot, relativePath), offset, size, total);
}

} // namespace fs

namespace dialog {

bool hasNativeOpenFile() { return true; }

void openFile(const std::string&, const std::vector<FileFilter>& filters, OpenFileCallback callback)
{
    std::string accept;
    for (const FileFilter& f : filters)
        for (const std::string& ext : f.extensions)
            accept += (accept.empty() ? "." : ",.") + ext;
    int id = g_nextDialogId++;
    g_pendingDialogs[id] = std::move(callback);
    enp_js_open_file_dialog(id, accept.c_str(), kUploadRoot);
}

} // namespace dialog

namespace shell {

bool revealInFileManager(const std::string& path)
{
    // The folder (a file's folder) in the default file manager.
    return openExternal(fs::isDirectory(path) ? path : fs::path::parent(path));
}

bool openExternal(const std::string& urlOrPath)
{
    if (!detail::isUrl(urlOrPath))
        return false;
    enp_js_open_window(urlOrPath.c_str());
    return true;
}

} // namespace shell

} // namespace enp::os
