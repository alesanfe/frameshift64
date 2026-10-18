/* See websocket_transport.h. */
#include "websocket_transport.h"
#include "port_log.h"

#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

/* Message/queue caps: lobby text and relayed game frames are tiny, so these
 * are far above any legitimate need. They bound the damage a hostile or
 * stuck server can do — applied by each backend's receive loop. */
constexpr size_t kMaxWsMessageBytes = 16u << 20;   /* reassembled message cap */
constexpr size_t kMaxWsQueuedMessages = 4096;      /* pending for the game thread */

} // namespace

#if defined(_WIN32) && !defined(SSB64_PORTABLE_WS)
#include <windows.h>
#include <winhttp.h>

namespace {

struct Connection {
    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    HINTERNET socket = nullptr;
};

std::mutex sMutex;                       // guards the queues, error text and handles
std::atomic<int> sState{ PORT_WS_CLOSED };
std::string sError;
std::deque<std::string> sTextQueue;
std::deque<std::vector<unsigned char>> sBinaryQueue;
Connection sConn;
std::thread sThread;
std::atomic<bool> sStop{ false };

std::wstring widen(const std::string &s)
{
    if (s.empty()) {
        return std::wstring();
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}

void fail(const std::string &why)
{
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sError = why;
    }
    sState = PORT_WS_FAILED;
    port_log("SSB64 WS: %s\n", why.c_str());
}

void close_conn_parts(Connection &c)
{
    if (c.request) { WinHttpCloseHandle(c.request); c.request = nullptr; }
    if (c.connect) { WinHttpCloseHandle(c.connect); c.connect = nullptr; }
    if (c.session) { WinHttpCloseHandle(c.session); c.session = nullptr; }
}

void close_handles(void)
{
    std::lock_guard<std::mutex> lock(sMutex);
    if (sConn.socket) { WinHttpCloseHandle(sConn.socket); sConn.socket = nullptr; }
    close_conn_parts(sConn);
}

/* Blocking dial: URL parse, HTTP handshake, websocket upgrade. Returns the
 * upgraded socket handle, or nullptr with `err` set. While `live`/`mu` are
 * given the intermediate handles are published as they are obtained so a
 * closer can abort a pending handshake; on success `live` keeps the
 * session/connect handles (the request is retired by the upgrade). */
HINTERNET ws_dial(const std::string &url, std::mutex *mu, Connection *live, std::string *err)
{
    bool secure = false;
    std::string rest;
    if (url.rfind("wss://", 0) == 0) {
        secure = true;
        rest = url.substr(6);
    } else if (url.rfind("ws://", 0) == 0) {
        rest = url.substr(5);
    } else {
        *err = "server URL must start with ws:// or wss://";
        return nullptr;
    }
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    std::string host = hostport;
    INTERNET_PORT port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        /* std::stoi would throw on a non-numeric port — this runs on the
         * connect thread, so an exception is std::terminate. strtol fails
         * soft into a clean dial error instead. */
        const std::string portText = hostport.substr(colon + 1);
        char *end = nullptr;
        long p = std::strtol(portText.c_str(), &end, 10);
        if (end == portText.c_str() || *end != '\0' || p <= 0 || p > 65535) {
            *err = "invalid port in server URL";
            return nullptr;
        }
        port = static_cast<INTERNET_PORT>(p);
    }

    Connection c;
    auto publish = [&]() {
        if (mu != nullptr && live != nullptr) {
            std::lock_guard<std::mutex> lock(*mu);
            *live = c;
        }
    };
    auto dead = [&](const std::string &why) -> HINTERNET {
        *err = why;
        close_conn_parts(c);
        publish();
        return nullptr;
    };

