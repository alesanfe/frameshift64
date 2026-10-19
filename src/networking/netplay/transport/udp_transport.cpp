/* See udp_transport.h. */
#include "udp_transport.h"
#include "port_log.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
typedef SOCKET PortSocket;
static const PortSocket kInvalidSocket = INVALID_SOCKET;
static int port_udp_last_error(void) { return WSAGetLastError(); }
[[maybe_unused]] static bool port_udp_would_block(int err) { return err == WSAEWOULDBLOCK; }
static void port_udp_close_socket(PortSocket s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int PortSocket;
static const PortSocket kInvalidSocket = -1;
static int port_udp_last_error(void) { return errno; }
static bool port_udp_would_block(int err) { return err == EAGAIN || err == EWOULDBLOCK; }
static void port_udp_close_socket(PortSocket s) { close(s); }
#endif

namespace {

struct PendingPacket {
    unsigned int release_ms;
    std::vector<uint8_t> bytes;
    bool has_to = false;
    sockaddr_in to = {};
    int relay_link = -1;   /* -1 = legacy route (peer / unframed relay) */
};

PortSocket sSocket = kInvalidSocket;
sockaddr_in sPeer;
bool sHasPeer = false;

/* Ingress thread: a datagram sitting in the OS socket buffer while the game
 * thread is stalled (rollback resim, GC pause) is indistinguishable from
 * remote silence on the wire — the peer's pacing drop fires even though
 * packets were arriving. Draining the socket on its own thread keeps
 * ingestion at wire speed; the game thread pops the queue once per tick,
 * so a slow frame delays *processing* but never *arrival*. */
struct IngressPacket {
    sockaddr_in from;
    int size;
    uint8_t bytes[2048];
};
constexpr size_t kIngressCap = 2048;   /* ~4 MB worst case; inputs are ~50B */
std::deque<IngressPacket> sIngress;
std::mutex sIngressMutex;
std::thread sRecvThread;
std::atomic<bool> sRecvStop{false};
unsigned int sIngressDropped = 0;

void recv_thread_main(PortSocket sock)
{
    while (!sRecvStop.load(std::memory_order_relaxed)) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sock, &rfds);
        timeval tv{};
        tv.tv_usec = 20000;
        int ready = select(static_cast<int>(sock) + 1, &rfds, nullptr, nullptr, &tv);
        if (ready <= 0) {
            continue;
        }
        IngressPacket packet;
#if defined(_WIN32)
        int from_len = sizeof(packet.from);
#else
        socklen_t from_len = sizeof(packet.from);
#endif
        int size = recvfrom(sock, reinterpret_cast<char *>(packet.bytes), sizeof(packet.bytes), 0,
                            reinterpret_cast<sockaddr *>(&packet.from), &from_len);
        if (size <= 0) {
            continue;   /* would-block race or socket error: keep draining */
        }
        packet.size = size;
        {
            std::lock_guard<std::mutex> lock(sIngressMutex);
            if (sIngress.size() < kIngressCap) {
                sIngress.push_back(packet);
            } else {
                sIngressDropped++;
            }
        }
    }
}

void start_recv_thread(void)
{
    if (sRecvThread.joinable()) {
        return;
    }
    sRecvStop.store(false, std::memory_order_relaxed);
    sRecvThread = std::thread(recv_thread_main, sSocket);
}

void stop_recv_thread(void)
{
    if (!sRecvThread.joinable()) {
        return;
    }
    sRecvStop.store(true, std::memory_order_relaxed);
    sRecvThread.join();     /* select() wakes within 20ms */
    std::lock_guard<std::mutex> lock(sIngressMutex);
    sIngress.clear();
}

/* Relay fallback: game packets travel through the lobby server instead of
 * UDP (src/networking/netplay/session/netlobby.cpp installs WebSocket send/receive hooks). */
port_udp_relay_send_fn sRelaySend = nullptr;
port_udp_relay_recv_fn sRelayRecv = nullptr;
bool sRelayFramed = false;   /* v2 protocol: relayed frames carry a slot byte */

/* Multi-peer links, keyed by remote game slot. */
struct Link {
    sockaddr_in addr = {};
    bool relay = false;
    bool active = false;
};
Link sLinks[PORT_UDP_LINK_MAX];

