/* download_service.cpp — backends behind the Download API declared in
 * download_service.h. See the header for the contract. */
#include "platform/http/download_service.h"

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

#else // desktop: spawned curl child (killable for mid-transfer cancel)

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#endif

namespace ssb64 {

struct Download {
#if defined(_WIN32)
    HANDLE proc = nullptr;
#else
    pid_t pid = -1;
    FILE* pipe = nullptr;
#endif
    // mutable: Poll stores the terminal state it observes so Finish can
    // report it after the child has already been reaped (pclose would
    // otherwise lose the exit status to ECHILD).
    mutable DownloadState state = DownloadState::Running;
    bool cancelRequested = false;
};

namespace {

std::string ShellQuote(const std::filesystem::path& p) {
    return "\"" + p.generic_string() + "\"";
}

std::string CurlCommandPrefix() {
#if defined(__linux__)
    // AppImage: bundled libs poison distro tools — same strip as Updater.
    return "env -u LD_LIBRARY_PATH -u LD_PRELOAD curl";
#else
    return "curl";
#endif
}

#if defined(_WIN32)
HANDLE SpawnCurl(const std::string& cmd) {
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string mutableCmd = cmd;
    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        SPDLOG_ERROR("download: CreateProcess failed ({}) for `{}`",
                     GetLastError(), cmd);
        return nullptr;
    }
    CloseHandle(pi.hThread);
    return pi.hProcess;
}
#else
/* The shim reports its own pid then exec()s curl — the printed pid IS
 * curl's after exec, so the caller can SIGTERM it and waitpid() it
 * directly instead of relying on pclose's internal reap. */
pid_t SpawnCurl(const std::string& curlArgs, FILE** pipeOut) {
    const std::string shim = "sh -c 'echo $$; exec " + curlArgs + "'";
    FILE* pipe = popen(shim.c_str(), "r");
    if (!pipe) {
        SPDLOG_ERROR("download: popen failed for `{}`", shim);
        return -1;
    }
    pid_t pid = -1;
    char buf[64];
    if (fgets(buf, sizeof(buf), pipe)) {
        pid = static_cast<pid_t>(std::strtol(buf, nullptr, 10));
    }
    *pipeOut = pipe;
    return pid;
}
#endif

} // namespace

Download* DownloadStart(const std::string& url,
                        const std::filesystem::path& destPath) {
    const std::string args = "-fLs -o " + ShellQuote(destPath) + " \"" + url + "\"";
    auto* d = new Download;
#if defined(_WIN32)
    d->proc = SpawnCurl(CurlCommandPrefix() + " " + args);
    if (d->proc == nullptr) {
        delete d;
        return nullptr;
    }
#else
    d->pid = SpawnCurl(CurlCommandPrefix() + " " + args, &d->pipe);
    if (d->pid <= 0 || d->pipe == nullptr) {
        if (d->pipe) pclose(d->pipe);
        delete d;
        return nullptr;
    }
#endif
    return d;
}

DownloadState DownloadPoll(const Download* d) {
    if (d->state != DownloadState::Running) return d->state;
#if defined(_WIN32)
    if (WaitForSingleObject(d->proc, 0) == WAIT_TIMEOUT) {
        return DownloadState::Running;
    }
    DWORD code = 1;
    GetExitCodeProcess(d->proc, &code);
    d->state = d->cancelRequested ? DownloadState::Cancelled
                                  : (code == 0 ? DownloadState::Done : DownloadState::Error);
#else
    // WNOHANG reaps a finished child — the terminal state is stored in
    // d->state because pclose can't recover it afterwards (ECHILD).
    int status = 0;
    if (waitpid(d->pid, &status, WNOHANG) <= 0) {
        return DownloadState::Running;
    }
    d->state = d->cancelRequested ? DownloadState::Cancelled
        : (WIFEXITED(status) && WEXITSTATUS(status) == 0
               ? DownloadState::Done : DownloadState::Error);
#endif
    return d->state;
}

void DownloadCancel(Download* d) {
    d->cancelRequested = true;
#if defined(_WIN32)
    if (d->proc != nullptr) TerminateProcess(d->proc, 1);
#else
    if (d->pid > 0) kill(d->pid, SIGTERM);
#endif
}

DownloadState DownloadFinish(Download* d) {
    if (d->state == DownloadState::Running) {
        // Still in flight when joined — block until it exits (a cancel
        // was already signalled, or the caller stopped polling early).
#if defined(_WIN32)
        WaitForSingleObject(d->proc, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(d->proc, &code);
        d->state = d->cancelRequested ? DownloadState::Cancelled
                                      : (code == 0 ? DownloadState::Done : DownloadState::Error);
#else
        int status = 0;
        if (waitpid(d->pid, &status, 0) > 0) {
            d->state = d->cancelRequested ? DownloadState::Cancelled
                : (WIFEXITED(status) && WEXITSTATUS(status) == 0
                       ? DownloadState::Done : DownloadState::Error);
        } else {
            d->state = d->cancelRequested ? DownloadState::Cancelled
                                          : DownloadState::Error;
        }
#endif
    }
#if defined(_WIN32)
    if (d->proc != nullptr) CloseHandle(d->proc);
#else
    if (d->pipe != nullptr) {
        // Free the popen FILE. The exit status is already in d->state —
        // pclose is only bookkeeping here (its own waitpid may ECHILD).
        char buf[256];
        while (fgets(buf, sizeof(buf), d->pipe) != nullptr) {
        }
        pclose(d->pipe);
    }
#endif
    const DownloadState st = d->state;
    delete d;
    return st;
}

} // namespace ssb64

#endif
