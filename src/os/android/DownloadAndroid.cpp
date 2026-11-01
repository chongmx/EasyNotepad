// Android downloads (see Download.h): MainActivity hands them to the system DownloadManager.
#include "enp/os/Download.h"

#include "JniEnv.h"

#include "enp/os/Fs.h"

namespace enp::os::download {

namespace {

jmethodID activityMethod(android::ScopedJniEnv& env, const char* name, const char* signature)
{
    jclass cls = env->GetObjectClass(android::app()->activity->clazz);
    return env->GetMethodID(cls, name, signature);
}

// DownloadManager's COLUMN_REASON codes, for a paused or failed transfer.
std::string reasonText(long long reason, bool paused)
{
    if (paused) {
        switch (reason) {
        case 1: return "waiting to retry";
        case 2: return "waiting for a network";
        case 3: return "waiting for Wi-Fi (mobile data not allowed)";
        default: return "paused";
        }
    }
    switch (reason) {
    case 1006: return "not enough storage";
    case 1007: return "storage not found";
    case 1009: return "file already exists";
    case 1001: return "file error";
    case 1004: return "network data error";
    case 1005: return "too many redirects";
    default: return reason >= 400 && reason < 600 ? "HTTP " + std::to_string(reason) : "download failed (" + std::to_string(reason) + ")";
    }
}

} // namespace

std::string directory()
{
    const char* external = android::app()->activity->externalDataPath;
    std::string dir = external ? std::string(external) : fs::appDataDirectory();
    dir = fs::path::join(dir, "models");
    fs::createDirectories(dir);
    return dir;
}

bool start(const std::string& url, const std::string& path, bool allowMetered, std::string* error)
{
    android::ScopedJniEnv env;
    if (!env)
        return false;
    jmethodID id = activityMethod(env, "startDownload", "(Ljava/lang/String;Ljava/lang/String;Z)Z");
    if (!id) {
        env.clearException();
        if (error)
            *error = "this build cannot download";
        return false;
    }
    jstring jurl = env->NewStringUTF(url.c_str());
    jstring jpath = env->NewStringUTF(path.c_str());
    const bool ok = env->CallBooleanMethod(android::app()->activity->clazz, id, jurl, jpath, jboolean(allowMetered));
    env->DeleteLocalRef(jurl);
    env->DeleteLocalRef(jpath);
    if (env.clearException() || !ok) {
        if (error)
            *error = "the system download service refused the download";
        return false;
    }
    return true;
}

Status status()
{
    Status s;
    android::ScopedJniEnv env;
    if (!env)
        return s;
    jmethodID id = activityMethod(env, "downloadStatus", "()[J");
    if (!id) {
        env.clearException();
        return s;
    }
    auto array = static_cast<jlongArray>(env->CallObjectMethod(android::app()->activity->clazz, id));
    if (env.clearException() || !array || env->GetArrayLength(array) < 4)
        return s;
    jlong v[4];
    env->GetLongArrayRegion(array, 0, 4, v);
    env->DeleteLocalRef(array);
    s.received = v[1] > 0 ? uint64_t(v[1]) : 0;
    s.total = v[2] > 0 ? uint64_t(v[2]) : 0;
    switch (v[0]) {
    case 1: s.state = Status::State::Running; break;
    case 2: s.state = Status::State::Running; s.error = reasonText(v[3], true); break;
    case 3: s.state = Status::State::Done; break;
    case 4: s.state = Status::State::Failed; s.error = reasonText(v[3], false); break;
    default: s.state = Status::State::Idle; break;
    }
    return s;
}

void cancel()
{
    android::ScopedJniEnv env;
    if (!env)
        return;
    if (jmethodID id = activityMethod(env, "cancelDownload", "()V"))
        env->CallVoidMethod(android::app()->activity->clazz, id);
    env.clearException();
}

} // namespace enp::os::download
