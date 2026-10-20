/* Signaling transport: STUN, control/uplink websockets, relay hooks — split from netlobby.cpp; the wire-protocol overview lives there. */
#include "netlobby_internal.h"
#include <cctype>
/* State-free helpers (compat token, env, split, rules reads)
 * live in netlobby_util.cpp; wire JSON parsing in lobby_json.cpp. */
using namespace ssb64::netlobby;

namespace ssb64::netlobby {

void shutdown_net(void)
{
    port_ws_uplink_close();
    port_ws_close();
    port_udp_set_relay_hooks(nullptr, nullptr);
    port_udp_close();
    for (Peer &p : sLobby.peers) {
        p = Peer{};
    }
    sLobby.is_host = false;
    sLobby.lan = false;
    sLobby.spectate = false;
    sLobby.quickplay = false;
    sLobby.my_game_slot = -1;
    sLobby.ws_welcomed = false;
    sLobby.endpoint_sent = false;
    sLobby.stun_done = false;
    sLobby.stun_attempt = 0;
    sLobby.stun_public.clear();
    sLobby.player_count = 1;
    sLobby.my_slot = sLobby.is_host ? 0 : 1;
    sLobby.session = 0;
    sLobby.up_key.clear();
    sLobby.start_sent_ms = 0;
    sLobby.last_start_ms = 0;
    sLobby.ping_sent_ms = 0;    /* per-session; lobby_rtt_ms itself survives */
    sLobby.my_ready = false;
}

void fail(const std::string &why)
{
    shutdown_net();
    set_state(PORT_NETLOBBY_FAILED, why);
}

/* ---- relay over the lobby WebSocket ---- */

bool diag_relay(void)
{
    return env("SSB64_NETPLAY_DEBUG_STAGES") != nullptr;
}

int relay_send(const void *data, int size)
{
    if (diag_relay() && size >= 9) {
        const unsigned char *b = static_cast<const unsigned char *>(data);
        unsigned ptype = (static_cast<unsigned>(b[7]) << 8) | b[8];
        port_log("SSB64 Lobby: relay send %d bytes to_slot=%u ptype=%u\n", size, b[0], ptype);
    }
    /* port_ws_send_binary itself prefers the uplink once it is open */
    return port_ws_send_binary(data, size);
}

int relay_recv(void *buffer, int capacity)
{
    int n = port_ws_poll_binary(buffer, capacity);
    if (n > 0 && diag_relay()) {
        const unsigned char *b = static_cast<const unsigned char *>(buffer);
        unsigned ptype = (n >= 9) ? ((static_cast<unsigned>(b[7]) << 8) | b[8]) : 0;
        port_log("SSB64 Lobby: relay recv %d bytes from_slot=%u ptype=%u\n", n, b[0], ptype);
    }
    return n;
}

/* ---- STUN (RFC 5389 binding request; server list in netlobby_internal.h) ---- */

constexpr uint32_t kStunCookie = 0x2112A442;

const char *stun_server(void)
{
    /* CVar override wins outright (LAN play behind exotic NATs); otherwise
     * walk kStunServers by attempt so a dead provider just costs a retry. */
    if (const char *ov = CVarGetString("gNetplay.StunServer", ""); ov != nullptr && *ov != '\0') {
        return ov;
    }
    return kStunServers[sLobby.stun_attempt % kStunServerCount];
}

void send_stun(void)
{
    uint8_t req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
    std::memcpy(req + 8, sLobby.stun_txid, 12);
    sLobby.stun_ms = port_udp_now_ms();
    port_udp_send_to(stun_server(), req, sizeof(req));
}

static bool stun_check_header(const uint8_t *msg, int size)
{
    return size >= 20 && msg[0] == 0x01 && msg[1] == 0x01 &&
           msg[4] == 0x21 && msg[5] == 0x12 && msg[6] == 0xA4 && msg[7] == 0x42 &&
           std::memcmp(msg + 8, sLobby.stun_txid, 12) == 0;
}

/* (XOR-)MAPPED-ADDRESS, IPv4 — returns true when the endpoint was captured. */
static void stun_apply_mapped_addr(int type, const uint8_t *v)
{
    uint16_t port = static_cast<uint16_t>((v[2] << 8) | v[3]);
    uint32_t addr = (static_cast<uint32_t>(v[4]) << 24) | (v[5] << 16) | (v[6] << 8) | v[7];
    if (type == 0x0020) {
        port ^= static_cast<uint16_t>(kStunCookie >> 16);
        addr ^= kStunCookie;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", addr >> 24, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF,
                  addr & 0xFF, port);
    sLobby.stun_public = text;
    sLobby.stun_done = true;
    port_log("SSB64 Lobby: public UDP endpoint %s\n", text);
}

bool parse_stun(const uint8_t *msg, int size)
{
    if (!stun_check_header(msg, size)) {
        return false;
    }
    if (sLobby.stun_done) {
        return true;    // answer to a retransmitted request
    }
    int length = (msg[2] << 8) | msg[3];
    int end = (20 + length < size) ? 20 + length : size;
    for (int at = 20; at + 4 <= end;) {
        int type = (msg[at] << 8) | (msg[at + 1]);
        int alen = (msg[at + 2] << 8) | msg[at + 3];
        const uint8_t *v = msg + at + 4;
        if (at + 4 + alen > end) {
            break;
        }
        if ((type == 0x0020 || type == 0x0001) && alen >= 8 && v[1] == 0x01) {   // (XOR-)MAPPED-ADDRESS, IPv4
            stun_apply_mapped_addr(type, v);
            return true;
        }
        at += 4 + ((alen + 3) & ~3);
    }
    return false;
}



/* ---- server (WebSocket) ---- */

std::string new_code(void)
{
    std::string code;
    for (int i = 0; i < 6; i++) {
        code += kCodeAlphabet[sLobby.rng() % 32];
    }
    return code;
}

/* Room codes — user input or the server's "room" field — land raw in the
 * query string. Keep them inside the code alphabet so a stray '&'/'='/space
 * can't smuggle extra params or corrupt the URL. */
static std::string url_safe_code(const std::string &code)
{
    std::string out;
    for (char c : code) {
        if (std::strchr(kCodeAlphabet, c) != nullptr) {
            out += c;
        }
    }
    return out;
}

void connect_server(void)
{
    std::string code = url_safe_code(sLobby.code);
    std::string url = sLobby.server;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/ws?room=" + code + "&role=" +
           (sLobby.quickplay ? "queue"
            : (sLobby.spectate ? "spec" : (sLobby.is_host ? "host" : "join"))) + "&v=2";
    /* Matchmaking telemetry: report the lobby-path RTT we measured on a
     * prior session (ping -> keepalive_ack). First-ever connects carry no
     * hint; the server only logs it — population is too thin to partition
     * by latency yet, so this is instrumentation-first. */
    if (sLobby.quickplay && sLobby.lobby_rtt_ms > 0 && sLobby.lobby_rtt_ms < 10000) {
        url += "&ms=" + std::to_string(sLobby.lobby_rtt_ms);
    }
    sLobby.ws_welcomed = false;
    sLobby.endpoint_sent = false;
    port_ws_connect(url.c_str());
}

/* Second, send-only websocket for relayed game traffic. A synchronous ws
 * send on the control socket serializes behind its pending receive (measured
 * multi-second stalls), so the uplink never issues a receive. v2 rooms only. */
void open_uplink(void)
{
    if (sLobby.server_version < 2) {
        return;
    }
    /* up_key is server-issued and lands raw in the query string — a stray
     * '&'/'='/space would smuggle extra params or corrupt the URL. Keep
     * only URL-safe characters; drop anything else. */
    std::string key;
    for (char c : sLobby.up_key) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            key += c;
        }
    }
    std::string url = sLobby.server;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/ws?room=" + url_safe_code(sLobby.code) + "&role=join&v=2&up=1&slot=" +
           std::to_string(sLobby.my_slot) + "&key=" + key;
    port_ws_uplink_connect(url.c_str());
}