/* Network simulator (send side). */
unsigned int sSimLatencyMs = 0;
unsigned int sSimJitterMs = 0;
unsigned int sSimLossPct = 0;
unsigned int sSimDupPct = 0;
unsigned int sSimReorderPct = 0;
unsigned int sSimReorderWindowMs = 100;  /* extra delay a reordered packet picks up */
unsigned int sSimCorruptPct = 0;
std::mt19937 sSimRng(1);
std::deque<PendingPacket> sSimQueue;
bool sSimConfigured = false;

#ifdef SSB64_TEST_HOOKS
unsigned int env_uint(const char *name, unsigned int fallback)
{
    const char *value = std::getenv(name);
    return (value != nullptr) ? static_cast<unsigned int>(std::strtoul(value, nullptr, 10)) : fallback;
}
#endif

void configure_sim(void)
{
    if (sSimConfigured) {
        return;
    }
    sSimConfigured = true;
    /* Network impairment is a test harness: it deliberately drops, corrupts
     * and delays real traffic. Compiled in only for test builds
     * (-DSSB64_TEST_HOOKS=ON); shipped binaries never honor these vars. */
#ifdef SSB64_TEST_HOOKS
    sSimLatencyMs = env_uint("SSB64_NETPLAY_SIM_LATENCY_MS", 0);
    sSimJitterMs = env_uint("SSB64_NETPLAY_SIM_JITTER_MS", 0);
    sSimLossPct = env_uint("SSB64_NETPLAY_SIM_LOSS_PCT", 0);
    sSimDupPct = env_uint("SSB64_NETPLAY_SIM_DUP_PCT", 0);
    sSimReorderPct = env_uint("SSB64_NETPLAY_SIM_REORDER_PCT", 0);
    sSimReorderWindowMs = env_uint("SSB64_NETPLAY_SIM_REORDER_WINDOW_MS", 100);
    sSimCorruptPct = env_uint("SSB64_NETPLAY_SIM_CORRUPT_PCT", 0);
    sSimRng.seed(env_uint("SSB64_NETPLAY_SIM_SEED", 1));

    if (sSimLatencyMs || sSimJitterMs || sSimLossPct ||
        sSimDupPct || sSimReorderPct || sSimCorruptPct) {
        port_log("SSB64 UDP: simulating latency=%ums jitter=%ums loss=%u%% dup=%u%% reorder=%u%%(+%ums) corrupt=%u%%\n",
                 sSimLatencyMs, sSimJitterMs, sSimLossPct,
                 sSimDupPct, sSimReorderPct, sSimReorderWindowMs, sSimCorruptPct);
    }
#endif
}

bool startup(void)
{
#if defined(_WIN32)
    static bool sStarted = false;
    if (!sStarted) {
        WSADATA data;
        if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
            port_log("SSB64 UDP: WSAStartup failed\n");
            return false;
        }
        sStarted = true;
    }
#endif
    return true;
}

/* "host:port" -> sockaddr_in. "*" / "0.0.0.0" -> INADDR_ANY. Hostnames resolve. */
bool parse_address(const char *spec, sockaddr_in *out)
{
    if (spec == nullptr) {
        return false;
    }
    std::string text(spec);
    size_t colon = text.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= text.size()) {
        return false;
    }
    std::string host = text.substr(0, colon);
    long port = std::strtol(text.c_str() + colon + 1, nullptr, 10);
    if (port < 0 || port > 65535) {     /* 0 = let the OS pick (bind only) */
        return false;
    }
    std::memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons(static_cast<uint16_t>(port));

    if (host == "*" || host == "0.0.0.0") {
        out->sin_addr.s_addr = htonl(INADDR_ANY);
        return true;
    }
    if (inet_pton(AF_INET, host.c_str(), &out->sin_addr) == 1) {
        return true;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo *result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) != 0 || result == nullptr) {
        return false;
    }
    out->sin_addr = reinterpret_cast<sockaddr_in *>(result->ai_addr)->sin_addr;
    freeaddrinfo(result);
    return true;
}

int raw_send_to(const sockaddr_in &to, const uint8_t *data, int size)
{
    int sent = sendto(sSocket, reinterpret_cast<const char *>(data), size, 0,
                      reinterpret_cast<const sockaddr *>(&to), sizeof(to));
    if (sent != size) {
        static int sSendtoErrDiag = 8;
        if (sSendtoErrDiag-- > 0) {
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &to.sin_addr, ip, sizeof(ip));
            port_log("SSB64 UDP: sendto %s:%u failed err=%d size=%d\n", ip,
                     static_cast<unsigned>(ntohs(to.sin_port)), port_udp_last_error(), size);
        }
    }
    return (sent == size) ? size : -1;
}

