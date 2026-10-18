/* download_service.h — cancellable background file download, same
 * contract on every platform.
 *
 *   - Windows / Linux / macOS: an in-process libcurl transfer on a
 *     worker thread; cancel flips a flag the progress callback polls.
 *   - Android: a Java-side worker thread streaming through
 *     HttpURLConnection; cancel disconnects the connection. Apps cannot
 *     exec helper binaries, so the platform HTTP stack is the backend.
 *   - UWP: stub — the features using this are compiled out there.
 *
 * Progress is read by polling the destination file's size from the
 * caller (both backends stream to disk incrementally), so the service
 * itself stays transport-only. Download to a `.part` path and rename
 * on success — a torn payload is never left under the final name. */
#pragma once

#include <filesystem>
#include <string>

namespace ssb64 {

/* Opaque in-flight download. nullptr means "failed to start". */
struct Download;

enum class DownloadState {
    Running,
    Done,
    Error,
    Cancelled,
};

/* Begin downloading `url` to `destPath` in the background, following
 * redirects. Returns nullptr if the transfer could not even be
 * started (no backend, spawn failure). */
Download* DownloadStart(const std::string& url,
                        const std::filesystem::path& destPath);

/* Non-blocking state query. */
DownloadState DownloadPoll(const Download* d);

/* Request cancellation — idempotent, safe on an already-finished
 * transfer. The blocked worker unblocks promptly. */
void DownloadCancel(Download* d);

/* Join the transfer and release the handle. Returns the terminal
 * state (Done on success). Must be called exactly once on every
 * non-null handle, including after DownloadCancel. */
DownloadState DownloadFinish(Download* d);

} // namespace ssb64
