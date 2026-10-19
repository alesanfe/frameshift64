#pragma once

/**
 * port_log.h — Unified crash-safe logging for the SSB64 PC port.
 *
 * All port logging goes through port_log(). Output is written to a single
 * file (ssb64.log) with immediate fflush after every write, so nothing is
 * lost on crash. Call port_log_init() once at the very start of main(),
 * before any LUS initialization (which redirects stderr).
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Open the log file. Call once, before anything else. */
void port_log_init(const char *path);

/* Close the log file. Call at shutdown. */
void port_log_close(void);

/* Return the raw file descriptor of the log file, or -1 if not open.
 * Used by async-signal-safe crash handlers that cannot call fprintf. */
int port_log_get_fd(void);

/* Write a formatted message to the log file.
 * Two extra taps exist beyond ssb64.log:
 *  - port_log_set_sink(): a process callback receiving each formatted
 *    message (fires even before port_log_init, so early boot output is
 *    capturable). The sink must not log back into port_log — re-entrant
 *    calls are dropped.
 *  - SSB64_LOG_JSON=1 env var: mirrors every message as a JSONL record
 *    ({"t":<ms-since-init>,"sid":"...","lvl":"info","cat":"...","msg":"..."})
 *    to <logpath>.jsonl, for tests and tooling that want structured events
 *    instead of grep-archaeology. Values after token=/key=/secret=/password=
 *    are redacted in the JSONL mirror, and the user home directory is
 *    collapsed to "~". */
#ifdef __GNUC__
__attribute__((format(printf, 1, 2)))
#endif
void port_log(const char *fmt, ...);

/* Structured variant of port_log: same formatted text in ssb64.log (with a
 * [WARN]/[ERROR] prefix for non-INFO), plus "lvl"/"cat" fields in the
 * JSONL mirror and the extended sink. `category` may be NULL. */
typedef enum {
	PORT_LOG_INFO = 0,
	PORT_LOG_WARN,
	PORT_LOG_ERROR
} port_log_level;
#ifdef __GNUC__
__attribute__((format(printf, 3, 4)))
#endif
void port_log_ex(port_log_level level, const char *category, const char *fmt, ...);

typedef void (*port_log_sink_fn)(const char *message, void *user);
void port_log_set_sink(port_log_sink_fn fn, void *user);

/* Extended sink receiving level + category alongside the message. A sink
 * registered with port_log_set_sink still works and sees only the text. */
typedef void (*port_log_sink_ex_fn)(port_log_level level, const char *category,
                                    const char *message, void *user);
void port_log_set_sink_ex(port_log_sink_ex_fn fn, void *user);

/* Correlation id stamped as "sid" on every JSONL record. Without an
 * explicit call, one is generated from pid+time at port_log_init. */
void port_log_set_session(const char *session_id);

/* Flush the log (and all stdio streams) and terminate the process with
 * `code` immediately: no destructors, no atexit handlers, no DLL detach
 * callbacks. For batch/rig modes that report a verdict through the exit
 * code from inside a game coroutine, where normal teardown is not safe
 * (render/audio threads are mid-frame). */
void port_exit_process(int code);

/* Emscripten: replay the ring of lines emitted from inside a coroutine
 * (fiber) context, where taking any lock is unsafe — the main-thread
 * futex wait is a synchronous busy-spin and a lock held by a suspended
 * context never releases. Fiber writes go straight to the console via a
 * raw JS import and queue here for file/sink persistence. The main
 * context drains once per frame and after each coroutine resume.
 * No-op on native builds. */
void port_log_fiber_drain(void);

#ifdef __cplusplus
}
#endif
