/* download_service.cpp — backends behind the Download API declared in
 * download_service.h. See the header for the contract. */
#include "download_service.h"
#include "url_safety.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#if defined(__ANDROID__)

/* Android backend: static methods on BattleShipActivity manage a
 * registry of HttpURLConnection worker threads (INTERNET permission is
 * in the manifest). The native side keeps only the Java task id. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <jni.h>

namespace ssb64 {

struct Download {
    jint taskId;
    DownloadState state = DownloadState::Running;
    bool cancelRequested = false;
};

namespace {

/* Resolve the activity class + env for a static method call. Returns
 * false (after logging) if the JNI bridge is unavailable. */
bool JniGet(JNIEnv** envOut, jclass* clsOut) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    jobject activity = env ? static_cast<jobject>(SDL_AndroidGetActivity()) : nullptr;
    if (env == nullptr || activity == nullptr) {
        SPDLOG_ERROR("download: SDL JNI bridge unavailable");
        return false;
    }
    jclass cls = env->GetObjectClass(activity);
    env->DeleteLocalRef(activity);
    if (cls == nullptr) {
        SPDLOG_ERROR("download: cannot resolve activity class");
        return false;
    }
    *envOut = env;
    *clsOut = cls;
    return true;
}

/* Call a static int method. Two signatures share this helper:
 * (String,String)I when id < 0 (startDownload) and (I)I when id >= 0
 * (pollDownload / finishDownload). -1 signals a JNI failure — callers
 * treat it as Error. */
jint CallStaticInt(const char* name, const std::string& a,
                   const std::string& b, jint id) {
    JNIEnv* env; jclass cls;
    if (!JniGet(&env, &cls)) return -1;
    jmethodID mid;
    jint rc = -1;
    if (id >= 0) {
        mid = env->GetStaticMethodID(cls, name, "(I)I");
        if (mid != nullptr) rc = env->CallStaticIntMethod(cls, mid, id);
    } else {
        mid = env->GetStaticMethodID(cls, name,
                                   "(Ljava/lang/String;Ljava/lang/String;)I");
        if (mid != nullptr) {
            jstring ja = env->NewStringUTF(a.c_str());
            jstring jb = env->NewStringUTF(b.c_str());
            rc = env->CallStaticIntMethod(cls, mid, ja, jb);
            env->DeleteLocalRef(ja);
            env->DeleteLocalRef(jb);
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    return rc;
}

void CallStaticVoid(const char* name, jint id) {
    JNIEnv* env; jclass cls;
    if (!JniGet(&env, &cls)) return;
    jmethodID mid = env->GetStaticMethodID(cls, name, "(I)V");
    if (mid != nullptr) {
        env->CallStaticVoidMethod(cls, mid, id);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
}

DownloadState FromJavaState(jint s) {
    switch (s) {
        case 0: return DownloadState::Running;
        case 1: return DownloadState::Done;
        case 3: return DownloadState::Cancelled;
        default: return DownloadState::Error;
    }
}

} // namespace

Download* DownloadStart(const std::string& url,
                        const std::filesystem::path& destPath) {
    const jint id = CallStaticInt("startDownload", url, destPath.string(), -1);
    if (id < 0) {
        SPDLOG_ERROR("download: startDownload JNI call failed");
        return nullptr;
    }
    auto* d = new Download;
    d->taskId = id;
    return d;
}

DownloadState DownloadPoll(const Download* d) {
    if (d->state != DownloadState::Running) return d->state;
    const jint s = CallStaticInt("pollDownload", {}, {}, d->taskId);
    return FromJavaState(s);
}

void DownloadCancel(Download* d) {
    d->cancelRequested = true;
    CallStaticVoid("cancelDownload", d->taskId);
}

DownloadState DownloadFinish(Download* d) {
    const jint s = CallStaticInt("finishDownload", {}, {}, d->taskId);
    DownloadState st = FromJavaState(s);
    if (st == DownloadState::Error && d->cancelRequested) {
        st = DownloadState::Cancelled;
    }
    delete d;
    return st;
}

} // namespace ssb64

#elif defined(BATTLESHIP_UWP)

namespace ssb64 {
Download* DownloadStart(const std::string&, const std::filesystem::path&) {
    return nullptr;
}
DownloadState DownloadPoll(const Download*) { return DownloadState::Error; }
void DownloadCancel(Download*) {}
DownloadState DownloadFinish(Download*) { return DownloadState::Error; }
} // namespace ssb64

#elif defined(SSB64_HAVE_CURL) // desktop: in-process libcurl worker

#include "http_curl.h"

#include <atomic>
#include <thread>

namespace ssb64 {

/* The worker runs one blocking easy transfer; DownloadCancel flips the
 * flag the XFERINFO callback polls (≤200ms to abort), Finish joins the
 * thread so the handle is always destroyed exactly once. */
struct Download {
    std::thread worker;
    std::atomic<DownloadState> state{ DownloadState::Running };
    std::atomic<bool> cancelRequested{ false };
};

Download* DownloadStart(const std::string& url,
                        const std::filesystem::path& destPath) {
    if (!IsAllowedHttpUrl(url)) {
        SPDLOG_ERROR("download: refusing unsafe URL `{}`", url);
        return nullptr;
    }
    auto* d = new Download;
    d->worker = std::thread([d, url, destPath] {
        std::string error;
        const bool ok = CurlDownloadToFile(url, destPath, &error, nullptr, &d->cancelRequested);
        if (ok) {
            d->state = DownloadState::Done;
        } else if (d->cancelRequested.load()) {
            d->state = DownloadState::Cancelled;
        } else {
            SPDLOG_ERROR("download: `{}` failed: {}", url, error);
            d->state = DownloadState::Error;
        }
    });
    return d;
}

DownloadState DownloadPoll(const Download* d) {
    return d->state.load();
}

void DownloadCancel(Download* d) {
    d->cancelRequested = true;
}

DownloadState DownloadFinish(Download* d) {
    if (d->worker.joinable()) {
        d->worker.join();
    }
    const DownloadState st = d->state.load();
    delete d;
    return st;
}

} // namespace ssb64

#else // no curl backend (Switch): downloads unavailable

namespace ssb64 {
Download* DownloadStart(const std::string&, const std::filesystem::path&) {
    return nullptr;
}
DownloadState DownloadPoll(const Download*) { return DownloadState::Error; }
void DownloadCancel(Download*) {}
DownloadState DownloadFinish(Download*) { return DownloadState::Error; }
} // namespace ssb64

#endif
