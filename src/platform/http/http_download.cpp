/* http_download.cpp — per-platform backends behind the common
 * HttpDownloadToFile contract declared in http_download.h. */
#include "http_download.h"
#include "url_safety.h"

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

#elif defined(SSB64_HAVE_CURL) // desktop: in-process libcurl

#include "http_curl.h"

namespace ssb64 {

bool HttpDownloadToFile(const std::string& url,
                        const std::filesystem::path& destPath) {
    if (!IsAllowedHttpUrl(url)) {
        SPDLOG_ERROR("http download: refusing unsafe URL `{}`", url);
        return false;
    }
    std::string error;
    if (!CurlDownloadToFile(url, destPath, &error)) {
        SPDLOG_ERROR("http download: `{}` failed: {}", url, error);
        return false;
    }
    return true;
}

} // namespace ssb64

#else // no curl backend (Switch): downloads unavailable

namespace ssb64 {
bool HttpDownloadToFile(const std::string&, const std::filesystem::path&) {
    return false;
}
} // namespace ssb64

#endif
