// Android backend (NativeActivity).
#include "../OsInternal.h"
#include "JniEnv.h"

#include <android/asset_manager.h>
#include <android/log.h>

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

std::vector<std::string> rootDirectories()
{
    std::vector<std::string> roots{android::app()->activity->internalDataPath};
    if (const char* ext = android::app()->activity->externalDataPath)
        roots.emplace_back(ext);
    roots.emplace_back("/storage/emulated/0/Download");
    roots.emplace_back("/storage/emulated/0/Documents");
    return roots;
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