/* Game traffic to the peer: direct UDP, or through the relay hook. */
int raw_send(const uint8_t *data, int size)
{
    if (sRelaySend != nullptr) {
        return sRelaySend(data, size) ? size : -1;
    }
    return raw_send_to(sPeer, data, size);
}

/* Routed send for links: relay_link >= 0 goes over the relay as [slot]
 * framed data (v2), relay_link == -2 is the legacy unframed relay route,
 * relay_link == -1 sends to the UDP endpoint `to`. */
void log_send_diag(const char *fmt, ...)
{
    static int sBudget = 40;
    if (sBudget <= 0) {
        return;
    }
    sBudget--;
    char buf[256];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    port_log("SSB64 UDP: %s\n", buf);
}

int route_send(const uint8_t *data, int size, const sockaddr_in *to, int relay_link)
{
    if (relay_link >= 0 || relay_link == -2) {
        if (sRelaySend == nullptr) {
            log_send_diag("route_send relay_link=%d dropped: no relay hook", relay_link);
            return -1;
        }
        if (relay_link < 0 || !sRelayFramed) {
            int ok = sRelaySend(data, size);
            if (!ok) log_send_diag("route_send unframed relay send failed size=%d", size);
            return ok ? size : -1;
        }
        uint8_t framed[1500];
        if (size + 1 > static_cast<int>(sizeof(framed))) {
            return -1;
        }
        framed[0] = static_cast<uint8_t>(relay_link);
        std::memcpy(framed + 1, data, size);
        int ok = sRelaySend(framed, size + 1);
        if (!ok) log_send_diag("route_send framed relay send failed to=%d size=%d", relay_link, size);
        return ok ? size : -1;
    }
    return raw_send_to(*to, data, size);
}

bool same_endpoint(const sockaddr_in &a, const sockaddr_in &b)
{
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

/* Receives one datagram. kind: 0 peer/direct, 1 relayed from the peer,
 * 2 anyone else. Returns size, 0 none, -1 error. Relayed packets come
 * first so the relay can't be starved by stray UDP. With framed relay the
 * returned buffer keeps the leading [from] slot byte; callers that
 * understand v2 (link_recv/recv_from) strip it themselves. */
int raw_recv(void *buffer, int capacity, sockaddr_in *from, int *kind)
{
    if (sRelayRecv != nullptr) {
        int relayed = sRelayRecv(buffer, capacity);
        if (relayed > 0) {
            std::memset(from, 0, sizeof(*from));
            *kind = 1;
            return relayed;
        }
    }
    /* The recv thread already pulled the datagram out of the socket; this
     * just dequeues it. The socket error path is gone: errors are counted
     * on the thread, and oversized datagrams can never arrive here since
     * the thread reads into a full-size buffer (a stray >2KB datagram would
     * have failed recvfrom outright on Windows rather than truncating and
     * wedging the socket). */
    IngressPacket packet;
    unsigned int dropped;
    {
        std::lock_guard<std::mutex> lock(sIngressMutex);
        if (sIngress.empty()) {
            return 0;
        }
        packet = std::move(sIngress.front());
        sIngress.pop_front();
        dropped = sIngressDropped;
        sIngressDropped = 0;
    }
    if (dropped != 0) {
        port_log("SSB64 UDP: ingress queue full — dropped %u datagrams\n", dropped);
    }
    *from = packet.from;
    int size = packet.size;
    *kind = (sHasPeer && same_endpoint(*from, sPeer)) ? 0 : 2;
    if (size > capacity) {
        return -1;  /* not one of ours: every protocol datagram fits its caller's buffer */
    }
    std::memcpy(buffer, packet.bytes, size);
    return size;
}

void flush_sim_queue(void)
{
    unsigned int now = port_udp_now_ms();
    while (!sSimQueue.empty() && static_cast<int>(now - sSimQueue.front().release_ms) >= 0) {
        PendingPacket &front = sSimQueue.front();
        route_send(front.bytes.data(), static_cast<int>(front.bytes.size()),
                   front.has_to ? &front.to : nullptr, front.relay_link);
        sSimQueue.pop_front();
    }
}

/* Shared send path for port_udp_send / port_udp_link_send: applies the
 * simulated network conditions, then routes to an endpoint or relay link.
 * In order: loss drops, corruption flips one random byte (integrity checks
 * downstream are expected to reject it), duplication schedules a twin, and
 * reorder holds the packet long enough to overtake later sends — all driven
 * by the seeded sim rng so runs are reproducible. */
int send_routed(const uint8_t *bytes, int size, const sockaddr_in *to, int relay_link)
{
    if (sSimLossPct != 0 && (sSimRng() % 100u) < sSimLossPct) {
        return size;
    }
    std::vector<uint8_t> corrupted;
    const uint8_t *payload = bytes;
    if (sSimCorruptPct != 0 && size > 0 && (sSimRng() % 100u) < sSimCorruptPct) {
        corrupted.assign(bytes, bytes + size);
        corrupted[sSimRng() % corrupted.size()] ^= static_cast<uint8_t>(1u + (sSimRng() % 0xFFu));
        payload = corrupted.data();
    }
    unsigned int delay = sSimLatencyMs + ((sSimJitterMs != 0) ? (sSimRng() % (sSimJitterMs + 1)) : 0);
    if (sSimReorderPct != 0 && (sSimRng() % 100u) < sSimReorderPct) {
        delay += 1 + (sSimRng() % sSimReorderWindowMs);
    }
    const bool duplicate = (sSimDupPct != 0) && (sSimRng() % 100u) < sSimDupPct;
    if (delay == 0 && !duplicate) {
        return route_send(payload, size, to, relay_link);
    }
    PendingPacket packet;
    packet.release_ms = port_udp_now_ms() + delay;
    packet.bytes.assign(payload, payload + size);
    if (to != nullptr) {
        packet.has_to = true;
        packet.to = *to;
    }
    packet.relay_link = relay_link;

    /* Jitter/reorder may reorder packets, exactly like a real network. */
    auto it = sSimQueue.end();
    while (it != sSimQueue.begin() && static_cast<int>((it - 1)->release_ms - packet.release_ms) > 0) {
        --it;
    }
    it = sSimQueue.insert(it, std::move(packet));
    if (duplicate) {
        /* the twin shares the release time: it ships back-to-back with the
         * original, the way a dup lands on the wire */
        sSimQueue.insert(it + 1, *it);
    }
    {
        static size_t sSimHighWater = 0;
        if (sSimQueue.size() > sSimHighWater + 256) {
            sSimHighWater = sSimQueue.size();
            port_log("SSB64 UDP: sim queue depth %zu\n", sSimQueue.size());
        }
    }
    flush_sim_queue();
    return size;
}

} // namespace

