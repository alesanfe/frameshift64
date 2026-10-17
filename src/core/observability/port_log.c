#include "port_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__SWITCH__)
#include <switch.h>  /* Mutex */
#else
#include <unistd.h>
#include <pthread.h>
#endif

static FILE *sLogFile = NULL;

/* Structured mirror (TD-011): when SSB64_LOG_JSON is set, every message is
 * also written as {"t":<ms>,"msg":"..."} to <logpath>.jsonl. */
static FILE *sJsonFile = NULL;
static unsigned long long sInitMs = 0;

/* Optional process-wide sinks (TD-011 LogSink seam). */
static port_log_sink_fn sSink = NULL;
static void *sSinkUser = NULL;
static port_log_sink_ex_fn sSinkEx = NULL;
static void *sSinkExUser = NULL;

/* Session correlation id for the JSONL mirror. */
static char sSessionId[24] = {0};
#if defined(_MSC_VER)
static __declspec(thread) int sInSink = 0;
#else
static _Thread_local int sInSink = 0;
#endif

static unsigned long long log_now_ms(void)
{
#ifdef _WIN32
	return (unsigned long long)GetTickCount64();
#elif defined(__SWITCH__)
	return 0;  /* no cheap monotonic clock on libnx pre-init; t=0 */
#else
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
	return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
#endif
}

