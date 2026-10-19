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

std::mutex sUpMutex;
std::mutex sUpIoMutex;
std::atomic<int> sUpState{ PORT_WS_CLOSED };
CURL* sUpCurl = nullptr;
/* Lazy-reconnect state: the uplink is send-only, so a dead TCP shows up
 * only as send failures — without a re-dial a relayed peer went mute
 * until the match timed out. The last URL is remembered and re-dialed
 * inside send at a bounded cadence. */
std::string sUpUrl;
std::chrono::steady_clock::time_point sUpRetryAfter{};
constexpr auto kUpRedialMs = std::chrono::milliseconds(2000);

void fail(const std::string &why)
{
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sError = why;
    }
    sState = PORT_WS_FAILED;
    port_log("SSB64 WS: %s\n", why.c_str());
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
        const unsigned int flags = meta != nullptr ? meta->flags : 0u;
        if ((flags & CURLWS_CLOSE) != 0) {
            port_log("SSB64 WS: close frame received\n");
            if (!sStop) { fail("server closed the connection"); }
            break;
        }
        /* Control frames need no action: curl already queued an auto-PONG
         * for PINGs while collecting (ws_client_collect) and PONGs are
         * unsolicited keepalive answers. Their payload never reaches us. */
        if ((flags & (CURLWS_PING | CURLWS_PONG)) != 0) {
            continue;
        }
        /* Every data chunk carries its message type (curl keeps it through
         * CONT frames), so only start a new message when none is open —
         * clearing on every chunk would keep just the last fragment. */
        if (message_kind < 0) {
            if ((flags & CURLWS_TEXT) != 0) {
                message_kind = 0x1;
            } else if ((flags & CURLWS_BINARY) != 0) {
                message_kind = 0x2;
            } else {
                continue;   /* stray chunk with no type: drop */
            }
            message.clear();
        }
        message.insert(message.end(), buf, buf + nread);
        if (message.size() > kMaxWsMessageBytes) {
            fail("server sent an oversized message");
            break;
        }
        /* bytesleft drains per frame; the message ends only when the LAST
         * frame's payload is consumed, which curl marks by clearing CONT. */
        const bool done = (meta == nullptr || meta->bytesleft == 0) &&
                          (flags & CURLWS_CONT) == 0;
        if (done) {
            bool overflow = false;
            {
                std::lock_guard<std::mutex> lock(sMutex);
                overflow = sTextQueue.size() + sBinaryQueue.size() >= kMaxWsQueuedMessages;
                if (!overflow) {
                    if (message_kind == 0x1) {
                        sTextQueue.emplace_back(message.begin(), message.end());
                    } else {
                        sBinaryQueue.push_back(message);
                    }
                }
            }
            if (overflow) {
                fail("message queue overflow");
                break;
            }
            message.clear();
            message_kind = -1;
        }
    }
    /* The handle survives the loop on purpose — port_ws_close joins this
     * thread and then cleans it up under sIoMutex, so no in-flight send
     * can race the free. */
}

/* Send on the dedicated uplink, re-dialing the remembered URL at a bounded
 * cadence when the link is dead. Returns 1 when the frame went out on the
 * uplink, -1 when the uplink is unusable and the caller should fall back
 * to the control socket — the frame is never dropped just because the
 * uplink died. sUpMutex is held across the whole dial+send: it bounds the
 * serialization to one uplink at a time and gives close() a single wait
 * point; the dial is bounded by CURLOPT_TIMEOUT_MS (10s). */
/* Re-dial the remembered URL when the uplink is FAILED and the backoff
 * window elapsed - a single claimer per window (sUpRetryAfter), and
 * CONNECTING is excluded so we never overlap an explicit
 * port_ws_uplink_connect dial. Caller holds sUpMutex. Returns true when
 * sUpCurl is a live socket. */
bool uplink_ensure_curl(void)
{
    if (sUpState == PORT_WS_OPEN && sUpCurl != nullptr) {
        return true;
    }
    const auto now = std::chrono::steady_clock::now();
    if (sUpState != PORT_WS_FAILED || sUpUrl.empty() || now < sUpRetryAfter) {
        return false;
    }
    sUpRetryAfter = now + kUpRedialMs;
    std::string err;
    std::string url = sUpUrl;
    CURL* c = ws_dial(url, &err);
    if (c == nullptr) {
        sUpState = PORT_WS_FAILED;
        return false;
    }
    if (sUpUrl != url) {    /* closed or re-targeted while we dialed */
        sUpState = PORT_WS_CLOSED;
        curl_easy_cleanup(c);
        return false;
    }
    sUpCurl = c;
    sUpState = PORT_WS_OPEN;
    port_log("SSB64 WS: uplink reconnected\n");
    return true;
}