extern "C" int port_udp_open(const char *bind_spec)
{
    sockaddr_in bind_addr;

    configure_sim();

    if (sSocket != kInvalidSocket) {
        return 1;
    }
    if (!startup()) {
        return 0;
    }
    if (!parse_address(bind_spec, &bind_addr)) {
        port_log("SSB64 UDP: invalid bind address '%s' (expected host:port)\n", bind_spec ? bind_spec : "(null)");
        return 0;
    }
    sSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sSocket == kInvalidSocket) {
        port_log("SSB64 UDP: socket failed err=%d\n", port_udp_last_error());
        return 0;
    }
    if (bind(sSocket, reinterpret_cast<const sockaddr *>(&bind_addr), sizeof(bind_addr)) != 0) {
        port_log("SSB64 UDP: bind %s failed err=%d\n", bind_spec, port_udp_last_error());
        port_udp_close();
        return 0;
    }
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (ioctlsocket(sSocket, FIONBIO, &nonblocking) != 0) {
        port_log("SSB64 UDP: nonblocking setup failed err=%d\n", port_udp_last_error());
        port_udp_close();
        return 0;
    }
    /* Without this, an ICMP port-unreachable from a peer that isn't up yet
     * makes the next recvfrom fail with WSAECONNRESET. */
    BOOL report_reset = FALSE;
    DWORD returned = 0;
    WSAIoctl(sSocket, SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), nullptr, 0, &returned, nullptr, nullptr);
#else
    int flags = fcntl(sSocket, F_GETFL, 0);
    if (flags < 0 || fcntl(sSocket, F_SETFL, flags | O_NONBLOCK) != 0) {
        port_log("SSB64 UDP: nonblocking setup failed err=%d\n", port_udp_last_error());
        port_udp_close();
        return 0;
    }
#endif
    start_recv_thread();
    return 1;
}

extern "C" int port_udp_set_peer(const char *peer_spec)
{
    if (!startup() || !parse_address(peer_spec, &sPeer) || sPeer.sin_addr.s_addr == htonl(INADDR_ANY) || sPeer.sin_port == 0) {
        port_log("SSB64 UDP: invalid peer address '%s' (expected host:port)\n", peer_spec ? peer_spec : "(null)");
        sHasPeer = false;
        return 0;
    }
    sHasPeer = true;
    return 1;
}