    c.session = WinHttpOpen(L"BattleShip-Netplay/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c.session) {
        return dead("WinHttpOpen failed");
    }
    /* Bounded receive: costs nothing where it does not apply to websocket
     * receives and keeps long-lived sends from being starved where it does. */
    WinHttpSetTimeouts(c.session, 5000, 5000, 5000, 200);
    c.connect = WinHttpConnect(c.session, widen(host).c_str(), port, 0);
    c.request = (c.connect != nullptr)
        ? WinHttpOpenRequest(c.connect, L"GET", widen(path).c_str(), nullptr,
                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             secure ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;
    publish();
    if (!c.connect || !c.request) {
        return dead("couldn't reach " + host);
    }
    if (!WinHttpSetOption(c.request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(c.request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(c.request, nullptr)) {
        return dead("couldn't connect to " + host + " (error " + std::to_string(GetLastError()) + ")");
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(c.request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                        &status, &size, nullptr);
    if (status != 101) {
        return dead("server answered HTTP " + std::to_string(status));
    }
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(c.request, 0);
    if (!socket) {
        return dead("WebSocket upgrade failed");
    }
    WinHttpCloseHandle(c.request);
    c.request = nullptr;
    publish();
    return socket;
}

void run(std::string url)
{
    std::string err;
    HINTERNET socket = ws_dial(url, &sMutex, &sConn, &err);
    if (!socket) {
        fail(err);
        close_handles();
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sConn.socket = socket;
    }
    sState = PORT_WS_OPEN;

    std::vector<unsigned char> message;
    unsigned char buffer[4096];
    while (!sStop) {
        DWORD read = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        DWORD recvErr = WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &read, &type);
        if (recvErr == ERROR_WINHTTP_TIMEOUT) {
            continue;   // idle window so pending sends can proceed
        }
        if (recvErr != ERROR_SUCCESS) {
            if (!sStop) fail("connection to the server was lost");
            break;
        }
        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            if (!sStop) {
                USHORT code = 0;
                char reason[128] = {};
                DWORD reason_len = 0;
                WinHttpWebSocketQueryCloseStatus(socket, &code, reason, sizeof(reason) - 1, &reason_len);
                fail(std::string("server closed the connection") + (reason_len ? std::string(": ") + reason : ""));
            }
            break;
        }
        message.insert(message.end(), buffer, buffer + read);
        if (message.size() > kMaxWsMessageBytes) {
            fail("server sent an oversized message");
            break;
        }
        bool fragment = (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
                         type == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE);
        if (fragment) {
            continue;
        }
        /* Same caps as the portable path: the game thread drains the queues
         * lazily, so a flooding server could otherwise exhaust memory while
         * we're stalled. Overflow => fail the connection, don't desync. */
        bool overflow = false;
        {
            std::lock_guard<std::mutex> lock(sMutex);
            overflow = sTextQueue.size() + sBinaryQueue.size() >= kMaxWsQueuedMessages;
            if (!overflow) {
                if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
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
    }
}

/* WinHTTP forbids concurrent sends on one websocket; the game coroutine
 * (netpeer relay traffic) and the main loop (lobby pings) can both send.
 * One mutex serializes all sends across both sockets. */
std::mutex sSendMutex;
std::atomic<int> sSendInFlight{ 0 };

std::atomic<unsigned long> sSendCount[2]{};   /* [0]=control [1]=uplink */

int send_on(HINTERNET socket, const void *data, DWORD size, WINHTTP_WEB_SOCKET_BUFFER_TYPE type,
            char which)
{
    ULONGLONG t0 = GetTickCount64();
    sSendMutex.lock();
    ULONGLONG waited = GetTickCount64() - t0;
    int inflight = ++sSendInFlight;
    DWORD err = WinHttpWebSocketSend(socket, type, const_cast<void *>(data), size);
    --sSendInFlight;
    sSendMutex.unlock();
    ULONGLONG sent = GetTickCount64() - t0 - waited;
    unsigned long count = ++sSendCount[which == 'U' ? 1 : 0];
    if (err != ERROR_SUCCESS || waited > 100 || sent > 100) {
        port_log("SSB64 WS: send %c type=%u err=%lu wait=%llums sent=%llums inflight=%d total=%lu\n",
                 which, static_cast<unsigned>(type), err, waited, sent, inflight, count);
    }
    return err == ERROR_SUCCESS ? 1 : 0;
}

int send_frame(const void *data, DWORD size, WINHTTP_WEB_SOCKET_BUFFER_TYPE type)
{
    if (sState != PORT_WS_OPEN) {
        return 0;
    }
    HINTERNET socket;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        socket = sConn.socket;
    }
    if (!socket) {
        return 0;
    }
    return send_on(socket, data, size, type, 'C');
}

/* Uplink socket: send-only second connection. The control socket carries a
 * pending blocking receive for server->client traffic; keeping all outbound
 * game frames on a dedicated socket avoids mixing directions on one handle
 * and sidesteps ws proxies that mishandle it (observed with wrangler dev).
 * Once open, ALL outbound traffic prefers this socket - the control socket
 * only receives. */
std::mutex sUpMutex;
/* Serializes uplink sends vs close/re-dial so the socket is never closed
 * while a frame is being written to it. */
std::mutex sUpIoMutex;
std::atomic<int> sUpState{ PORT_WS_CLOSED };
Connection sUpConn;
/* Lazy-reconnect state: the uplink is send-only, so a dead TCP shows up
 * only as send failures — without a re-dial a relayed peer went mute
 * until the match timed out. The last URL is remembered and re-dialed
 * inside send at a bounded cadence. */
std::string sUpUrl;
ULONGLONG sUpRetryAfterTick = 0;
constexpr ULONGLONG kUpRedialMs = 2000;

/* Send on the dedicated uplink, re-dialing the remembered URL at a bounded
 * cadence when the link is dead. Returns 1 when the frame went out on the
 * uplink, -1 when the uplink is unusable and the caller should fall back
 * to the control socket — the frame is never dropped just because the
 * uplink died. */
int uplink_send(const void *data, DWORD size, WINHTTP_WEB_SOCKET_BUFFER_TYPE type)
{
    HINTERNET socket = nullptr;
    bool dial = false;
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpState == PORT_WS_OPEN && sUpConn.socket != nullptr) {
            socket = sUpConn.socket;
        } else if (!sUpUrl.empty() && GetTickCount64() >= sUpRetryAfterTick) {
            /* Claim this redial window — only one sender dials per period. */
            sUpRetryAfterTick = GetTickCount64() + kUpRedialMs;
            dial = true;
        } else {
            return -1;
        }
    }
    if (dial) {
        std::lock_guard<std::mutex> io(sUpIoMutex);
        {
            std::lock_guard<std::mutex> lock(sUpMutex);
            if (sUpState == PORT_WS_OPEN && sUpConn.socket != nullptr) {
                socket = sUpConn.socket;    /* another sender re-dialed while we waited */
            }
        }
        if (socket == nullptr) {
            std::string url, err;
            {
                std::lock_guard<std::mutex> lock(sUpMutex);
                url = sUpUrl;
            }
            /* ws_dial publishes intermediate handles into sUpConn under
             * sUpMutex — a concurrent close can still abort the dial. */
            socket = ws_dial(url, &sUpMutex, &sUpConn, &err);
            std::lock_guard<std::mutex> lock(sUpMutex);
            if (socket == nullptr) {
                sUpState = PORT_WS_FAILED;
                return -1;
            }
            if (sUpUrl != url) {    /* closed or re-targeted while we dialed */
                close_conn_parts(sUpConn);
                sUpState = PORT_WS_CLOSED;
                WinHttpCloseHandle(socket);
                return -1;
            }
            sUpConn.socket = socket;
            sUpState = PORT_WS_OPEN;
            port_log("SSB64 WS: uplink reconnected\n");
        }
    }
    std::lock_guard<std::mutex> io(sUpIoMutex);
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpConn.socket != socket) {
            return -1;    /* closed or swapped while we waited on the lock */
        }
    }
    if (send_on(socket, data, size, type, 'U')) {
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpConn.socket == socket) {
            sUpConn.socket = nullptr;
            sUpState = PORT_WS_FAILED;
        }
    }
    WinHttpCloseHandle(socket);
    return -1;
}

} // namespace

