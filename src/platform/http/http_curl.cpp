/* See http_curl.h. */
#include "http_curl.h"

#include <curl/curl.h>

#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace ssb64 {
namespace {

/* curl_global_init is documented as not thread-safe; run it once up
 * front instead of trusting callers to serialize their first transfer. */
std::once_flag sGlobalOnce;

CURL* MakeEasy(char (*errorBuf)[CURL_ERROR_SIZE]) {
    std::call_once(sGlobalOnce, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    CURL* h = curl_easy_init();
    if (h == nullptr) {
        return nullptr;
    }
    (*errorBuf)[0] = '\0';
    curl_easy_setopt(h, CURLOPT_ERRORBUFFER, *errorBuf);
    curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(h, CURLOPT_MAXREDIRS, 8L);
    /* Callers vet entry URLs with IsAllowedHttpUrl (http/https only), but the
     * vetting happens above the sink and FOLLOWLOCATION would otherwise
     * chase a 302 to file:// and copy a local path into the destination —
     * pin the protocol set here so every client inherits it. *_STR names
     * need curl 7.85+; older system curls get the bitmask form. */
#if LIBCURL_VERSION_NUM >= 0x075500
    curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
#else
    curl_easy_setopt(h, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(h, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
#endif
    /* Same contract as the curl -fLs shim: fail (not save) on HTTP >= 400. */
    curl_easy_setopt(h, CURLOPT_FAILONERROR, 1L);
    /* GitHub's API 403s requests without a UA; harmless elsewhere. */
    curl_easy_setopt(h, CURLOPT_USERAGENT, "BattleShip/" BATTLESHIP_CURRENT_VERSION);
    /* Abort stalled sockets instead of hanging a worker forever: no bytes
     * for 30s kills the transfer (big files still get the whole time). */
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(h, CURLOPT_LOW_SPEED_TIME, 30L);
    return h;
}

size_t WriteToString(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* s = static_cast<std::string*>(userdata);
    s->append(ptr, size * nmemb);
    return size * nmemb;
}

size_t WriteToFile(char* ptr, size_t size, size_t nmemb, void* userdata) {
    auto* f = static_cast<FILE*>(userdata);
    return std::fwrite(ptr, size, nmemb, f);
}

struct ProgressCtx {
    const CurlProgressFn* fn = nullptr;
    const std::atomic<bool>* cancel = nullptr;
};

int ProgressCb(void* clientp, curl_off_t total, curl_off_t now, curl_off_t, curl_off_t) {
    auto* ctx = static_cast<ProgressCtx*>(clientp);
    if (ctx->cancel != nullptr && ctx->cancel->load(std::memory_order_relaxed)) {
        return 1;
    }
    if (ctx->fn != nullptr && *ctx->fn) {
        return (*ctx->fn)(static_cast<unsigned long long>(now),
                          static_cast<unsigned long long>(total)) ? 0 : 1;
    }
    return 0;
}

} // namespace

bool CurlGetToString(const std::string& url, std::string* body, std::string* error,
                     const std::vector<std::string>& extraHeaders, long timeoutSeconds) {
    char errbuf[CURL_ERROR_SIZE];
    CURL* h = MakeEasy(&errbuf);
    if (h == nullptr) {
        if (error) *error = "curl init failed";
        return false;
    }
    struct curl_slist* headers = nullptr;
    for (const auto& line : extraHeaders) {
        headers = curl_slist_append(headers, line.c_str());
    }
    std::string out;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeoutSeconds);
    const CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) {
        if (error) *error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        return false;
    }
    *body = std::move(out);
    return true;
}

bool CurlPostToString(const std::string& url, const std::string& payload,
                      const std::string& contentType, std::string* body,
                      std::string* error, long timeoutSeconds) {
    char errbuf[CURL_ERROR_SIZE];
    CURL* h = MakeEasy(&errbuf);
    if (h == nullptr) {
        if (error) *error = "curl init failed";
        return false;
    }
    struct curl_slist* headers = curl_slist_append(
        nullptr, ("Content-Type: " + contentType).c_str());
    std::string out;
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_POST, 1L);
    curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.data());
    curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE, static_cast<long>(payload.size()));
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, WriteToString);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, &out);
    curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(h, CURLOPT_TIMEOUT, timeoutSeconds);
    const CURLcode rc = curl_easy_perform(h);
    curl_slist_free_all(headers);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK) {
        if (error) *error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
        return false;
    }
    if (body != nullptr) *body = std::move(out);
    return true;
}

bool CurlDownloadToFile(const std::string& url, const std::filesystem::path& destPath,
                        std::string* error, const CurlProgressFn& progress,
                        const std::atomic<bool>* cancel) {
    char errbuf[CURL_ERROR_SIZE];
    CURL* h = MakeEasy(&errbuf);
    if (h == nullptr) {
        if (error) *error = "curl init failed";
        return false;
    }
#if defined(_WIN32)
    FILE* f = _wfopen(destPath.wstring().c_str(), L"wb");
#else
    FILE* f = std::fopen(destPath.string().c_str(), "wb");
#endif
    if (f == nullptr) {
        curl_easy_cleanup(h);
        if (error) *error = "cannot open " + destPath.string() + " for writing";
        return false;
    }
    ProgressCtx ctx{ &progress, cancel };
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, WriteToFile);
    curl_easy_setopt(h, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(h, CURLOPT_XFERINFOFUNCTION, ProgressCb);
    curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ctx);
    curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);
    const CURLcode rc = curl_easy_perform(h);
    /* fwrite() buffers — a full disk can only fail here, on the final
     * flush; rc==CURLE_OK alone isn't enough to call the file good. */
    const bool flushFailed = (std::fclose(f) != 0);
    curl_easy_cleanup(h);
    if (rc != CURLE_OK || flushFailed) {
        std::error_code ec;
        std::filesystem::remove(destPath, ec);      // never leave a torn file
        if (error) {
            if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
                *error = "cancelled";
            } else if (flushFailed) {
                *error = "flush failed writing " + destPath.string();
            } else {
                *error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
            }
        }
        return false;
    }
    return true;
}

} // namespace ssb64