extern "C" void port_udp_close(void)
{
    stop_recv_thread();     /* join before closing: no recv on a dead socket */
    if (sSocket != kInvalidSocket) {
        port_udp_close_socket(sSocket);
        sSocket = kInvalidSocket;
    }
    sSimQueue.clear();
    sRelaySend = nullptr;
    sRelayRecv = nullptr;
    sRelayFramed = false;
    port_udp_link_clear_all();
    sHasPeer = false;
}

extern "C" int port_udp_is_open(void)
{
    return sSocket != kInvalidSocket;
}

extern "C" int port_udp_send(const void *data, int size)
{
    if (sSocket == kInvalidSocket || (!sHasPeer && sRelaySend == nullptr) || data == nullptr || size <= 0) {
        return -1;
    }
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    return send_routed(bytes, size, sHasPeer ? &sPeer : nullptr, (sRelaySend != nullptr) ? -2 : -1);
}

extern "C" int port_udp_recv(void *buffer, int capacity)
{
    if (sSocket == kInvalidSocket || buffer == nullptr || capacity <= 0) {
        return -1;
    }
    flush_sim_queue();

    for (;;) {
        sockaddr_in from;
        int kind = 2;
        int size = raw_recv(buffer, capacity, &from, &kind);

        if (size <= 0) {
            return size;
        }
        if (sHasPeer && kind == 2) {
            continue; /* not our peer (stray lobby/server traffic) */
        }
        return size;
    }
}

extern "C" int port_udp_send_to(const char *spec, const void *data, int size)
{
    sockaddr_in to;

    if (sSocket == kInvalidSocket || data == nullptr || size <= 0 || !parse_address(spec, &to)) {
        return -1;
    }
    return raw_send_to(to, static_cast<const uint8_t *>(data), size);
}

