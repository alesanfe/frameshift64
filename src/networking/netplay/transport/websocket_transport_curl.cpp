/* See websocket_transport.h - libcurl WebSocket backend: covers ws:// and
 * wss:// on every desktop OS (the default lobby is wss://). Empty TU when the
 * WinHTTP backend owns the platform or the curl WS feature is absent. */
#if defined(SSB64_HAVE_CURL_WS) && (!defined(_WIN32) || defined(SSB64_PORTABLE_WS))

#include "websocket_transport.h"
#include "websocket_transport_internal.h"
#include "port_log.h"
#include "base64.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>


/* Portable client over libcurl's WebSocket API (CONNECT_ONLY mode):
 * curl does the HTTP upgrade + TLS, then curl_ws_send/curl_ws_recv move
 * whole frames. Same threading model as the WinHTTP path — the game
 * thread only touches the queues. Covers ws:// and wss:// on every
 * desktop OS; the default lobby is wss://, so this is what makes
 * netplay work outside Windows at all. */

#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#else
#include <fcntl.h>
#endif

#include <curl/curl.h>
#include <curl/websockets.h>

namespace {

std::mutex sMutex;
std::atomic<int> sState{ PORT_WS_CLOSED };
std::string sError;
std::deque<std::string> sTextQueue;
std::deque<std::vector<unsigned char>> sBinaryQueue;
/* Handle lifetime: created by the worker in run(), released ONLY by
 * port_ws_close after joining the worker — so a send can never touch a
 * freed handle. sCurl is read/written under sIoMutex (sends, cleanup)
 * and assigned by run() before it flips the state OPEN. */
CURL* sCurl = nullptr;
std::thread sThread;
std::atomic<bool> sStop{ false };
/* Serializes every curl_ws_* call on sCurl — the easy handle is not
 * thread-safe and recv runs on the worker while sends come from the
 * game thread. */
std::mutex sIoMutex;

/* Uplink bookkeeping — the locking choreography is shared
 * (websocket_transport_internal.h); this TU keeps only the easy handle. */
WsUplink sUp;
CURL* sUpCurl = nullptr;

void fail(const std::string &why)
{
    WsFail(sMutex, sError, sState, why);
}

/* Whole-message send under the caller's per-handle IO mutex; retries
 * CURLE_AGAIN (non-blocking socket) with a short sleep. */
int send_frame_curl(CURL* c, const void* data, size_t size, unsigned int flags)
{
    size_t sent = 0;
    for (int tries = 0; tries < 2000; ++tries) {
        size_t n = 0;
        const CURLcode rc = curl_ws_send(c, static_cast<const char*>(data) + sent,
                                         size - sent, &n, 0, flags);
        if (rc == CURLE_OK) {
            sent += n;
            if (sent == size) {
                return 1;
            }
            flags = CURLWS_CONT;    /* remainder goes out as continuation */
            continue;
        }
        if (rc != CURLE_AGAIN) {
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return 0;
}

CURL* ws_dial(const std::string &url, std::string *err)
{
    CURL* h = curl_easy_init();
    if (h == nullptr) {
        *err = "curl init failed";
        return nullptr;
    }
    curl_easy_setopt(h, CURLOPT_URL, url.c_str());
    /* CONNECT_ONLY=2: perform the upgrade, then hand us the WebSocket. */
    curl_easy_setopt(h, CURLOPT_CONNECT_ONLY, 2L);
    curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, 10000L);
    curl_easy_setopt(h, CURLOPT_USERAGENT, "BattleShip/" BATTLESHIP_CURRENT_VERSION);
    const CURLcode rc = curl_easy_perform(h);
    if (rc != CURLE_OK) {
        if (rc == CURLE_NOT_BUILT_IN) {
            /* The version probe only checks headers — a curl compiled
             * WITHOUT websockets still links curl_ws_* but returns
             * CURLE_NOT_BUILT_IN here. Say so plainly instead of the
             * generic strerror text. */
            *err = "libcurl was built without WebSocket support "
                   "(websockets feature off) — reinstall curl[websockets] "
                   "or build without SSB64_HAVE_CURL_WS";
        } else {
            *err = std::string("connect failed: ") + curl_easy_strerror(rc);
        }
        curl_easy_cleanup(h);
        return nullptr;
    }
    /* Put the active socket non-blocking so curl_ws_recv returns
     * CURLE_AGAIN when idle — that is what lets the worker notice
     * sStop between frames instead of hanging in recv. */
    curl_socket_t sock = CURL_SOCKET_BAD;
    curl_easy_getinfo(h, CURLINFO_ACTIVESOCKET, &sock);
    if (sock != CURL_SOCKET_BAD) {
#if defined(_WIN32)
        u_long one = 1;
        ioctlsocket(sock, FIONBIO, &one);
#else
        fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
#endif
    }
    return h;
}

/* Handles one curl_ws_recv chunk. Returns false when the loop should break. */
static bool run_dispatch_chunk(CURL *c, const curl_ws_frame *meta,
                               const unsigned char *buf, size_t nread,
                               std::vector<unsigned char> &message,
                               int *message_kind)
{
    (void)c;
    const unsigned int flags = meta != nullptr ? meta->flags : 0u;
    if ((flags & CURLWS_CLOSE) != 0) {
        port_log("SSB64 WS: close frame received\n");
        if (!sStop) { fail("server closed the connection"); }
        return false;
    }
    /* Control frames need no action: curl already queued an auto-PONG
     * for PINGs while collecting (ws_client_collect) and PONGs are
     * unsolicited keepalive answers. Their payload never reaches us. */
    if ((flags & (CURLWS_PING | CURLWS_PONG)) != 0) {
        return true;
    }
    /* Every data chunk carries its message type (curl keeps it through
     * CONT frames), so only start a new message when none is open —
     * clearing on every chunk would keep just the last fragment. */
    if (*message_kind < 0) {
        if ((flags & CURLWS_TEXT) != 0) {
            *message_kind = 0x1;
        } else if ((flags & CURLWS_BINARY) != 0) {
            *message_kind = 0x2;
        } else {
            return true;   /* stray chunk with no type: drop */
        }
        message.clear();
    }
    message.insert(message.end(), buf, buf + nread);
    if (message.size() > kMaxWsMessageBytes) {
        fail("server sent an oversized message");
        return false;
    }
    /* bytesleft drains per frame; the message ends only when the LAST
     * frame's payload is consumed, which curl marks by clearing CONT. */
    const bool done = (meta == nullptr || meta->bytesleft == 0) &&
                      (flags & CURLWS_CONT) == 0;
    if (done) {
        if (!WsEnqueueBounded(sMutex, sTextQueue, sBinaryQueue,
                              message, *message_kind == 0x1)) {
            fail("message queue overflow");
            return false;
        }
        message.clear();
        *message_kind = -1;
    }
    return true;
}

void run(std::string url)
{
    std::string err;
    CURL* c = ws_dial(url, &err);
    if (c == nullptr) {
        fail(err);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sCurl = c;
    }
    sState = PORT_WS_OPEN;

    /* Reassembly: curl_ws_recv delivers frame payload chunks; meta->flags
     * marks TEXT/BINARY/CONT/CLOSE/PING/PONG and meta->bytesleft tells us
     * when a fragmented message finished. */
    unsigned char buf[65536];
    std::vector<unsigned char> message;
    int message_kind = -1;
    while (!sStop) {
        size_t nread = 0;
        const curl_ws_frame* meta = nullptr;
        CURLcode rc;
        {
            std::lock_guard<std::mutex> ioLock(sIoMutex);
            rc = curl_ws_recv(c, buf, sizeof(buf), &nread, &meta);
        }
        if (rc == CURLE_AGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }
        if (rc != CURLE_OK) {
            if (!sStop) { fail("connection to the server was lost"); }
            break;
        }
        if (!run_dispatch_chunk(c, meta, buf, nread, message, &message_kind)) {
            break;
        }
    }
    /* The handle survives the loop on purpose — port_ws_close joins this
     * thread and then cleans it up under sIoMutex, so no in-flight send
     * can race the free. */
}



/* Returns a live uplink handle: the open one, or a fresh re-dial of the
 * remembered URL. The locking choreography lives in WsUplinkSocket —
 * nullptr -> caller falls back to the control socket. */
CURL* uplink_socket(void)
{
    return WsUplinkSocket(sUp, WsSteadyMs(), static_cast<CURL*>(nullptr),
        sUpCurl,
        [](const std::string &url) {
            std::string err;
            return ws_dial(url, &err);
        },
        [](CURL* c) { curl_easy_cleanup(c); });
}

/* Send on the dedicated uplink, re-dialing the remembered URL at a bounded
 * cadence when the previous socket died. Returns 1 on a successful uplink
 * send, -1 when the uplink is unusable and the caller should fall back to
 * the control socket - the frame is never dropped just because the
 * uplink died. */
int uplink_send(const void *data, size_t size, unsigned int flags)
{
    return WsUplinkSend(sUp, sUpCurl, uplink_socket(),
        static_cast<CURL*>(nullptr),
        [&](CURL* c) { return send_frame_curl(c, data, size, flags); },
        [](CURL* c) { curl_easy_cleanup(c); });
}

} // namespace

extern "C" int port_ws_connect(const char *url)
{
    port_ws_close();
    port_ws_uplink_close();
    sStop = false;
    WsClearIngress(sMutex, sError, sTextQueue, sBinaryQueue);
    sState = PORT_WS_CONNECTING;
    sThread = std::thread(run, std::string(url != nullptr ? url : ""));
    return 1;
}

extern "C" void port_ws_close(void)
{
    sStop = true;
    if (sThread.joinable()) {
        sThread.join();
    }
    {
        std::lock_guard<std::mutex> ioLock(sIoMutex);
        if (sCurl != nullptr) {
            curl_easy_cleanup(sCurl);
            sCurl = nullptr;
        }
    }
    sState = PORT_WS_CLOSED;
}

extern "C" int port_ws_state(void) { return sState; }

extern "C" const char *port_ws_error(void)
{
    static std::string copy;
    std::lock_guard<std::mutex> lock(sMutex);
    copy = sError;
    return copy.c_str();
}

extern "C" int port_ws_send_text(const char *text)
{
    /* uplink first (control sends stall behind its pending receive); a
     * dead uplink falls back to the control socket rather than dropping */
    return WsSend(sState, text, static_cast<int>(std::strlen(text)), CURLWS_TEXT,
        [](const void *d, int n, unsigned char f) {
            return uplink_send(d, static_cast<size_t>(n), f);
        },
        [](const void *d, int n, unsigned char f) {
            std::lock_guard<std::mutex> ioLock(sIoMutex);
            if (sCurl == nullptr) {
                return 0;
            }
            return send_frame_curl(sCurl, d, static_cast<size_t>(n), f);
        });
}

extern "C" int port_ws_send_binary(const void *data, int size)
{
    return WsSend(sState, data, size, CURLWS_BINARY,
        [](const void *d, int n, unsigned char f) {
            return uplink_send(d, static_cast<size_t>(n), f);
        },
        [](const void *d, int n, unsigned char f) {
            std::lock_guard<std::mutex> ioLock(sIoMutex);
            if (sCurl == nullptr) {
                return 0;
            }
            return send_frame_curl(sCurl, d, static_cast<size_t>(n), f);
        });
}

extern "C" int port_ws_poll_text(char *out, int capacity)
{
    return WsPollText(sMutex, sTextQueue, out, capacity);
}

extern "C" int port_ws_poll_binary(void *out, int capacity)
{
    return WsPollBinary(sMutex, sBinaryQueue, out, capacity);
}

extern "C" int port_ws_uplink_connect(const char *url)
{
    port_ws_uplink_close();
    return WsUplinkConnect(sUp, sUpCurl, static_cast<CURL*>(nullptr), url,
        [](const std::string &u, std::string &err) {
            return ws_dial(u, &err);
        },
        [](CURL* c) { curl_easy_cleanup(c); });
}

extern "C" void port_ws_uplink_close(void)
{
    /* Explicit close clears the remembered URL — never resurrect; the
     * graceful close runs under ioMu to wait out an in-flight send. */
    WsUplinkClose(sUp, sUpCurl, static_cast<CURL*>(nullptr),
        [] {},
        [](CURL* c) {
            send_frame_curl(c, "", 0, CURLWS_CLOSE);
            curl_easy_cleanup(c);
        });
}

extern "C" int port_ws_uplink_state(void) { return sUp.state; }

extern "C" int port_ws_uplink_send(const void *data, int size)
{
    /* Uplink-only API — no control-socket fallback here. */
    return uplink_send(data, static_cast<size_t>(size), CURLWS_BINARY) > 0 ? 1 : 0;
}

#endif