extern "C" int port_ws_connect(const char *url)
{
    port_ws_close();
    port_ws_uplink_close();    // a new room never reuses an old uplink
    sStop = false;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sError.clear();
        sTextQueue.clear();
        sBinaryQueue.clear();
    }
    sState = PORT_WS_CONNECTING;
    sThread = std::thread(run, std::string(url ? url : ""));
    return 1;
}

extern "C" void port_ws_close(void)
{
    sStop = true;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sConn.socket) {
            WinHttpWebSocketClose(sConn.socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        }
    }
    close_handles();    // unblocks a pending receive / connect
    if (sThread.joinable()) {
        sThread.join();
    }
    sState = PORT_WS_CLOSED;
}

extern "C" int port_ws_state(void)
{
    return sState;
}

extern "C" const char *port_ws_error(void)
{
    static std::string copy;
    std::lock_guard<std::mutex> lock(sMutex);
    copy = sError;
    return copy.c_str();
}

extern "C" int port_ws_send_text(const char *text)
{
    /* uplink first: sends on the control socket stall behind its receive;
     * a dead uplink falls back to the control socket rather than dropping */
    if (uplink_send(text, static_cast<DWORD>(std::strlen(text)),
                    WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) > 0) {
        return 1;
    }
    return send_frame(text, static_cast<DWORD>(std::strlen(text)), WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE);
}