/* Send on the dedicated uplink, re-dialing the remembered URL at a bounded
 * cadence when the previous socket died. Returns 1 on a successful uplink
 * send, -1 when the uplink is unusable and the caller should fall back to
 * the control socket - the frame is never dropped just because the
 * uplink died. sUpMutex is held across the whole dial+send: it bounds the
 * serialization to one uplink at a time and gives close() a single wait
 * point. */
int uplink_send_curl(const void *data, size_t size, unsigned int flags)
{
    std::lock_guard<std::mutex> lock(sUpMutex);
    if (!uplink_ensure_curl()) {
        return -1;
    }
    std::lock_guard<std::mutex> ioLock(sUpIoMutex);
    if (send_frame_curl(sUpCurl, data, size, flags)) {
        return 1;
    }
    /* Dead socket - drop the handle so the next send takes the re-dial
     * path. cleanup under the same io lock that serializes sends. */
    CURL* dead = sUpCurl;
    sUpCurl = nullptr;
    sUpState = PORT_WS_FAILED;
    curl_easy_cleanup(dead);
    return -1;
}

} // namespace

extern "C" int port_ws_connect(const char *url)
{
    port_ws_close();
    port_ws_uplink_close();
    sStop = false;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sError.clear();
        sTextQueue.clear();
        sBinaryQueue.clear();
    }
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
    if (uplink_send_curl(text, std::strlen(text), CURLWS_TEXT) > 0) {
        return 1;
    }
    if (sState != PORT_WS_OPEN) {
        return 0;
    }
    std::lock_guard<std::mutex> ioLock(sIoMutex);
    if (sCurl == nullptr) {
        return 0;
    }
    return send_frame_curl(sCurl, text, std::strlen(text), CURLWS_TEXT);
}

extern "C" int port_ws_send_binary(const void *data, int size)
{
    if (uplink_send_curl(data, static_cast<size_t>(size), CURLWS_BINARY) > 0) {
        return 1;
    }
    if (sState != PORT_WS_OPEN) {
        return 0;
    }
    std::lock_guard<std::mutex> ioLock(sIoMutex);
    if (sCurl == nullptr) {
        return 0;
    }
    return send_frame_curl(sCurl, data, static_cast<size_t>(size), CURLWS_BINARY);
}

extern "C" int port_ws_poll_text(char *out, int capacity)
{
    std::lock_guard<std::mutex> lock(sMutex);
    if (sTextQueue.empty() || capacity <= 0) {
        return 0;
    }
    std::string text = std::move(sTextQueue.front());
    sTextQueue.pop_front();
    size_t n = (text.size() < static_cast<size_t>(capacity - 1)) ? text.size() : static_cast<size_t>(capacity - 1);
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
    return 1;
}

extern "C" int port_ws_poll_binary(void *out, int capacity)
{
    std::lock_guard<std::mutex> lock(sMutex);
    if (sBinaryQueue.empty()) {
        return 0;
    }
    std::vector<unsigned char> data = std::move(sBinaryQueue.front());
    sBinaryQueue.pop_front();
    if (static_cast<int>(data.size()) > capacity) {
        return 0;
    }
    std::memcpy(out, data.data(), data.size());
    return static_cast<int>(data.size());
}

extern "C" int port_ws_uplink_connect(const char *url)
{
    port_ws_uplink_close();
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        sUpUrl = url != nullptr ? url : "";
        sUpRetryAfter = std::chrono::steady_clock::time_point{};
    }
    sUpState = PORT_WS_CONNECTING;
    std::string err;
    CURL* c = ws_dial(url != nullptr ? url : "", &err);
    std::lock_guard<std::mutex> lock(sUpMutex);
    if (c == nullptr) {
        port_log("SSB64 WS: uplink failed: %s\n", err.c_str());
        sUpState = PORT_WS_FAILED;
        return 0;
    }
    if (sUpUrl != (url != nullptr ? url : "")) {    /* closed or re-targeted while we dialed */
        sUpState = PORT_WS_CLOSED;
        curl_easy_cleanup(c);
        return 0;
    }
    sUpCurl = c;
    sUpState = PORT_WS_OPEN;
    return 1;
}

extern "C" void port_ws_uplink_close(void)
{
    CURL* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        c = sUpCurl;
        sUpCurl = nullptr;
        sUpUrl.clear();    /* explicit close — never resurrect */
    }
    if (c != nullptr) {
        {
            std::lock_guard<std::mutex> ioLock(sUpIoMutex);
            send_frame_curl(c, "", 0, CURLWS_CLOSE);
        }
        curl_easy_cleanup(c);
    }
    sUpState = PORT_WS_CLOSED;
}

extern "C" int port_ws_uplink_state(void) { return sUpState; }

extern "C" int port_ws_uplink_send(const void *data, int size)
{
    /* Uplink-only API — no control-socket fallback here. */
    return uplink_send_curl(data, static_cast<size_t>(size), CURLWS_BINARY) > 0 ? 1 : 0;
}

#endif