/* Escape a line into a JSON string (with quotes). Caller ensures cap. */
static void json_escape(char *dst, size_t cap, const char *src)
{
	static const char kHex[] = "0123456789abcdef";
	size_t o = 0;
	if (cap == 0) return;
	dst[o++] = '"';
	for (const unsigned char *p = (const unsigned char *)src; *p && o + 8 < cap; p++) {
		unsigned char c = *p;
		if (c == '"' || c == '\\') { dst[o++] = '\\'; dst[o++] = (char)c; }
		else if (c == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
		else if (c == '\r') { dst[o++] = '\\'; dst[o++] = 'r'; }
		else if (c == '\t') { dst[o++] = '\\'; dst[o++] = 't'; }
		else if (c < 0x20) {
			dst[o++] = '\\'; dst[o++] = 'u'; dst[o++] = '0'; dst[o++] = '0';
			dst[o++] = kHex[c >> 4]; dst[o++] = kHex[c & 0xF];
		}
		else dst[o++] = (char)c;
	}
	dst[o++] = '"';
	dst[o] = '\0';
}

/* Redact secrets/home paths before a message reaches the JSONL mirror.
 * Logs travel to crash bundles and bug reports; a token pasted into a
 * lobby command or a path under the user's profile shouldn't ship with
 * them. Operates in place on the plain-text copy. */
static void json_redact(char *s)
{
	static const char *const kKeys[] = {
		"token=", "key=", "secret=", "password=", "api_key=", "auth="
	};
	const char *home = getenv(
#ifdef _WIN32
		"USERPROFILE"
#else
		"HOME"
#endif
	);
	for (size_t k = 0; k < sizeof(kKeys) / sizeof(kKeys[0]); k++) {
		const size_t klen = strlen(kKeys[k]);
		for (char *p = s; (p = strstr(p, kKeys[k])) != NULL; p += klen + 3) {
			char *v = p + klen;
			/* Replace the value run (non-space, non-quote) with "***". */
			size_t vl = 0;
			while (v[vl] && v[vl] != ' ' && v[vl] != '"' && v[vl] != '\t' &&
			       v[vl] != '\n' && v[vl] != ',') vl++;
			if (vl == 0) continue;
			memmove(v + 3, v + vl, strlen(v + vl) + 1);
			memcpy(v, "***", 3);
		}
	}
	if (home != NULL && home[0] != '\0') {
		const size_t hl = strlen(home);
		for (char *p = s; (p = strstr(p, home)) != NULL; p += 1) {
			memmove(p + 1, p + hl, strlen(p + hl) + 1);
			p[0] = '~';
		}
	}
}

static const char *level_name(port_log_level level)
{
	switch (level) {
	case PORT_LOG_WARN:  return "warn";
	case PORT_LOG_ERROR: return "error";
	default:             return "info";
	}
}

/* sLogFile is touched by every thread that logs and by the atexit /
 * shutdown path. Without a lock, port_log_close can fclose() the FILE*
 * while another thread sits inside vfprintf on it (use-after-free), and
 * two writers can interleave bytes mid-line. No dependency on C11
 * <stdatomic.h> — MSVC support is too recent to assume. */
#ifdef _WIN32
static volatile LONG sLogLock;
#define LOG_LOCK()   do { while (InterlockedExchange(&sLogLock, 1) != 0) Sleep(0); } while (0)
#define LOG_UNLOCK() (void)InterlockedExchange(&sLogLock, 0)
#elif defined(__SWITCH__)
static Mutex sLogMutex;   /* zero-init == unlocked for libnx Mutex */
#define LOG_LOCK()   mutexLock(&sLogMutex)
#define LOG_UNLOCK() mutexUnlock(&sLogMutex)
#else
static pthread_mutex_t sLogMutex = PTHREAD_MUTEX_INITIALIZER;
#define LOG_LOCK()   (void)pthread_mutex_lock(&sLogMutex)
#define LOG_UNLOCK() (void)pthread_mutex_unlock(&sLogMutex)
#endif

#ifdef __SWITCH__
/* On Switch, SD-card fflush is slow (~5-20ms). Buffer in RAM and only
 * flush when the buffer is nearly full or on close. */
static char sLogBuffer[65536];
static size_t sLogBufUsed = 0;
#endif

void port_log_init(const char *path)
{
	LOG_LOCK();
	if (sLogFile == NULL) {
		sLogFile = fopen(path, "w");
		sInitMs = log_now_ms();
		if (sSessionId[0] == '\0') {
			/* Correlation id for the JSONL mirror — pid+start time is
			 * enough; it only needs to distinguish runs. */
			snprintf(sSessionId, sizeof(sSessionId), "%llx-%llx",
			         (unsigned long long)sInitMs,
#ifdef _WIN32
			         (unsigned long long)GetCurrentProcessId()
#elif defined(__SWITCH__)
			         0ull
#else
			         (unsigned long long)getpid()
#endif
			);
		}
		{
			const char *json = getenv("SSB64_LOG_JSON");
			if (json != NULL && json[0] != '\0' && json[0] != '0') {
				char jpath[1100];
				int n = snprintf(jpath, sizeof(jpath), "%s.jsonl", path);
				if (n > 0 && (size_t)n < sizeof(jpath)) {
					sJsonFile = fopen(jpath, "w");
				}
			}
		}
	}
	LOG_UNLOCK();
}

void port_log_close(void)
{
	LOG_LOCK();
	if (sLogFile != NULL) {
#ifdef __SWITCH__
		if (sLogBufUsed > 0) {
			fwrite(sLogBuffer, 1, sLogBufUsed, sLogFile);
			sLogBufUsed = 0;
		}
#endif
		fclose(sLogFile);
		sLogFile = NULL;
	}
	if (sJsonFile != NULL) {
		fclose(sJsonFile);
		sJsonFile = NULL;
	}
	LOG_UNLOCK();
}

int port_log_get_fd(void)
{
	int fd = -1;
	LOG_LOCK();
	if (sLogFile != NULL) {
		fd = fileno(sLogFile);
	}
	LOG_UNLOCK();
	return fd;
}

void port_log_write(port_log_level level, const char *category, const char *msg, int n);

void port_log(const char *fmt, ...)
{
	char msg[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (n > 0) {
		port_log_write(PORT_LOG_INFO, NULL, msg, n);
	}
}

void port_log_ex(port_log_level level, const char *category, const char *fmt, ...)
{
	char msg[4096];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	if (n > 0) {
		port_log_write(level, category, msg, n);
	}
}

void port_log_write(port_log_level level, const char *category, const char *msg, int n)
{
	/* Format once, feed three taps: plain log, JSONL mirror, sinks.
	 * Non-INFO records carry a textual [level/cat] prefix in the plain
	 * log so grep still finds them without the JSONL mirror. */
	size_t len = ((size_t)n < 4096) ? (size_t)n : 4095;
	char pbuf[4300];
	const char *out = msg;
	size_t olen = len;
	if (level != PORT_LOG_INFO || (category != NULL && category[0])) {
		int pn = snprintf(pbuf, sizeof(pbuf), "[%s%s%s] %.*s",
		                  level_name(level),
		                  (category != NULL && category[0] != '\0') ? "/" : "",
		                  (category != NULL && category[0] != '\0') ? category : "",
		                  (int)len, msg);
		if (pn > 0) {
			out = pbuf;
			olen = ((size_t)pn < sizeof(pbuf)) ? (size_t)pn : sizeof(pbuf) - 1;
		}
	}

	LOG_LOCK();
	FILE *f = sLogFile;
	if (f != NULL) {
#ifdef __SWITCH__
		/* vsnprintf returns the untruncated length — clamp so a too-long
		 * line fills the buffer instead of overflowing the index. */
		size_t remaining = sizeof(sLogBuffer) - sLogBufUsed;
		size_t copy = (olen < remaining) ? olen : remaining;
		memcpy(sLogBuffer + sLogBufUsed, out, copy);
		sLogBufUsed += copy;
		if (sLogBufUsed >= sizeof(sLogBuffer) * 3 / 4) {
			fwrite(sLogBuffer, 1, sLogBufUsed, f);
			sLogBufUsed = 0;
		}
#else
		fwrite(out, 1, olen, f);
		/* Header contract: every write is durable so crash-time reports
		 * (vectored handler, crash filter) survive process death. stdio
		 * buffering would otherwise silently drop them — observed: the
		 * exception section vanished mid-line on a heap-corruption kill. */
		fflush(f);
#endif
	}
	if (sJsonFile != NULL) {
		char jline[4700];
		char esc[4100];
		char jmsg[4096];
		/* Redact on a scratch copy — the plain log keeps the full text. */
		size_t jml = (len < sizeof(jmsg)) ? len : sizeof(jmsg) - 1;
		memcpy(jmsg, msg, jml);
		jmsg[jml] = '\0';
		json_redact(jmsg);
		json_escape(esc, sizeof(esc), jmsg);
		char cesc[160];
		json_escape(cesc, sizeof(cesc), (category != NULL) ? category : "");
		int jn = snprintf(jline, sizeof(jline),
		                  "{\"t\":%llu,\"sid\":\"%s\",\"lvl\":\"%s\",\"cat\":%s,\"msg\":%s}\n",
		                  log_now_ms() - sInitMs, sSessionId,
		                  level_name(level), cesc, esc);
		if (jn > 0) {
			fwrite(jline, 1, ((size_t)jn < sizeof(jline)) ? (size_t)jn : sizeof(jline) - 1, sJsonFile);
		}
	}
	LOG_UNLOCK();
	/* fflush on every call costs seconds per frame on a slow drive when
	 * figatree watchdogs fire 28x per frame during a stuck APPEAR. Rely on
	 * stdio's buffer + OS-on-exit flush for normal logging; crash dumps
	 * have their own flush path. */

	/* Sinks run outside the lock (they may be slow) but must not recurse —
	 * a sink that calls port_log would re-enter forever. */
	port_log_sink_fn sink = sSink;
	port_log_sink_ex_fn sink_ex = sSinkEx;
	if ((sink != NULL || sink_ex != NULL) && !sInSink) {
		sInSink = 1;
		if (sink != NULL) sink(msg, sSinkUser);
		if (sink_ex != NULL) sink_ex(level, category, msg, sSinkExUser);
		sInSink = 0;
	}
}

void port_log_set_sink(port_log_sink_fn fn, void *user)
{
	LOG_LOCK();
	sSink = fn;
	sSinkUser = user;
	LOG_UNLOCK();
}

void port_log_set_sink_ex(port_log_sink_ex_fn fn, void *user)
{
	LOG_LOCK();
	sSinkEx = fn;
	sSinkExUser = user;
	LOG_UNLOCK();
}

void port_log_set_session(const char *session_id)
{
	LOG_LOCK();
	snprintf(sSessionId, sizeof(sSessionId), "%s",
	         (session_id != NULL) ? session_id : "");
	LOG_UNLOCK();
}

void port_exit_process(int code)
{
	LOG_LOCK();
	if (sLogFile != NULL) fflush(sLogFile);
	LOG_UNLOCK();
	fflush(NULL);
#ifdef _WIN32
	/* Not _exit()/ExitProcess(): those kill the other threads and then run
	 * every DLL's DLL_PROCESS_DETACH on this thread, which fail-fasts when a
	 * killed thread held one of their locks (seen as STATUS_FAIL_FAST_EXCEPTION
	 * at VS scene teardown). TerminateProcess skips all of that. */
	TerminateProcess(GetCurrentProcess(), (UINT)code);
	/* Only reached if TerminateProcess itself failed; never return into the game. */
	_exit(code);
#elif defined(__SWITCH__)
	/* Immediate kernel exit — no atexit/unwind, same contract as the other
	 * platforms' paths here. Declared via <switch.h> above. */
	svcExitProcess();
#else
	_exit(code);
#endif
}