extern "C" int port_ws_send_binary(const void *data, int size)
{
    if (uplink_send(data, static_cast<DWORD>(size),
                    WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) > 0) {
        return 1;
    }
    return send_frame(data, static_cast<DWORD>(size), WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE);
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
        return 0;   // oversized: drop
    }
    std::memcpy(out, data.data(), data.size());
    return static_cast<int>(data.size());
}

/* ---- uplink connection lifecycle ---- */

extern "C" int port_ws_uplink_connect(const char *url)
{
    port_ws_uplink_close();
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        sUpUrl = url != nullptr ? url : "";
        sUpRetryAfterTick = 0;
    }
    sUpState = PORT_WS_CONNECTING;
    std::string err;
    HINTERNET socket = ws_dial(url != nullptr ? url : "", &sUpMutex, &sUpConn, &err);
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (socket == nullptr) {
            port_log("SSB64 WS: uplink failed: %s\n", err.c_str());
            sUpState = PORT_WS_FAILED;
            return 0;
        }
        if (sUpUrl.empty()) {    /* closed while we were dialing */
            close_conn_parts(sUpConn);
            sUpState = PORT_WS_CLOSED;
            WinHttpCloseHandle(socket);
            return 0;
        }
        sUpConn.socket = socket;
        sUpState = PORT_WS_OPEN;
    }
    return 1;
}

extern "C" void port_ws_uplink_close(void)
{
    HINTERNET socket;
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        sUpUrl.clear();    /* explicit close — never resurrect */
        socket = sUpConn.socket;
        sUpConn.socket = nullptr;
        close_conn_parts(sUpConn);
        sUpState = PORT_WS_CLOSED;
    }
    if (socket != nullptr) {
        /* sUpIoMutex waits out an in-flight send before the close. */
        std::lock_guard<std::mutex> io(sUpIoMutex);
        WinHttpWebSocketClose(socket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(socket);
    }
}

extern "C" int port_ws_uplink_state(void)
{
    return sUpState;
}

extern "C" int port_ws_uplink_send(const void *data, int size)
{
    /* Uplink-only API — no control-socket fallback here. */
    return uplink_send(data, static_cast<DWORD>(size),
                       WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) > 0 ? 1 : 0;
}

