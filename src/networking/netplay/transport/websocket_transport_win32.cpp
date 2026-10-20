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

struct WsDialTarget {
    bool secure = false;
    std::string hostport;
    std::string host;
    std::string path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTP_PORT;
};

static bool ws_parse_dial_target(const std::string &url, WsDialTarget *t, std::string *err)
{
    std::string rest;
    if (url.rfind("wss://", 0) == 0) {
        t->secure = true;
        rest = url.substr(6);
    } else if (url.rfind("ws://", 0) == 0) {
        rest = url.substr(5);
    } else {
        *err = "server URL must start with ws:// or wss://";
        return false;
    }
    size_t slash = rest.find('/');
    t->hostport = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    t->path = (slash == std::string::npos) ? "/" : rest.substr(slash);
    t->host = t->hostport;
    t->port = t->secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    size_t colon = t->hostport.rfind(':');
    if (colon != std::string::npos) {
        t->host = t->hostport.substr(0, colon);
        /* std::stoi would throw on a non-numeric port — this runs on the
         * connect thread, so an exception is std::terminate. strtol fails
         * soft into a clean dial error instead. */
        const std::string portText = t->hostport.substr(colon + 1);
        char *end = nullptr;
        long p = std::strtol(portText.c_str(), &end, 10);
        if (end == portText.c_str() || *end != '\0' || p <= 0 || p > 65535) {
            *err = "invalid port in server URL";
            return false;
        }
        t->port = static_cast<INTERNET_PORT>(p);
    }
    return true;
}

static void ws_open_request(Connection *c, const WsDialTarget &t)
{
    c->session = WinHttpOpen(L"BattleShip-Netplay/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                             WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!c->session) {
        return;
    }
    /* Bounded receive: costs nothing where it does not apply to websocket
     * receives and keeps long-lived sends from being starved where it does. */
    WinHttpSetTimeouts(c->session, 5000, 5000, 5000, 200);
    c->connect = WinHttpConnect(c->session, widen(t.host).c_str(), t.port, 0);
    c->request = (c->connect != nullptr)
        ? WinHttpOpenRequest(c->connect, L"GET", widen(t.path).c_str(), nullptr,
                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                             t.secure ? WINHTTP_FLAG_SECURE : 0)
        : nullptr;
}

/* Sends the upgrade request and completes the websocket handshake.
 * On failure *err is set and nullptr returned (request still owned by c). */
