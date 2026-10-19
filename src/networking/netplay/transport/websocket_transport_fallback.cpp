/* See websocket_transport.h - BSD-socket fallback for targets without
 * WinHTTP or libcurl websockets (e.g. Switch, curl <7.86). Plain ws:// only;
 * wss:// reports that it needs TLS. Emscripten is excluded: its transport is
 * websocket_web.cpp (emscripten_websocket_*), which always defines these
 * symbols. */
#if !defined(SSB64_HAVE_CURL_WS) && !defined(__EMSCRIPTEN__) && (!defined(_WIN32) || defined(SSB64_PORTABLE_WS))

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
    WsFail(sMutex, sError, sState, why);
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
        "Sec-WebSocket-Key: " + ssb64::Base64Encode(key_bytes, sizeof(key_bytes)) + "\r\n"
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
            if (!WsEnqueueBounded(sMutex, sTextQueue, sBinaryQueue,
                                  message, message_kind == 0x1)) {
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
/* Returns a live uplink socket: the open one, or a fresh re-dial of the
 * remembered URL when the previous socket failed. The FAILED->claim->dial
 * path runs at a bounded cadence (sUpRetryAfter) with a single claimer per
 * window; CONNECTING is excluded so we never overlap an explicit
 * port_ws_uplink_connect dial. WS_SOCK_BAD -> caller falls back to the
 * control socket. */
ws_sock_t uplink_socket(void)
{
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpState == PORT_WS_OPEN && sUpSock != WS_SOCK_BAD) {
            return sUpSock;
        }
        if (sUpState != PORT_WS_FAILED || sUpUrl.empty() ||
            std::chrono::steady_clock::now() < sUpRetryAfter) {
            return WS_SOCK_BAD;
        }
        sUpRetryAfter = std::chrono::steady_clock::now() + kUpRedialMs;
    }
    std::lock_guard<std::mutex> io(sUpIoMutex);
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        if (sUpState == PORT_WS_OPEN && sUpSock != WS_SOCK_BAD) {
            return sUpSock;    /* another sender re-dialed while we waited */
        }
    }
    std::string url, err;
    {
        std::lock_guard<std::mutex> lock(sUpMutex);
        url = sUpUrl;
    }
    ws_sock_t s = ws_dial(url, &err, nullptr);
    std::lock_guard<std::mutex> lock(sUpMutex);
    if (s == WS_SOCK_BAD) {
        sUpState = PORT_WS_FAILED;
        return WS_SOCK_BAD;
    }
    if (sUpUrl != url) {    /* closed or re-targeted while we dialed */
        if (sUpUrl.empty()) {
            sUpState = PORT_WS_CLOSED;
        }
        ws_close_sock(s);
        return WS_SOCK_BAD;
    }
    sUpSock = s;
    sUpState = PORT_WS_OPEN;
    port_log("SSB64 WS: uplink reconnected\n");
    return s;
}

/* Send on the dedicated uplink, re-dialing the remembered URL at a bounded
 * cadence when the previous socket died. Returns 1 on a successful uplink
 * send, -1 when the uplink is unusable and the caller should fall back to
 * the control socket - the frame is never dropped just because the
 * uplink died. */
int uplink_send(const void *data, int size, unsigned char opcode)
{
    ws_sock_t s = uplink_socket();
    if (s == WS_SOCK_BAD) {
        return -1;
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
    WsClearIngress(sMutex, sError, sTextQueue, sBinaryQueue);
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
    return WsPollText(sMutex, sTextQueue, out, capacity);
}

extern "C" int port_ws_poll_binary(void *out, int capacity)
{
    return WsPollBinary(sMutex, sBinaryQueue, out, capacity);
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