extern "C" int port_udp_recv_from(void *buffer, int capacity, char *from_text, int from_capacity)
{
    sockaddr_in from;
    int kind = 2;

    if (sSocket == kInvalidSocket || buffer == nullptr || capacity <= 0) {
        return -1;
    }
    flush_sim_queue();

    uint8_t bytes[2048];
    int size = raw_recv(bytes, sizeof(bytes), &from, &kind);
    if (size <= 0) {
        return size;
    }
    int skip = (kind == 1 && sRelayFramed) ? 1 : 0;
    if (size - skip > capacity) {
        return -1;
    }
    std::memcpy(buffer, bytes + skip, size - skip);
    if (from_text != nullptr && from_capacity > 0) {
        if (kind == 1) {
            if (sRelayFramed && size >= 1) {
                std::snprintf(from_text, from_capacity, "relay:%u", bytes[0]);
            } else {
                std::snprintf(from_text, from_capacity, "relay");
            }
        } else {
            char ip[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
            std::snprintf(from_text, from_capacity, "%s:%u", ip, static_cast<unsigned>(ntohs(from.sin_port)));
        }
    }
    return size - skip;
}

extern "C" void port_udp_set_relay_hooks(port_udp_relay_send_fn send, port_udp_relay_recv_fn recv)
{
    sRelaySend = send;
    sRelayRecv = recv;
}

/* ---- multi-peer links (v2 lobby protocol) ---- */

extern "C" int port_udp_link_set(int link, const char *spec)
{
    if (link < 0 || link >= PORT_UDP_LINK_MAX) {
        return 0;
    }
    if (spec != nullptr && std::strcmp(spec, "relay") == 0) {
        sLinks[link].relay = true;
        sLinks[link].active = true;
        return 1;
    }
    if (!startup() || !parse_address(spec, &sLinks[link].addr) ||
        sLinks[link].addr.sin_addr.s_addr == htonl(INADDR_ANY) || sLinks[link].addr.sin_port == 0) {
        port_log("SSB64 UDP: invalid link address '%s' (expected host:port)\n", spec ? spec : "(null)");
        return 0;
    }
    sLinks[link].relay = false;
    sLinks[link].active = true;
    return 1;
}

extern "C" void port_udp_link_clear(int link)
{
    if (link >= 0 && link < PORT_UDP_LINK_MAX) {
        sLinks[link] = Link{};
    }
}

extern "C" void port_udp_link_clear_all(void)
{
    for (Link &link : sLinks) {
        link = Link{};
    }
}

extern "C" int port_udp_link_is_set(int link)
{
    return (link >= 0 && link < PORT_UDP_LINK_MAX && sLinks[link].active) ? 1 : 0;
}

extern "C" int port_udp_is_relay(void)
{
    return (sRelaySend != nullptr) ? 1 : 0;
}

extern "C" void port_udp_set_relay_framed(int framed)
{
    sRelayFramed = (framed != 0);
}

extern "C" int port_udp_link_is_relay(int link)
{
    return (link >= 0 && link < PORT_UDP_LINK_MAX && sLinks[link].active && sLinks[link].relay) ? 1 : 0;
}

extern "C" int port_udp_link_send(int link, const void *data, int size)
{
    if (sSocket == kInvalidSocket || data == nullptr || size <= 0 ||
        link < 0 || link >= PORT_UDP_LINK_MAX || !sLinks[link].active) {
        log_send_diag("link_send link=%d dropped: socket=%d size=%d active=%d",
                      link, (int)(sSocket != kInvalidSocket), size,
                      (link >= 0 && link < PORT_UDP_LINK_MAX) ? (int)sLinks[link].active : -1);
        return -1;
    }
    const uint8_t *bytes = static_cast<const uint8_t *>(data);
    const Link &l = sLinks[link];
    return send_routed(bytes, size, l.relay ? nullptr : &l.addr, l.relay ? link : -1);
}

/* Maps a datagram source to its link: framed relay traffic carries the
 * remote slot, direct UDP matches the link's endpoint. The framing byte
 * is only trusted as far as the link table allows: a relayed frame that
 * claims a slot with no active relay-backed link is dropped rather than
 * routed to a link the attacker picked. */
int link_of(const sockaddr_in *from, int kind, const uint8_t *bytes, int size)
{
    if (kind == 1) {
        if (sRelayFramed) {
            const int link = (size >= 1 && bytes[0] < PORT_UDP_LINK_MAX) ? bytes[0] : -1;
            return (link >= 0 && sLinks[link].active && sLinks[link].relay) ? link : -1;
        }
        for (int i = 0; i < PORT_UDP_LINK_MAX; i++) {
            if (sLinks[i].active && sLinks[i].relay) {
                return i;
            }
        }
        return -1;
    }
    for (int i = 0; i < PORT_UDP_LINK_MAX; i++) {
        if (sLinks[i].active && !sLinks[i].relay && same_endpoint(*from, sLinks[i].addr)) {
            return i;
        }
    }
    return -1;
}

extern "C" int port_udp_link_recv(int *out_link, void *buffer, int capacity)
{
    if (sSocket == kInvalidSocket || buffer == nullptr || capacity <= 0 || out_link == nullptr) {
        return -1;
    }
    flush_sim_queue();

    uint8_t bytes[2048];
    for (;;) {
        sockaddr_in from;
        int kind = 2;
        int size = raw_recv(bytes, sizeof(bytes), &from, &kind);

        if (size <= 0) {
            return size;
        }
        int link = link_of(&from, kind, bytes, size);
        int payload = (kind == 1 && sRelayFramed) ? size - 1 : size;

        if (link < 0 || payload <= 0 || payload > capacity) {
            static unsigned int sUnrouteable = 0;
            if ((++sUnrouteable % 300u) == 1u || sUnrouteable < 4u) {
                char ip[INET_ADDRSTRLEN] = "?";
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                port_log("SSB64 UDP: unrouteable #%u kind=%d from=%s:%u link=%d size=%d head=%02X%02X\n",
                         sUnrouteable, kind, ip, static_cast<unsigned>(ntohs(from.sin_port)),
                         link, size, size > 0 ? bytes[0] : 0, size > 1 ? bytes[1] : 0);
            }
            continue;   /* stray, oversized or unrouteable: keep draining */
        }
        std::memmove(buffer, bytes + (size - payload), payload);
        *out_link = link;
        return payload;
    }
}

extern "C" int port_udp_local_endpoint(char *out, int capacity)
{
    sockaddr_in bound;
#if defined(_WIN32)
    int len = sizeof(bound);
#else
    socklen_t len = sizeof(bound);
#endif
    if (sSocket == kInvalidSocket || out == nullptr ||
        getsockname(sSocket, reinterpret_cast<sockaddr *>(&bound), &len) != 0) {
        return 0;
    }
    /* Route-selection trick: "connecting" a throwaway UDP socket sends
     * nothing but makes the OS pick the outbound interface address. */
    in_addr local = bound.sin_addr;
    PortSocket probe = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probe != kInvalidSocket) {
        sockaddr_in remote;
        std::memset(&remote, 0, sizeof(remote));
        remote.sin_family = AF_INET;
        remote.sin_port = htons(53);
        inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
        if (connect(probe, reinterpret_cast<sockaddr *>(&remote), sizeof(remote)) == 0) {
            sockaddr_in name;
#if defined(_WIN32)
            int name_len = sizeof(name);
#else
            socklen_t name_len = sizeof(name);
#endif
            if (getsockname(probe, reinterpret_cast<sockaddr *>(&name), &name_len) == 0) {
                local = name.sin_addr;
            }
        }
        port_udp_close_socket(probe);
    }
    char ip[INET_ADDRSTRLEN] = "127.0.0.1";
    inet_ntop(AF_INET, &local, ip, sizeof(ip));
    std::snprintf(out, capacity, "%s:%u", ip, static_cast<unsigned>(ntohs(bound.sin_port)));
    return 1;
}

