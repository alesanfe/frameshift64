/* http_download.cpp — per-platform backends behind the common
 * HttpDownloadToFile contract declared in http_download.h. */
#include "platform/http/http_download.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include <spdlog/spdlog.h>

#if defined(__ANDROID__)

/* Android: apps cannot exec helper binaries, so the download runs
 * through java.net.HttpURLConnection via JNI. The Java side lives in
 * BattleShipActivity.httpDownloadToFile (platforms/android/). Requires
 * the INTERNET permission declared in AndroidManifest.xml. */
#include <SDL2/SDL.h>
#include <SDL2/SDL_system.h>
#include <jni.h>

namespace ssb64 {

bool HttpDownloadToFile(const std::string& url,
                        const std::filesystem::path& destPath) {
    JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
    if (env == nullptr) {
        SPDLOG_ERROR("http download: SDL_AndroidGetJNIEnv failed");
        return false;
    }
    jobject activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (activity == nullptr) {
        SPDLOG_ERROR("http download: SDL_AndroidGetActivity failed");
        return false;
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID mid = env->GetMethodID(cls, "httpDownloadToFile",
                                   "(Ljava/lang/String;Ljava/lang/String;)Z");
    if (mid == nullptr) {
        env->ExceptionClear();
        SPDLOG_ERROR("http download: httpDownloadToFile not found on activity");
        env->DeleteLocalRef(activity);
        env->DeleteLocalRef(cls);
        return false;
    }
    jstring jurl = env->NewStringUTF(url.c_str());
    jstring jdest = env->NewStringUTF(destPath.string().c_str());
    const jboolean ok = env->CallBooleanMethod(activity, mid, jurl, jdest);
    const bool threw = env->ExceptionCheck();
    if (threw) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(jurl);
    env->DeleteLocalRef(jdest);
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    if (!ok || threw) {
        std::error_code ec;
        std::filesystem::remove(destPath, ec);
    }
    return ok == JNI_TRUE && !threw;
}

} // namespace ssb64

#elif defined(BATTLESHIP_UWP)

/* UWP: packages ship as MSIX and the features that download content
 * are compiled out — the file stays in the glob, so provide the stub. */
namespace ssb64 {
bool HttpDownloadToFile(const std::string&, const std::filesystem::path&) {
    return false;
}
} // namespace ssb64

#else // desktop: Windows / Linux / macOS — `curl` on PATH

namespace ssb64 {
namespace {

/* Run a subprocess, drain stdout so the child can't block on a full
 * pipe, and return true on exit code 0. */
bool RunSilent(const std::string& cmd) {
#if defined(_WIN32)
    FILE* pipe = _popen(cmd.c_str(), "r");
#else
    FILE* pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) {
        SPDLOG_ERROR("http download: popen failed for `{}`", cmd);
        return false;
    }
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
    }
#if defined(_WIN32)
    const int rc = _pclose(pipe);
#else
    const int rc = pclose(pipe);
#endif
    if (rc != 0) {
        // 127 = "command not found" — breadcrumb for minimal Linux
        // installs without curl.
        SPDLOG_ERROR("http download: command failed (exit={}): `{}`", rc, cmd);
    }
    return rc == 0;
}

/* Forward slashes are accepted by every tool we shell out to and avoid
 * the cmd.exe pitfall where a path ending in a backslash escapes the
 * closing quote. */
std::string ShellQuote(const std::filesystem::path& p) {
    return "\"" + p.generic_string() + "\"";
}

} // namespace

bool HttpDownloadToFile(const std::string& url,
                        const std::filesystem::path& destPath) {
    const std::string cmd = "curl -fLs -o " + ShellQuote(destPath) + " \"" + url + "\"";
    if (RunSilent(cmd)) {
        return true;
    }
    std::error_code ec;
    std::filesystem::remove(destPath, ec);
    return false;
}

} // namespace ssb64

#endif
