/* http_download.h — platform-blind "fetch URL to file" API.
 *
 * One contract for every target so feature code (ShaderDownloader)
 * never branches on the OS:
 *   - Windows / Linux / macOS: in-process libcurl (SSB64_HAVE_CURL).
 *   - Android: JNI call into BattleShipActivity.httpDownloadToFile,
 *     which streams through java.net.HttpURLConnection — Android apps
 *     cannot exec helper binaries, so the OS HTTP stack is the only
 *     conforming implementation.
 *   - UWP: stub returning false; the feature is compiled out there.
 *
 * Blocking and thread-agnostic — callers run it on their own worker
 * thread (same as the curl path it replaces). */
#pragma once

#include <filesystem>
#include <string>

namespace ssb64 {

/* GET `url` (following redirects), write the body to `destPath`, and
 * return true only when the response was a 2xx and the file was fully
 * written. On failure the partial file is removed so a torn payload
 * is never left behind. */
bool HttpDownloadToFile(const std::string& url,
                        const std::filesystem::path& destPath);

} // namespace ssb64