static HINTERNET ws_perform_upgrade(Connection *c, const WsDialTarget &t, std::string *err)
{
    if (!WinHttpSetOption(c->request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(c->request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(c->request, nullptr)) {
        *err = "couldn't connect to " + t.host + " (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    WinHttpQueryHeaders(c->request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr,
                        &status, &size, nullptr);
    if (status != 101) {
        *err = "server answered HTTP " + std::to_string(status);
        return nullptr;
    }
    HINTERNET socket = WinHttpWebSocketCompleteUpgrade(c->request, 0);
    if (!socket) {
        *err = "WebSocket upgrade failed";
        return nullptr;
    }
    WinHttpCloseHandle(c->request);
    c->request = nullptr;
    return socket;
}

/* Blocking dial: URL parse, HTTP handshake, websocket upgrade. Returns the
 * upgraded socket handle, or nullptr with `err` set. While `live`/`mu` are
 * given the intermediate handles are published as they are obtained so a
 * closer can abort a pending handshake; on success `live` keeps the
 * session/connect handles (the request is retired by the upgrade). */
HINTERNET ws_dial(const std::string &url, std::mutex *mu, Connection *live, std::string *err)
{
    WsDialTarget t;
    if (!ws_parse_dial_target(url, &t, err)) {
        return nullptr;
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

    ws_open_request(&c, t);
    if (!c.session) {
        return dead("WinHttpOpen failed");
    }
    publish();
    if (!c.connect || !c.request) {
        return dead("couldn't reach " + t.host);
    }
    HINTERNET socket = ws_perform_upgrade(&c, t, err);
    if (!socket) {
        return dead(*err);
    }
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
/* Uplink bookkeeping — the locking choreography is shared
 * (websocket_transport_internal.h); this TU keeps only the handle set.
 * The Connection wrapper lets close_conn_parts() free the whole chain
 * whenever the uplink is torn down or re-dialed. */
WsUplink sUp;
Connection sUpConn;

/* Returns a live uplink socket: the open one, or a fresh re-dial of the
 * remembered URL. The locking choreography lives in WsUplinkSocket —
 * nullptr -> caller falls back to the control socket. */
HINTERNET uplink_socket(void)
{
    return WsUplinkSocket(sUp, GetTickCount64(), static_cast<HINTERNET>(nullptr),
        sUpConn.socket,
        [](const std::string &url) {
            std::string err;
            /* A send-fail only closes the socket — the session/connect
             * handles from the dead link are still in sUpConn and
             * ws_dial's publish would overwrite them. Retire them first,
             * under the same mu ws_dial publishes with. */
            {
                std::lock_guard<std::mutex> g(sUp.mu);
                close_conn_parts(sUpConn);
            }
            /* ws_dial publishes intermediate handles into sUpConn under
             * sUp.mu — a concurrent close can still abort the dial. */
            return ws_dial(url, &sUp.mu, &sUpConn, &err);
        },
        [](HINTERNET s) {
            /* superseded socket: free the intermediates it published
             * plus the socket itself — under sUp.mu. */
            close_conn_parts(sUpConn);
            WinHttpCloseHandle(s);
        });
}

/* Send on the dedicated uplink, re-dialing the remembered URL at a bounded
 * cadence when the previous socket died. Returns 1 on a successful uplink
 * send, -1 when the uplink is unusable and the caller should fall back to
 * the control socket - the frame is never dropped just because the
 * uplink died. */
int uplink_send(const void *data, DWORD size, WINHTTP_WEB_SOCKET_BUFFER_TYPE type)
{
    return WsUplinkSend(sUp, sUpConn.socket, uplink_socket(),
        static_cast<HINTERNET>(nullptr),
        [&](HINTERNET s) { return send_on(s, data, size, type, 'U'); },
        [](HINTERNET s) { WinHttpCloseHandle(s); });
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
    return WsSend(sState, text, static_cast<int>(std::strlen(text)),
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        [](const void *d, int n, unsigned char t) {
            return uplink_send(d, static_cast<DWORD>(n),
                static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(t));
        },
        [](const void *d, int n, unsigned char t) {
            return send_frame(d, static_cast<DWORD>(n),
                static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(t));
        });
}

extern "C" int port_ws_send_binary(const void *data, int size)
{
    return WsSend(sState, data, size,
        WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
        [](const void *d, int n, unsigned char t) {
            return uplink_send(d, static_cast<DWORD>(n),
                static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(t));
        },
        [](const void *d, int n, unsigned char t) {
            return send_frame(d, static_cast<DWORD>(n),
                static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(t));
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

/* ---- uplink connection lifecycle ---- */

extern "C" int port_ws_uplink_connect(const char *url)
{
    port_ws_uplink_close();
    return WsUplinkConnect(sUp, sUpConn.socket,
        static_cast<HINTERNET>(nullptr), url,
        [](const std::string &u, std::string &err) {
            /* ws_dial publishes intermediate handles into sUpConn under
             * sUp.mu — a concurrent close can still abort the dial. */
            return ws_dial(u, &sUp.mu, &sUpConn, &err);
        },
        [](HINTERNET s) {
            close_conn_parts(sUpConn);
            WinHttpCloseHandle(s);
        });
}

extern "C" void port_ws_uplink_close(void)
{
    /* Explicit close clears the remembered URL — never resurrect; the
     * graceful close runs under ioMu to wait out an in-flight send. */
    WsUplinkClose(sUp, sUpConn.socket, static_cast<HINTERNET>(nullptr),
        [] { close_conn_parts(sUpConn); },
        [](HINTERNET s) {
            WinHttpWebSocketClose(s, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(s);
        });
}

extern "C" int port_ws_uplink_state(void)
{
    return sUp.state;
}

extern "C" int port_ws_uplink_send(const void *data, int size)
{
    /* Uplink-only API — no control-socket fallback here. */
    return uplink_send(data, static_cast<DWORD>(size),
                       WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) > 0 ? 1 : 0;
}

#endif