#elif defined(SSB64_HAVE_CURL_WS)

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
int uplink_send_curl(const void *data, size_t size, unsigned int flags)
{
    std::lock_guard<std::mutex> lock(sUpMutex);
    if (sUpState != PORT_WS_OPEN || sUpCurl == nullptr) {
        const auto now = std::chrono::steady_clock::now();
        if (sUpUrl.empty() || now < sUpRetryAfter) {
            return -1;
        }
        /* Claim this redial window — only one sender dials per period. */
        sUpRetryAfter = now + kUpRedialMs;
        std::string err;
        std::string url = sUpUrl;
        CURL* c = ws_dial(url, &err);
        if (c == nullptr) {
            sUpState = PORT_WS_FAILED;
            return -1;
        }
        if (sUpUrl != url) {    /* closed or re-targeted while we dialed */
            sUpState = PORT_WS_CLOSED;
            curl_easy_cleanup(c);
            return -1;
        }
        sUpCurl = c;
        sUpState = PORT_WS_OPEN;
        port_log("SSB64 WS: uplink reconnected\n");
    }
    std::lock_guard<std::mutex> ioLock(sUpIoMutex);
    if (send_frame_curl(sUpCurl, data, size, flags)) {
        return 1;
    }
    /* Dead socket — drop the handle so the next send takes the re-dial
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

#else

/* BSD-socket fallback for targets without libcurl websockets (Switch,
 * or a desktop curl <7.86 built without the feature). Plain ws:// only —
 * wss:// needs TLS and reports that up front. */

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET ws_sock_t;
#define WS_SOCK_BAD INVALID_SOCKET
static int ws_close_sock(ws_sock_t s) { return closesocket(s); }
#else
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int ws_sock_t;
#define WS_SOCK_BAD (-1)
static int ws_close_sock(ws_sock_t s) { return close(s); }
#endif

namespace {

std::mutex sMutex;
std::atomic<int> sState{ PORT_WS_CLOSED };
std::string sError;
std::deque<std::string> sTextQueue;
std::deque<std::vector<unsigned char>> sBinaryQueue;
ws_sock_t sSock = WS_SOCK_BAD;
std::thread sThread;
std::atomic<bool> sStop{ false };

std::mutex sSendMutex;
std::mutex sUpMutex;
/* Serializes uplink sends vs close/re-dial so a socket is never closed
 * while a frame is being written to it. */
std::mutex sUpIoMutex;
std::atomic<int> sUpState{ PORT_WS_CLOSED };
ws_sock_t sUpSock = WS_SOCK_BAD;
/* Lazy-reconnect state — same contract as the libcurl branch: the
 * uplink is send-only so a dead link only surfaces on send; re-dial the
 * remembered URL at a bounded cadence instead of going mute. */
std::string sUpUrl;
std::chrono::steady_clock::time_point sUpRetryAfter{};
constexpr auto kUpRedialMs = std::chrono::milliseconds(2000);

unsigned int sMaskRng = 0;

unsigned int next_rand(void)
{
    /* SplitMix32 — good enough for WS masking (the mask is a per-frame
     * nonce for proxies, not a security boundary). */
    sMaskRng += 0x9E3779B9u;
    unsigned int z = sMaskRng;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

void fail(const std::string &why)
{
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sError = why;
    }
    sState = PORT_WS_FAILED;
    port_log("SSB64 WS: %s\n", why.c_str());
}

bool send_all(ws_sock_t s, const void *data, size_t size)
{
    const unsigned char *p = static_cast<const unsigned char *>(data);
    while (size > 0) {
        int n = send(s, reinterpret_cast<const char *>(p), static_cast<int>(size), 0);
        if (n <= 0) {
            return false;
        }
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

/* Waits for readable (or error); false on timeout/closed. */
bool wait_readable(ws_sock_t s, int timeout_ms)
{
#if defined(_WIN32)
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(s, &fds);
    timeval tv{ timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
    return select(0, &fds, nullptr, nullptr, &tv) == 1;
#else
    pollfd pfd{ s, POLLIN, 0 };
    return poll(&pfd, 1, timeout_ms) == 1;
#endif
}

/* Buffered stream reader: bytes the handshake over-read past \r\n\r\n are
 * already WebSocket frames and must feed the frame parser before any new
 * recv. `buf`/`pos` hold that leftover; empty for a clean boundary. */
struct Reader {
    ws_sock_t s;
    std::vector<unsigned char> buf;
    size_t pos = 0;

    bool read(void *out, size_t size)
    {
        unsigned char *p = static_cast<unsigned char *>(out);
        size_t have = buf.size() - pos;
        size_t take = (have < size) ? have : size;
        if (take != 0) {
            std::memcpy(p, buf.data() + pos, take);
            pos += take;
            p += take;
            size -= take;
            if (pos == buf.size()) {
                buf.clear();
                pos = 0;
            }
        }
        while (size > 0) {
            int n = recv(s, reinterpret_cast<char *>(p), static_cast<int>(size), 0);
            if (n <= 0) {
                return false;
            }
            p += n;
            size -= static_cast<size_t>(n);
        }
        return true;
    }
};

const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64_encode(const unsigned char *data, size_t size)
{
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        unsigned int v = data[i] << 16;
        if (i + 1 < size) v |= data[i + 1] << 8;
        if (i + 2 < size) v |= data[i + 2];
        out += b64_table[(v >> 18) & 63];
        out += b64_table[(v >> 12) & 63];
        out += (i + 1 < size) ? b64_table[(v >> 6) & 63] : '=';
        out += (i + 2 < size) ? b64_table[v & 63] : '=';
    }
    return out;
}

/* ws://host[:port]/path -> connected + upgraded socket; `leftover` gets
 * any frame bytes that rode in past the \r\n\r\n. err set on failure. */
ws_sock_t ws_dial(const std::string &url, std::string *err,
                  std::vector<unsigned char> *leftover)
{
#if defined(_WIN32)
    static bool sWsaStarted = false;
    if (!sWsaStarted) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            *err = "WSAStartup failed";
            return WS_SOCK_BAD;
        }
        sWsaStarted = true;
    }
#endif
    if (url.rfind("wss://", 0) == 0) {
        *err = "wss:// needs TLS — this build lacks WebSocket TLS support; use ws:// for a local lobby";
        return WS_SOCK_BAD;
    }
    if (url.rfind("ws://", 0) != 0) {
        *err = "server URL must start with ws:// or wss://";
        return WS_SOCK_BAD;
    }
    std::string rest = url.substr(5);
    size_t slash = rest.find('/');
    std::string hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    std::string host = hostport, port = "80";
    size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = hostport.substr(colon + 1);
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *list = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &list) != 0 || list == nullptr) {
        *err = "couldn't resolve " + host;
        return WS_SOCK_BAD;
    }
    ws_sock_t s = WS_SOCK_BAD;
    for (addrinfo *it = list; it != nullptr && s == WS_SOCK_BAD; it = it->ai_next) {
        s = socket(it->ai_family, it->ai_socktype, it->ai_protocol);
        if (s == WS_SOCK_BAD) {
            continue;
        }
        if (connect(s, it->ai_addr, static_cast<int>(it->ai_addrlen)) != 0) {
            ws_close_sock(s);
            s = WS_SOCK_BAD;
        }
    }
    freeaddrinfo(list);
    if (s == WS_SOCK_BAD) {
        *err = "couldn't connect to " + host;
        return WS_SOCK_BAD;
    }

    unsigned char key_bytes[16];
    for (auto &b : key_bytes) { b = static_cast<unsigned char>(next_rand()); }
    std::string request =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + hostport + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: " + b64_encode(key_bytes, sizeof(key_bytes)) + "\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (!send_all(s, request.data(), request.size())) {
        ws_close_sock(s);
        *err = "handshake send failed";
        return WS_SOCK_BAD;
    }

    std::string reply;
    char chunk[512];
    int waited_ms = 0;
    while (reply.find("\r\n\r\n") == std::string::npos && reply.size() < 8192) {
        if (!wait_readable(s, 500)) {
            if ((waited_ms += 500) >= 5000) {
                ws_close_sock(s);
                *err = "handshake timed out";
                return WS_SOCK_BAD;
            }
            continue;
        }
        int n = recv(s, chunk, sizeof(chunk), 0);
        if (n <= 0) {
            ws_close_sock(s);
            *err = "handshake failed";
            return WS_SOCK_BAD;
        }
        reply.append(chunk, static_cast<size_t>(n));
    }
    /* "HTTP/1.1 101" (or HTTP/1.0) — a non-upgrade HTTP reply never has it. */
    if (reply.find(" 101") == std::string::npos) {
        ws_close_sock(s);
        *err = "server refused the WebSocket upgrade";
        return WS_SOCK_BAD;
    }
    size_t hdr_end = reply.find("\r\n\r\n") + 4;
    if (leftover != nullptr && reply.size() > hdr_end) {
        leftover->assign(reply.begin() + static_cast<long>(hdr_end), reply.end());
    }
    return s;
}

