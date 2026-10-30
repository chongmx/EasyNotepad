// Scoped JNIEnv access for any thread (attaches and detaches as needed).
#pragma once

#include <android_native_app_glue.h>
#include <jni.h>

namespace enp::os::android {

// Set by platformInit(); valid for the lifetime of the process.
android_app* app();

class ScopedJniEnv {
public:
    ScopedJniEnv()
    {
        JavaVM* vm = app()->activity->vm;
        jint rc = vm->GetEnv(reinterpret_cast<void**>(&env_), JNI_VERSION_1_6);
        if (rc == JNI_EDETACHED) {
            if (vm->AttachCurrentThread(&env_, nullptr) == JNI_OK)
                attached_ = true;
            else
                env_ = nullptr;
        }
    }
    ~ScopedJniEnv()
    {
        if (attached_)
            app()->activity->vm->DetachCurrentThread();
    }
    ScopedJniEnv(const ScopedJniEnv&) = delete;
    ScopedJniEnv& operator=(const ScopedJniEnv&) = delete;

    JNIEnv* operator->() const { return env_; }
    JNIEnv* get() const { return env_; }
    explicit operator bool() const { return env_ != nullptr; }

    // Clears a pending Java exception; returns true if there was one.
    bool clearException() const
    {
        if (!env_->ExceptionCheck())
            return false;
        env_->ExceptionClear();
        return true;
    }

private:
    JNIEnv* env_ = nullptr;
    bool attached_ = false;
};

} // namespace enp::os::android
