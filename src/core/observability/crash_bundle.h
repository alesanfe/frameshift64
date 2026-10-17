#pragma once

/**
 * crash_bundle.h — cross-platform crash-bundle collection (TD-015).
 *
 * Two halves:
 *
 *  1. Crash time: the fatal-signal/SEH handlers call
 *     port_diag_write_crash_marker(), which drops a tiny `crash.marker`
 *     file next to the log. On POSIX it uses only open(2)/write(2)/
 *     close(2) — all on the async-signal-safe list — so it can run inside
 *     a signal handler without adding re-fault risk.
 *
 *  2. Next boot: CollectPendingCrashBundle() runs BEFORE port_log_init()
 *     truncates the log. If a marker exists it assembles
 *     <logdir>/crashes/crash-<utc>-p<pid>/ containing crash_info.txt
 *     (marker fields + build version/sha + scene), the previous run's
 *     ssb64.log (which holds the handler's backtrace), and any stray
 *     crash_*.dmp minidumps on Windows. Old bundles rotate (newest 8).
 *
 * Privacy: bundles contain local filesystem paths (log lines, module
 * paths) and are meant to be attached to a bug report by the user —
 * nothing is ever uploaded automatically. Settings/saves are not copied
 * (they carry no diagnostic value a log doesn't).
 */

#include <cstdint>
#include <string>

namespace ssb64::diag {

/* Returns the bundle directory created, or "" if no crash was pending.
 * Never throws. */
std::string CollectPendingCrashBundle(const std::string &log_path);

} // namespace ssb64::diag

#ifdef __cplusplus
extern "C" {
#endif

/* Record where crash markers go. Call once after the log path is known
 * (right after port_log_init). Copies `dir`; safe to pass nullptr. */
void port_diag_set_crash_dir(const char *dir);

/* Write <crashdir>/crash.marker describing the fatal event. POSIX callers
 * may invoke this from a signal handler (async-signal-safe syscalls only).
 * `reason` is a short tag: "SIGSEGV", "unhandled", "signal", ... */
void port_diag_write_crash_marker(const char *reason, uintptr_t fault_addr);

/* Safe mode: when active, the port layer skips risky startup content
 * (scripted mods, hi-res texture packs) without touching persisted CVars.
 * Set at boot when CollectPendingCrashBundle() returns a bundle or the
 * SSB64_SAFE_MODE env var is set; read by startup gates and the menu UI. */
void port_diag_set_safe_mode(int active);
int port_diag_safe_mode_active(void);

/* One-shot: write safemode.request beside crash.marker so the NEXT launch
 * boots in safe mode (consumed at startup — the user asked for it, so a
 * crash bundle isn't required). */
void port_diag_request_safe_mode(void);

#ifdef __cplusplus
}
#endif