/* Sends one client frame (always masked, single fragment). */
int send_frame(ws_sock_t s, const void *data, int size, unsigned char opcode)
{
    if (s == WS_SOCK_BAD) {
        return 0;
    }
    unsigned char header[14];
    size_t hlen = 2;
    header[0] = static_cast<unsigned char>(0x80 | opcode);
    if (size < 126) {
        header[1] = static_cast<unsigned char>(0x80 | size);
    } else if (size <= 0xFFFF) {
        header[1] = 0x80 | 126;
        header[2] = static_cast<unsigned char>(size >> 8);
        header[3] = static_cast<unsigned char>(size);
        hlen = 4;
    } else {
        header[1] = 0x80 | 127;
        unsigned long long v = static_cast<unsigned long long>(size);
        for (int i = 0; i < 8; i++) {
            header[2 + i] = static_cast<unsigned char>(v >> (56 - i * 8));
        }
        hlen = 10;
    }
    unsigned char mask[4];
    for (auto &b : mask) { b = static_cast<unsigned char>(next_rand()); }
    std::memcpy(header + hlen, mask, 4);
    hlen += 4;

    std::vector<unsigned char> payload;
    const unsigned char *bytes = static_cast<const unsigned char *>(data);
    if (size > 0) {
        payload.resize(static_cast<size_t>(size));
        for (int i = 0; i < size; i++) {
            payload[static_cast<size_t>(i)] = bytes[i] ^ mask[i & 3];
        }
    }
    std::lock_guard<std::mutex> lock(sSendMutex);
    return (send_all(s, header, hlen) &&
            (payload.empty() || send_all(s, payload.data(), payload.size()))) ? 1 : 0;
}

/* Reads one complete frame. Returns opcode (0 continuation..0xF) with the
 * FIN bit in `*fin`, payload in `out` (unmasked — servers must not mask).
 * -1 on failure, -2 for a frame to drop (masked server frame). */