void send_endpoint_if_ready(void)
{
    if (!sLobby.ws_welcomed || sLobby.endpoint_sent || sLobby.spectate) {
        return;    /* spectators never punch, so they publish no endpoint */
    }
    if (!sLobby.stun_done) {
        unsigned int now = port_udp_now_ms();
        if (sLobby.stun_ms == 0 || now - sLobby.stun_ms >= 400) {
            send_stun();    // keep asking until the public endpoint is known
        }
        if (now - sLobby.stun_ms > kStunGiveUpMs && sLobby.stun_ms != 0) {
            if (sLobby.stun_attempt + 1 < kStunServerCount) {
                /* primary server silent — try the fallback before giving up */
                sLobby.stun_attempt++;
                sLobby.stun_ms = port_udp_now_ms();
                port_log("SSB64 Lobby: STUN %s silent, retrying %s\n",
                         kStunServers[sLobby.stun_attempt - 1], stun_server());
                send_stun();
            } else {
                sLobby.stun_done = true;    // proceed with just the LAN endpoint
                sLobby.stun_public = "-";
                port_log("SSB64 Lobby: STUN gave up, LAN endpoint only\n");
            }
        }
        return;
    }
    std::string msg = "{\"type\":\"endpoint\",\"public\":\"" + sLobby.stun_public +
                      "\",\"local\":\"" + local_endpoint() + "\"}";
    if (port_ws_send_text(msg.c_str())) {
        sLobby.endpoint_sent = true;
        sLobby.last_keepalive_ms = port_udp_now_ms();
    }
}

/* Ready-check rides the v2 roster broadcast: {type:"ready",ready:0|1} -> the
 * room stamps it on our slot and everyone sees it on the next "peers". v1
 * and LAN rooms have no roster - their PICK handshake stays the gate, so this
 * is a no-op there (and spectators never ready-up). */
void send_ready_state(void)
{
    if (sLobby.server_version != 2 || sLobby.lan || sLobby.spectate || !sLobby.ws_welcomed) {
        return;
    }
    char msg[64];
    std::snprintf(msg, sizeof(msg), "{\"type\":\"ready\",\"ready\":%d}", sLobby.my_ready ? 1 : 0);
    port_ws_send_text(msg);
}

/* ---- wire format ----
 * Lobby packets are plain text "SSBL <VERB> ..." so the relay stream and the
 * UDP stream can share one parser. During a match the engine speaks its own
 * binary "SSNP" protocol on the same socket/links. */

void send_to_peer(Peer &p, const char *text)
{
    int size = static_cast<int>(std::strlen(text)) + 1;
    if (port_udp_link_is_set(p.slot)) {
        port_udp_link_send(p.slot, text, size);
        return;
    }
    /* not connected yet: spray the candidates */
    for (const std::string &endpoint : p.candidates) {
        port_udp_send_to(endpoint.c_str(), text, size);
    }
}

} // namespace ssb64::netlobby
