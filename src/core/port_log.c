#include "port_log.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
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

void port_log(const char *fmt, ...)
{
	va_list ap;
	LOG_LOCK();
	FILE *f = sLogFile;
	if (f == NULL) {
		LOG_UNLOCK();
		return;
	}
	va_start(ap, fmt);
#ifdef __SWITCH__
	int n = vsnprintf(sLogBuffer + sLogBufUsed,
	                  sizeof(sLogBuffer) - sLogBufUsed, fmt, ap);
	if (n > 0) {
		/* vsnprintf returns the untruncated length — clamp so a too-long
		 * line fills the buffer instead of overflowing the index. */
		size_t remaining = sizeof(sLogBuffer) - sLogBufUsed;
		sLogBufUsed += ((size_t)n < remaining) ? (size_t)n : remaining;
		if (sLogBufUsed >= sizeof(sLogBuffer) * 3 / 4) {
			fwrite(sLogBuffer, 1, sLogBufUsed, f);
			sLogBufUsed = 0;
		}
	}
#else
	vfprintf(f, fmt, ap);
#endif
	va_end(ap);
	#if defined(BATTLESHIP_UWP)
	/* Xbox Device Portal may terminate a failed UWP process before stdio's
	 * normal-exit flush runs. Keep startup diagnostics durable so failures
	 * can be diagnosed from LocalState without attaching a debugger. */
	fflush(f);
	#endif
	LOG_UNLOCK();
	/* fflush on every call costs seconds per frame on a slow drive when
	 * figatree watchdogs fire 28x per frame during a stuck APPEAR. Rely on
	 * stdio's buffer + OS-on-exit flush for normal logging; crash dumps
	 * have their own flush path. */
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
#else
	_exit(code);
#endif
}