int recv_frame(Reader *r, std::vector<unsigned char> *out, int *fin)
{
    unsigned char head[2];
    if (!r->read(head, 2)) {
        return -1;
    }
    *fin = (head[0] & 0x80) != 0;
    int opcode = head[0] & 0x0F;
    unsigned long long len = head[1] & 0x7F;
    if (len == 126) {
        unsigned char ext[2];
        if (!r->read(ext, 2)) {
            return -1;
        }
        len = (static_cast<unsigned long long>(ext[0]) << 8) | ext[1];
    } else if (len == 127) {
        unsigned char ext[8];
        if (!r->read(ext, 8)) {
            return -1;
        }
        len = 0;
        for (int i = 0; i < 8; i++) {
            len = (len << 8) | ext[i];
        }
    }
    /* Cap before either branch allocates: a masked frame is already
     * protocol garbage, and an unmasked frame only ever carries ≤4MiB —
     * a giant len would otherwise reach out->assign/resize and throw
     * std::length_error on the recv thread (uncaught → terminate). */
    if (len > (4u << 20)) {
        return -1;
    }
    if ((head[1] & 0x80) != 0) {
        unsigned char mask[4];
        if (!r->read(mask, 4)) {
            return -1;
        }
        /* A masked server frame is protocol garbage; consume-and-drop. */
        out->assign(static_cast<size_t>(len), 0);
        if (len != 0 && !r->read(out->data(), static_cast<size_t>(len))) {
            return -1;
        }
        for (size_t i = 0; i < out->size(); i++) {
            (*out)[i] ^= mask[i & 3];
        }
        return -2;  /* drop */
    }
    if (len > 0) {
        out->resize(static_cast<size_t>(len));
        if (!r->read(out->data(), static_cast<size_t>(len))) {
            return -1;
        }
    } else {
        out->clear();
    }
    return opcode;
}

void run(std::string url)
{
    std::string err;
    std::vector<unsigned char> leftover;
    ws_sock_t s = ws_dial(url, &err, &leftover);
    if (s == WS_SOCK_BAD) {
        fail(err);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sSock = s;
    }
    sState = PORT_WS_OPEN;

    Reader r;
    r.s = s;
    r.buf = std::move(leftover);
    std::vector<unsigned char> message;
    int message_kind = -1;      /* opcode of the fragment that opened it */
    while (!sStop) {
        if (r.pos == r.buf.size() && !wait_readable(s, 200)) {
            continue;   /* idle tick so sStop is noticed */
        }
        std::vector<unsigned char> payload;
        int fin = 0;
        int opcode = recv_frame(&r, &payload, &fin);
        if (opcode == -1) {
            if (!sStop) { fail("connection to the server was lost"); }
            break;
        }
        if (opcode == -2) {
            continue;   /* masked server frame: dropped */
        }
        if (opcode == 0x8) {                       /* close */
            unsigned int code = payload.size() >= 2
                ? (static_cast<unsigned int>(payload[0]) << 8) | payload[1] : 0;
            port_log("SSB64 WS: close frame code=%u len=%u\n",
                     code, (unsigned)payload.size());
            if (!sStop) { fail("server closed the connection"); }
            break;
        }
        if (opcode == 0x9) {                       /* ping -> pong */
            send_frame(s, payload.data(), static_cast<int>(payload.size()), 0xA);
            continue;
        }
        if (opcode == 0xA) {
            continue;                              /* pong: nothing to track */
        }
        if (opcode != 0) {                         /* first fragment of a message */
            message_kind = opcode;
            message.clear();
        }
        message.insert(message.end(), payload.begin(), payload.end());
        if (message.size() > kMaxWsMessageBytes) {
            fail("server sent an oversized message");
            break;
        }
        if (fin != 0 && message_kind >= 0) {
            /* The game thread drains these lazily — cap them like the UDP
             * ingress queue so a flooding server can't exhaust memory
             * while we're stalled. Overflow means the consumer is dead;
             * failing the connection beats a silent desync. */
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
}

/* Send on the dedicated uplink, re-dialing the remembered URL at a bounded
 * cadence when the link is dead. Returns 1 when the frame went out on the
 * uplink, -1 when the uplink is unusable and the caller should fall back
 * to the control socket — the frame is never dropped just because the
 * uplink died. */
int uplink_send(const void *data, int size, unsigned char opcode)
{
    ws_sock_t s = WS_SOCK_BAD;
    bool dial = false;
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpState == PORT_WS_OPEN && sUpSock != WS_SOCK_BAD) {
            s = sUpSock;
        } else if (!sUpUrl.empty() &&
                   std::chrono::steady_clock::now() >= sUpRetryAfter) {
            /* Claim this redial window — only one sender dials per period. */
            sUpRetryAfter = std::chrono::steady_clock::now() + kUpRedialMs;
            dial = true;
        } else {
            return -1;
        }
    }
    if (dial) {
        std::lock_guard<std::mutex> io(sUpIoMutex);
        {
            std::lock_guard<std::mutex> lock(sUpMutex);
            if (sUpState == PORT_WS_OPEN && sUpSock != WS_SOCK_BAD) {
                s = sUpSock;    /* another sender re-dialed while we waited */
            }
        }
        if (s == WS_SOCK_BAD) {
            std::string url, err;
            {
                std::lock_guard<std::mutex> lock(sUpMutex);
                url = sUpUrl;
            }
            s = ws_dial(url, &err, nullptr);
            std::lock_guard<std::mutex> lock(sUpMutex);
            if (s == WS_SOCK_BAD) {
                sUpState = PORT_WS_FAILED;
                return -1;
            }
            if (sUpUrl != url) {    /* closed or re-targeted while we dialed */
                sUpState = PORT_WS_CLOSED;
                ws_close_sock(s);
                return -1;
            }
            sUpSock = s;
            sUpState = PORT_WS_OPEN;
            port_log("SSB64 WS: uplink reconnected\n");
        }
    }
    std::lock_guard<std::mutex> io(sUpIoMutex);
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpSock != s) {
            return -1;    /* closed or swapped while we waited on the lock */
        }
    }
    if (send_frame(s, data, size, opcode)) {
        return 1;
    }
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpSock == s) {
            sUpSock = WS_SOCK_BAD;
            sUpState = PORT_WS_FAILED;
        }
    }
    shutdown(s, 2);
    ws_close_sock(s);
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
    sMaskRng = std::random_device{}() ^
        static_cast<unsigned int>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    sThread = std::thread(run, std::string(url != nullptr ? url : ""));
    return 1;
}