extern "C" void port_udp_sleep_us(unsigned int usecs)
{
    std::this_thread::sleep_for(std::chrono::microseconds(usecs));
}

extern "C" unsigned int port_udp_now_ms(void)
{
    using namespace std::chrono;
    return static_cast<unsigned int>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/* ---- STUN probe (connection assistant) -----------------------------------
 *
 * RFC 5389 binding requests on an ephemeral socket of its own — the probe
 * never touches sSocket, so it is safe to run while a session is live.
 * Async contract (one poll per frame from the menu):
 *   start(specs, n) → poll() until it returns 0 → result(i, …) / close().
 * Sending the same txid to two different servers and comparing the mapped
 * ports is the classic cheap symmetric-NAT test: equal ports mean the NAT
 * keeps the mapping stable and hole punching stands a chance; differing
 * ports mean the peer will likely fall back to the lobby relay. */

namespace {

PortSocket sStunSock = kInvalidSocket;
uint8_t sStunTxid[12];
sockaddr_in sStunTargets[PORT_UDP_STUN_MAX];
char sStunMapped[PORT_UDP_STUN_MAX][48];   /* "ip:port" per server, "" while unanswered */
int sStunCount = 0;
unsigned int sStunStartMs = 0;
unsigned int sStunNextResendMs = 0;
constexpr unsigned int kStunResendMs = 400;
constexpr unsigned int kStunTimeoutMs = 2000;

void stun_send_all(void)
{
    uint8_t req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
    std::memcpy(req + 8, sStunTxid, 12);
    for (int i = 0; i < sStunCount; i++) {
        if (sStunMapped[i][0] == '\0') {
            sendto(sStunSock, reinterpret_cast<const char *>(req), sizeof(req), 0,
                   reinterpret_cast<const sockaddr *>(&sStunTargets[i]), sizeof(sStunTargets[i]));
        }
    }
}

/* Binding response header: type 0x0101, magic cookie, matching txid. */
int stun_header_ok(const uint8_t *msg, int size)
{
    return size >= 20 && msg[0] == 0x01 && msg[1] == 0x01 &&
        msg[4] == 0x21 && msg[5] == 0x12 && msg[6] == 0xA4 && msg[7] == 0x42 &&
        std::memcmp(msg + 8, sStunTxid, 12) == 0;
}

/* Decodes one (XOR-)MAPPED-ADDRESS attribute value into "ip:port". */
int stun_attr_mapped(const uint8_t *v, int type, int alen, char *out, int cap)
{
    if ((type != 0x0020 && type != 0x0001) || alen < 8 || v[1] != 0x01) {
        return 0;
    }
    uint16_t port = static_cast<uint16_t>((v[2] << 8) | v[3]);
    uint32_t addr = (static_cast<uint32_t>(v[4]) << 24) | (v[5] << 16) | (v[6] << 8) | v[7];
    if (type == 0x0020) {
        port ^= static_cast<uint16_t>(0x2112);
        addr ^= 0x2112A442u;
    }
    std::snprintf(out, static_cast<size_t>(cap), "%u.%u.%u.%u:%u",
                  addr >> 24, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, port);
    return 1;
}

/* Parses a binding response's (XOR-)MAPPED-ADDRESS into "ip:port". */
int stun_parse_mapped(const uint8_t *msg, int size, char *out, int cap)
{
    if (!stun_header_ok(msg, size)) {
        return 0;
    }
    const int length = (msg[2] << 8) | msg[3];
    const int end = (20 + length < size) ? 20 + length : size;
    for (int at = 20; at + 4 <= end;) {
        const int type = (msg[at] << 8) | msg[at + 1];
        const int alen = (msg[at + 2] << 8) | msg[at + 3];
        if (at + 4 + alen > end) {
            break;
        }
        if (stun_attr_mapped(msg + at + 4, type, alen, out, cap)) {
            return 1;
        }
        at += 4 + ((alen + 3) & ~3);
    }
    return 0;
}

}   // namespace

extern "C" int port_udp_stun_probe_start(const char *const *specs, int count)
{
    port_udp_stun_probe_close();
    if (specs == nullptr || count <= 0 || !startup()) {
        return 0;
    }
    for (int i = 0; i < count && i < PORT_UDP_STUN_MAX; i++) {
        if (specs[i] == nullptr || !parse_address(specs[i], &sStunTargets[sStunCount])) {
            continue;
        }
        sStunCount++;
    }
    if (sStunCount == 0) {
        return 0;
    }
    sStunSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sStunSock == kInvalidSocket) {
        return 0;
    }
#if defined(_WIN32)
    u_long nonblocking = 1;
    ioctlsocket(sStunSock, FIONBIO, &nonblocking);
    BOOL report_reset = FALSE;
    DWORD returned = 0;
    WSAIoctl(sStunSock, SIO_UDP_CONNRESET, &report_reset, sizeof(report_reset), nullptr, 0, &returned, nullptr, nullptr);
#else
    int flags = fcntl(sStunSock, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(sStunSock, F_SETFL, flags | O_NONBLOCK);
    }
#endif
    /* Txid needs uniqueness between consecutive probes, not secrecy. */
    unsigned int seed = port_udp_now_ms();
    for (int i = 0; i < 12; i++) {
        seed = seed * 1103515245u + 12345u + static_cast<unsigned int>(i);
        sStunTxid[i] = static_cast<uint8_t>(seed >> 16);
    }
    sStunStartMs = port_udp_now_ms();
    sStunNextResendMs = sStunStartMs;
    stun_send_all();
    return 1;
}

