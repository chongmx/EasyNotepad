// Android backend (NativeActivity).
#include "../OsInternal.h"
#include "JniEnv.h"

#include <algorithm>
#include <android/asset_manager.h>
#include <android/log.h>
#include <mutex>

namespace enp::os {

namespace android {

namespace {
android_app* g_app = nullptr;
} // namespace

android_app* app() { return g_app; }

} // namespace android

namespace {

// Reads getIntent().getDataString() so "Open with EasyNotepad" works for file:// URIs.
std::optional<std::string> launchIntentData()
{
    android::ScopedJniEnv env;
    if (!env)
        return std::nullopt;
    jobject activity = android::app()->activity->clazz;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getIntent = env->GetMethodID(activityClass, "getIntent", "()Landroid/content/Intent;");
    jobject intent = getIntent ? env->CallObjectMethod(activity, getIntent) : nullptr;
    std::optional<std::string> result;
    if (intent && !env.clearException()) {
        jclass intentClass = env->GetObjectClass(intent);
        jmethodID getDataString = env->GetMethodID(intentClass, "getDataString", "()Ljava/lang/String;");
        auto data = static_cast<jstring>(env->CallObjectMethod(intent, getDataString));
        if (data && !env.clearException()) {
            const char* utf = env->GetStringUTFChars(data, nullptr);
            result = std::string(utf);
            env->ReleaseStringUTFChars(data, utf);
        }
    }
    env.clearException();
    return result;
}

// ---- Files outside the app's folders: MainActivity's storage helpers ------------------------

jmethodID activityMethod(android::ScopedJniEnv& env, const char* name, const char* signature)
{
    jclass cls = env->GetObjectClass(android::app()->activity->clazz);
    jmethodID id = env->GetMethodID(cls, name, signature);
    env->DeleteLocalRef(cls);
    if (!id)
        env.clearException();
    return id;
}

// Calls a ()Z method of MainActivity; false if it cannot be called.
bool callActivityBool(const char* name)
{
    android::ScopedJniEnv env;
    if (!env)
        return false;
    jmethodID id = activityMethod(env, name, "()Z");
    const bool result = id && env->CallBooleanMethod(android::app()->activity->clazz, id);
    return !env.clearException() && result;
}

// Calls a ()V method of MainActivity.
void callActivityVoid(const char* name)
{
    android::ScopedJniEnv env;
    if (!env)
        return;
    if (jmethodID id = activityMethod(env, name, "()V"))
        env->CallVoidMethod(android::app()->activity->clazz, id);
    env.clearException();
}

// The shared storage ("/storage/emulated/<user>"), asked once: it does not move while the app runs.
const std::string& sharedStorage()
{
    static const std::string dir = []() {
        std::string result = "/storage/emulated/0";
        android::ScopedJniEnv env;
        if (!env)
            return result;
        if (jmethodID id = activityMethod(env, "sharedStoragePath", "()Ljava/lang/String;")) {
            auto text = static_cast<jstring>(env->CallObjectMethod(android::app()->activity->clazz, id));
            if (text && !env.clearException()) {
                if (const char* utf = env->GetStringUTFChars(text, nullptr)) {
                    if (*utf)
                        result = utf;
                    env->ReleaseStringUTFChars(text, utf);
                }
            }
            if (text)
                env->DeleteLocalRef(text);
        }
        env.clearException();
        return result;
    }();
    return dir;
}

} // namespace

namespace detail {

bool platformInit(const InitParams& params, std::vector<std::string>& outArguments)
{
    android::g_app = static_cast<android_app*>(params.nativeHandle);
    if (!android::g_app)
        return false;
    if (std::optional<std::string> data = launchIntentData()) {
        if (data->rfind("file://", 0) == 0)
            outArguments.push_back(detail::percentDecode(data->substr(7)));
        else if (detail::isUrl(*data) && data->rfind("content://", 0) != 0)
            outArguments.push_back(*data);
    }
    return true;
}

void platformShutdown() {}

void writeLogSink(log::Level level, const char* message)
{
    static const int priorities[] = {ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN, ANDROID_LOG_ERROR};
    __android_log_write(priorities[int(level)], "EasyNotepad", message);
}


} // namespace detail

namespace fs {

std::string executableDirectory() { return android::app()->activity->internalDataPath; }

std::string appDataDirectory()
{
    std::string dir = android::app()->activity->internalDataPath;
    createDirectories(dir);
    return dir;
}

bool hasStorageAccess()
{
    // Asked every frame while the file dialog is open; the answer changes when the user comes back
    // from the system settings, so it is kept for a second.
    static std::mutex lock;
    static double checkedAt = -1.0;
    static bool granted = false;
    std::lock_guard<std::mutex> guard(lock);
    if (checkedAt < 0.0 || now() - checkedAt > 1.0) {
        granted = callActivityBool("hasStorageAccess");
        checkedAt = now();
    }
    return granted;
}

void requestStorageAccess() { callActivityVoid("requestStorageAccess"); }

std::string homeDirectory()
{
    // "/" and "/storage/emulated" cannot be listed, so a browser starting there could never reach
    // the user's files: start in the shared storage itself, or in the app's folder without access.
    return hasStorageAccess() ? sharedStorage() : std::string(android::app()->activity->internalDataPath);
}

std::vector<std::string> rootDirectories()
{
    const std::string shared = sharedStorage();
    std::vector<std::string> roots{shared, path::join(shared, "Download"), path::join(shared, "Documents")};
    // SD cards and USB drives: /storage/<volume id> next to "emulated" (the shared storage) and "self".
    for (const DirEntry& e : listDirectory("/storage"))
        if (e.isDirectory && e.name != "emulated" && e.name != "self")
            roots.push_back(path::join("/storage", e.name));
    roots.emplace_back(android::app()->activity->internalDataPath);
    if (const char* ext = android::app()->activity->externalDataPath)
        roots.emplace_back(ext);
    return roots;
}

std::optional<std::string> readAssetRange(const std::string& relativePath, std::uint64_t offset, size_t size, std::uint64_t* total)
{
    AAsset* asset = AAssetManager_open(android::app()->activity->assetManager, relativePath.c_str(), AASSET_MODE_RANDOM);
    if (!asset)
        return std::nullopt;
    const std::uint64_t length = std::uint64_t(AAsset_getLength64(asset));
    if (total)
        *total = length;
    std::string data;
    if (offset < length && AAsset_seek64(asset, off64_t(offset), SEEK_SET) >= 0) {
        data.resize(size_t(std::min<std::uint64_t>(size, length - offset)));
        size_t got = 0;
        while (got < data.size()) {
            const int n = AAsset_read(asset, data.data() + got, data.size() - got);
            if (n <= 0)
                break;
            got += size_t(n);
        }
        data.resize(got);
    }
    AAsset_close(asset);
    return data;
}

std::optional<std::string> readAsset(const std::string& relativePath)
{
    AAsset* asset = AAssetManager_open(android::app()->activity->assetManager, relativePath.c_str(), AASSET_MODE_BUFFER);
    if (!asset)
        return std::nullopt;
    std::string data(size_t(AAsset_getLength(asset)), '\0');
    int64_t read = AAsset_read(asset, data.data(), data.size());
    AAsset_close(asset);
    if (read < 0)
        return std::nullopt;
    data.resize(size_t(read));
    return data;
}

} // namespace fs

namespace dialog {

// NativeActivity cannot receive onActivityResult(), so the Storage Access Framework picker
// is unavailable; the UI falls back to its built-in file browser.
bool hasNativeOpenFile() { return false; }

void openFile(const std::string&, const std::vector<FileFilter>&, OpenFileCallback callback)
{
    dispatch::postToMain([callback = std::move(callback)]() { callback(std::nullopt); });
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
        return false; // file:// URIs cannot be shared with other apps (FileUriExposedException)

    android::ScopedJniEnv env;
    if (!env)
        return false;
    jclass uriClass = env->FindClass("android/net/Uri");
    jmethodID parse = env->GetStaticMethodID(uriClass, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
    jstring jurl = env->NewStringUTF(urlOrPath.c_str());
    jobject uri = env->CallStaticObjectMethod(uriClass, parse, jurl);

    jclass intentClass = env->FindClass("android/content/Intent");
    jmethodID ctor = env->GetMethodID(intentClass, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
    jstring action = env->NewStringUTF("android.intent.action.VIEW");
    jobject intent = env->NewObject(intentClass, ctor, action, uri);

    jobject activity = android::app()->activity->clazz;
    jmethodID startActivity = env->GetMethodID(env->GetObjectClass(activity), "startActivity", "(Landroid/content/Intent;)V");
    env->CallVoidMethod(activity, startActivity, intent);
    bool failed = env.clearException();

    env->DeleteLocalRef(intent);
    env->DeleteLocalRef(action);
    env->DeleteLocalRef(uri);
    env->DeleteLocalRef(jurl);
    return !failed;
}

} // namespace shell

} // namespace enp::os