extern "C" void port_ws_close(void)
{
    sStop = true;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        if (sSock != WS_SOCK_BAD) {
            shutdown(sSock, 2);        /* unblocks the recv thread */
            ws_close_sock(sSock);
            sSock = WS_SOCK_BAD;
        }
    }
    if (sThread.joinable()) {
        sThread.join();
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
    if (uplink_send(text, static_cast<int>(std::strlen(text)), 0x1) > 0) {
        return 1;
    }
    if (sState != PORT_WS_OPEN) {
        return 0;
    }
    return send_frame(sSock, text, static_cast<int>(std::strlen(text)), 0x1);
}

extern "C" int port_ws_send_binary(const void *data, int size)
{
    if (uplink_send(data, size, 0x2) > 0) {
        return 1;
    }
    if (sState != PORT_WS_OPEN) {
        return 0;
    }
    return send_frame(sSock, data, size, 0x2);
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
    /* send-only socket: handshake leftovers (if any) are discarded —
     * nothing reads this side. */
    ws_sock_t s = ws_dial(url != nullptr ? url : "", &err, nullptr);
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (s == WS_SOCK_BAD) {
            port_log("SSB64 WS: uplink failed: %s\n", err.c_str());
            sUpState = PORT_WS_FAILED;
            return 0;
        }
        if (sUpUrl.empty()) {    /* closed while we were dialing */
            ws_close_sock(s);
            sUpState = PORT_WS_CLOSED;
            return 0;
        }
        sUpSock = s;
        sUpState = PORT_WS_OPEN;
    }
    return 1;
}

extern "C" void port_ws_uplink_close(void)
{
    ws_sock_t s;
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        sUpUrl.clear();    /* explicit close — never resurrect */
        s = sUpSock;
        sUpSock = WS_SOCK_BAD;
        sUpState = PORT_WS_CLOSED;
    }
    if (s != WS_SOCK_BAD) {
        /* sUpIoMutex waits out an in-flight send before the close. */
        std::lock_guard<std::mutex> io(sUpIoMutex);
        shutdown(s, 2);
        ws_close_sock(s);
    }
}

extern "C" int port_ws_uplink_state(void) { return sUpState; }

extern "C" int port_ws_uplink_send(const void *data, int size)
{
    /* Uplink-only API — no control-socket fallback here. */
    return uplink_send(data, size, 0x2) > 0 ? 1 : 0;
}

#endif