/* Returns 1 while any server is still unanswered inside the timeout, 0 when
 * the probe is finished (every answer collected or deadline hit). */
extern "C" int port_udp_stun_probe_poll(void)
{
    if (sStunSock == kInvalidSocket) {
        return 0;
    }
    const unsigned int now = port_udp_now_ms();
    uint8_t buf[512];
    for (;;) {
        sockaddr_in from;
#if defined(_WIN32)
        int from_len = sizeof(from);
#else
        socklen_t from_len = sizeof(from);
#endif
        const int n = recvfrom(sStunSock, reinterpret_cast<char *>(buf), sizeof(buf), 0,
                               reinterpret_cast<sockaddr *>(&from), &from_len);
        if (n <= 0) {
            break;
        }
        for (int i = 0; i < sStunCount; i++) {
            if (sStunMapped[i][0] == '\0' && from.sin_addr.s_addr == sStunTargets[i].sin_addr.s_addr &&
                from.sin_port == sStunTargets[i].sin_port) {
                stun_parse_mapped(buf, n, sStunMapped[i], sizeof(sStunMapped[i]));
            }
        }
    }
    int pending = 0;
    for (int i = 0; i < sStunCount; i++) {
        pending += (sStunMapped[i][0] == '\0') ? 1 : 0;
    }
    if (pending == 0) {
        return 0;
    }
    if (now - sStunStartMs >= kStunTimeoutMs) {
        return 0;
    }
    if (now >= sStunNextResendMs) {
        stun_send_all();
        sStunNextResendMs = now + kStunResendMs;
    }
    return 1;
}

extern "C" int port_udp_stun_probe_result(int index, char *out, int capacity)
{
    if (out == nullptr || capacity <= 0 || index < 0 || index >= sStunCount || sStunMapped[index][0] == '\0') {
        return 0;
    }
    std::snprintf(out, static_cast<size_t>(capacity), "%s", sStunMapped[index]);
    return 1;
}

extern "C" void port_udp_stun_probe_close(void)
{
    if (sStunSock != kInvalidSocket) {
        port_udp_close_socket(sStunSock);
        sStunSock = kInvalidSocket;
    }
    sStunCount = 0;
    for (int i = 0; i < PORT_UDP_STUN_MAX; i++) {
        sStunMapped[i][0] = '\0';
    }
}
