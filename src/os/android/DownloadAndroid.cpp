// Android downloads (see Download.h): MainActivity fetches the file on a thread of its own (HTTP
// with Range resume and retries), or copies one the user picked in the system file picker.
#include "enp/os/Download.h"

#include "JniEnv.h"

#include "enp/os/Fs.h"
#include "enp/os/Log.h"

namespace enp::os::download {

namespace {

jmethodID activityMethod(android::ScopedJniEnv& env, const char* name, const char* signature)
{
    jclass cls = env->GetObjectClass(android::app()->activity->clazz);
    return env->GetMethodID(cls, name, signature);
}

// MainActivity.getLastDownloadError(): the retry note, or why it failed.
std::string lastMessage(android::ScopedJniEnv& env)
{
    std::string message;
    if (jmethodID id = activityMethod(env, "getLastDownloadError", "()Ljava/lang/String;")) {
        if (auto text = static_cast<jstring>(env->CallObjectMethod(android::app()->activity->clazz, id))) {
            if (const char* chars = env->GetStringUTFChars(text, nullptr)) {
                message = chars;
                env->ReleaseStringUTFChars(text, chars);
            }
            env->DeleteLocalRef(text);
        }
    }
    env.clearException();
    return message;
}

// Calls a (String...)Z method of MainActivity; on false, `error` gets its message.
bool callStart(const char* method, const char* signature, const std::string& a, const std::string* b, bool flag,
               std::string* error)
{
    android::ScopedJniEnv env;
    if (!env)
        return false;
    jmethodID id = activityMethod(env, method, signature);
    if (!id) {
        env.clearException();
        if (error)
            *error = "this build cannot do that";
        return false;
    }
    jstring ja = env->NewStringUTF(a.c_str());
    jstring jb = b ? env->NewStringUTF(b->c_str()) : nullptr;
    const bool ok = b ? env->CallBooleanMethod(android::app()->activity->clazz, id, ja, jb, jboolean(flag))
                      : env->CallBooleanMethod(android::app()->activity->clazz, id, ja);
    env->DeleteLocalRef(ja);
    if (jb)
        env->DeleteLocalRef(jb);
    if (env.clearException() || !ok) {
        const std::string why = lastMessage(env);
        if (error)
            *error = why.empty() ? "another download or import is running" : why;
        ENP_LOG_WARN("%s failed to start: %s", method, why.c_str());
        return false;
    }
    return true;
}

} // namespace

std::string directory()
{
    static std::string dir; // created once (asked for every frame)
    if (dir.empty()) {
        const char* external = android::app()->activity->externalDataPath;
        dir = fs::path::join(external ? std::string(external) : fs::appDataDirectory(), "models");
        fs::createDirectories(dir);
    }
    return dir;
}

bool start(const std::string& url, const std::string& path, bool allowMetered, std::string* error)
{
    return callStart("startDownload", "(Ljava/lang/String;Ljava/lang/String;Z)Z", url, &path, allowMetered, error);
}

bool canImport() { return true; }

bool importFile(const std::string& path, std::string* error)
{
    return callStart("importFile", "(Ljava/lang/String;)Z", path, nullptr, false, error);
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
    case 2: s.state = Status::State::Running; s.error = lastMessage(env); break; // retrying: the note says why
    case 3: s.state = Status::State::Done; break;
    case 4: s.state = Status::State::Failed; s.error = lastMessage(env); break;
    default: s.state = Status::State::Idle; break;
    }
    return s;
}

bool meteredNetwork()
{
    android::ScopedJniEnv env;
    if (!env)
        return false;
    jmethodID id = activityMethod(env, "isMeteredNetwork", "()Z");
    if (!id) {
        env.clearException();
        return false;
    }
    const bool metered = env->CallBooleanMethod(android::app()->activity->clazz, id);
    return !env.clearException() && metered;
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
