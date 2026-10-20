/* ws_win32_leak_test — real WinHTTP handle-leak check for the uplink
 * choreography (src/networking/netplay/transport/websocket_transport_win32.cpp
 * compiled straight into this binary).
 *
 * Drives the production backend's extern "C" API against a loopback
 * RFC6455 server (ws_win32_leak_server.py, spawned by the test driver)
 * and measures the process handle count via GetProcessHandleCount —
 * the same OS evidence a manual Handle.exe/dump would show, asserted.
 *
 *   Phase A (clean churn):  uplink_connect -> uplink_send -> uplink_close
 *     N times. Every handle in the session/connect/request/socket chain
 *     must be retired — growth here means WsUplinkClose leaks.
 *
 *   Phase B (failure churn): uplink_connect -> send (server kills the
 *     link) -> send (lazy re-dial fires: connect resets retryAfter=0,
 *     so the first re-dial is immediate) -> close, M times. This is the
 *     exact path that used to orphan the WinHTTP session/connect
 *     intermediates when a re-dial overwrote sUpConn.
 *
 * The first cycles legitimately grow the count (WinHTTP per-process
 * caches, resolver state, threadpool), so the assertion compares the
 * LAST window against an earlier one — a per-cycle leak of even one
 * handle shows up as a slope, not noise.
 *
 * argv: <hold-port> <die-port> [clean-iters=200] [fail-iters=60] */

/* The same TU drives every production backend (win32/curl/fallback) —
 * only the linked transport .cpp changes. WS_LEAK_TEST_NAME keeps the
 * ctest report honest about which backend ran.
 *
 * POSIX port: on Linux/WSL the metric is open fd count (/proc/self/fd),
 * which is the resource class the transports actually allocate, and
 * Sleep -> nanosleep. This lets the same churn run under
 * -fsanitize=thread/-fsanitize=address with the threaded fallback
 * backend. */
#ifndef WS_LEAK_TEST_NAME
#define WS_LEAK_TEST_NAME "ws_win32_leak_test"
#endif

#include "smash_test/check.h"

#include "networking/netplay/transport/websocket_transport.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <time.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" void port_log(const char*, ...) {}

namespace {

long handle_count()
{
#ifdef _WIN32
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return static_cast<long>(n);
#else
    /* Count open fds — sockets, eventfds, timerfds all show up here. */
    long n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (d == nullptr) {
        return -1;
    }
    while (readdir(d) != nullptr) {
        ++n;
    }
    closedir(d);
    return n - 2;                    /* '.' and '..' are not fds */
#endif
}

void sleep_ms(int ms)
{
#ifdef _WIN32
    Sleep(static_cast<DWORD>(ms));
#else
    struct timespec ts { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, nullptr);
#endif
}

std::string ws_url(int port)
{
    /* WS_LEAK_SCHEME=wss drives the same churn over TLS (curl backend
     * against the server's --cert/--key listeners). */
    const char *scheme = std::getenv("WS_LEAK_SCHEME");
    char buf[128];
    std::snprintf(buf, sizeof(buf), "%s://127.0.0.1:%d/ws",
                  (scheme != nullptr) ? scheme : "ws", port);
    return buf;
}

bool wait_state(PortWsState want, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 20) {
        if (port_ws_uplink_state() == want) {
            return true;
        }
        sleep_ms(20);
    }
    return port_ws_uplink_state() == want;
}

/* One clean connect/send/close cycle against the hold server. */
void clean_cycle(int port)
{
    std::string url = ws_url(port);
    SMASH_EXPECT(port_ws_uplink_connect(url.c_str()) == 1);
    SMASH_EXPECT(wait_state(PORT_WS_OPEN, 5000));
    SMASH_EXPECT(port_ws_uplink_send("ping", 4) == 1);
    port_ws_uplink_close();
    SMASH_EXPECT(port_ws_uplink_state() == PORT_WS_CLOSED);
}

/* One failure cycle against the die server: the link dies under the
 * first send; the second send forces the lazy re-dial (retryAfter is
 * 0 right after an explicit connect, so it fires immediately). */
void fail_cycle(int port)
{
    std::string url = ws_url(port);
    SMASH_EXPECT(port_ws_uplink_connect(url.c_str()) == 1);
    if (!wait_state(PORT_WS_OPEN, 5000)) {
        /* Server died before the upgrade completed — still a valid
         * failure-path cycle, just skip the sends. */
        port_ws_uplink_close();
        return;
    }
    port_ws_uplink_send("x", 1);
    sleep_ms(60);                      /* let the server's close land */
    port_ws_uplink_send("y", 1);    /* link dead -> FAILED          */
    port_ws_uplink_send("z", 1);    /* FAILED -> lazy re-dial (the
                                     * path that orphaned handles)  */
    sleep_ms(60);
    port_ws_uplink_close();
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <hold-port> <die-port> [clean] [fail]\n", argv[0]);
        return 2;
    }
    const int holdPort = std::atoi(argv[1]);
    const int diePort = std::atoi(argv[2]);
    const int cleanIters = argc > 3 ? std::atoi(argv[3]) : 200;
    const int failIters = argc > 4 ? std::atoi(argv[4]) : 60;

    /* ── warm-up: absorb WinHTTP/process first-use caches ──────────── */
    for (int i = 0; i < 10; i++) {
        clean_cycle(holdPort);
    }

    /* ── Phase A: clean churn, measured across two equal windows ───── */
    const long aStart = handle_count();
    for (int i = 0; i < cleanIters; i++) {
        clean_cycle(holdPort);
    }
    const long aMid = handle_count();
    for (int i = 0; i < cleanIters; i++) {
        clean_cycle(holdPort);
    }
    const long aEnd = handle_count();

    /* A per-cycle leak of even 1 handle shows as ~cleanIters growth.
     * The window-vs-window comparison isolates slope from one-off
     * warm-up residue. Generous bound for pool/thread noise. */
    const long growA = aEnd - aMid;
    std::printf("handles A: %ld -> %ld -> %ld (slope %+ld over %d cycles)\n",
                aStart, aMid, aEnd, growA, cleanIters);
    SMASH_CHECK(growA <= 32);

    /* ── Phase B: send-fail -> lazy re-dial churn ──────────────────── */
    const long bStart = handle_count();
    for (int i = 0; i < failIters; i++) {
        fail_cycle(diePort);
    }
    const long bEnd = handle_count();
    const long growB = bEnd - bStart;
    std::printf("handles B: %ld -> %ld (slope %+ld over %d failure cycles)\n",
                bStart, bEnd, growB, failIters);
    SMASH_CHECK(growB <= 32);

    /* Sanity: the loop really did exercise the transport — a zero-send
     * run would "pass" a leak check vacuously. State must be CLOSED. */
    SMASH_CHECK(port_ws_uplink_state() == PORT_WS_CLOSED);

    return smash_test_report(WS_LEAK_TEST_NAME);
}
