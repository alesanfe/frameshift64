/* http_curl.h — shared in-process HTTP helpers on top of libcurl.
 *
 * Single place that sets the options every call site wants (redirect
 * following, non-2xx failure, timeouts, User-Agent) so Download,
 * HttpDownloadToFile and the Updater stop hand-rolling curl command
 * lines. Desktop only — callers are compiled out on Android/UWP/Switch,
 * matching the SSB64_HAVE_CURL gate in CMakeLists.txt. */
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ssb64 {

/* Progress callback for CurlDownloadToFile: (bytes so far, total if
 * known else 0). Return false to abort the transfer (treated as a
 * cancel). Called from whatever thread runs the transfer. */
using CurlProgressFn = std::function<bool(unsigned long long, unsigned long long)>;

/* Blocking GET to memory. Fills `body` on a 2xx reply; `error` gets a
 * human-readable cause otherwise. `extraHeaders` are added verbatim
 * ("Accept: ..." style). Returns false on transport/HTTP failure. */
bool CurlGetToString(const std::string& url, std::string* body, std::string* error,
                     const std::vector<std::string>& extraHeaders = {},
                     long timeoutSeconds = 30);

/* Blocking POST with a string payload. `contentType` is the bare media
 * type ("application/json"). Fills `body` (when non-null) on a 2xx
 * reply; `error` gets a human-readable cause otherwise. */
bool CurlPostToString(const std::string& url, const std::string& payload,
                      const std::string& contentType, std::string* body,
                      std::string* error, long timeoutSeconds = 30);

/* Blocking GET to file. Writes straight to `destPath`; on failure or
 * cancel the partial file is removed. `cancel` (if given) aborts the
 * transfer within ~200ms of being set; cancel counts as failure but is
 * distinguishable through CurlErrorKind::Cancelled in `error`... kept
 * simple: returns false and error says "cancelled". */
bool CurlDownloadToFile(const std::string& url, const std::filesystem::path& destPath,
                        std::string* error, const CurlProgressFn& progress = nullptr,
                        const std::atomic<bool>* cancel = nullptr);

/* ws://host[:port]/path -> http://host[:port], wss:// -> https://; any
 * path/query is stripped. A bare hostname gets https://. Empty or null
 * input stays empty (callers gate on it). Single home for what used to
 * be two near-identical helpers in netleaderboard.cpp and
 * crash_bundle.cpp — one of which forgot to strip the path. */
std::string HttpBaseFromWsServer(const char* server);

} // namespace ssb64
