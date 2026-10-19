/* See websocket_transport.h - WinHTTP backend (desktop Windows default). Split out of websocket_transport.cpp;
 * empty TU off _WIN32 or when SSB64_PORTABLE_WS forces the portable path. */
#if defined(_WIN32) && !defined(SSB64_PORTABLE_WS)

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
    WsFail(sMutex, sError, sState, why);
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
        if (!WsEnqueueBounded(sMutex, sTextQueue, sBinaryQueue, message,
                              type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)) {
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
/* Returns a live uplink socket: the open one, or a fresh re-dial of the
 * remembered URL when the previous socket failed. The FAILED->claim->dial
 * path runs at a bounded cadence (sUpRetryAfterTick) with a single claimer
 * per window; CONNECTING is excluded so we never overlap an explicit
 * port_ws_uplink_connect dial. nullptr -> caller falls back to the control
 * socket. */
HINTERNET uplink_socket(void)
{
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpState == PORT_WS_OPEN && sUpConn.socket != nullptr) {
            return sUpConn.socket;
        }
        if (sUpState != PORT_WS_FAILED || sUpUrl.empty() ||
            GetTickCount64() < sUpRetryAfterTick) {
            return nullptr;
        }
        sUpRetryAfterTick = GetTickCount64() + kUpRedialMs;
    }
    std::lock_guard<std::mutex> io(sUpIoMutex);
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpState == PORT_WS_OPEN && sUpConn.socket != nullptr) {
            return sUpConn.socket;    /* another sender re-dialed while we waited */
        }
    }
    std::string url, err;
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        url = sUpUrl;
    }
    /* ws_dial publishes intermediate handles into sUpConn under sUpMutex -
     * a concurrent close can still abort the dial. */
    HINTERNET socket = ws_dial(url, &sUpMutex, &sUpConn, &err);
    std::lock_guard<std::mutex> lock(sUpMutex);
    if (socket == nullptr) {
        sUpState = PORT_WS_FAILED;
        return nullptr;
    }
    if (sUpUrl != url) {    /* closed or re-targeted while we dialed */
        if (sUpUrl.empty()) {
            sUpState = PORT_WS_CLOSED;
        }
        close_conn_parts(sUpConn);
        WinHttpCloseHandle(socket);
        return nullptr;
    }
    sUpConn.socket = socket;
    sUpState = PORT_WS_OPEN;
    port_log("SSB64 WS: uplink reconnected\n");
    return socket;
}

/* Send on the dedicated uplink, re-dialing the remembered URL at a bounded
 * cadence when the previous socket died. Returns 1 on a successful uplink
 * send, -1 when the uplink is unusable and the caller should fall back to
 * the control socket - the frame is never dropped just because the
 * uplink died. */
int uplink_send(const void *data, DWORD size, WINHTTP_WEB_SOCKET_BUFFER_TYPE type)
{
    HINTERNET socket = uplink_socket();
    if (socket == nullptr) {
        return -1;
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
    WsClearIngress(sMutex, sError, sTextQueue, sBinaryQueue);
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
    return WsPollText(sMutex, sTextQueue, out, capacity);
}

extern "C" int port_ws_poll_binary(void *out, int capacity)
{
    return WsPollBinary(sMutex, sBinaryQueue, out, capacity);
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

#endif
