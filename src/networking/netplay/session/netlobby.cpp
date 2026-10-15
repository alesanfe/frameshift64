/* See netlobby.h.
 *
 * Lobby server: services/netplay-lobby (Cloudflare Worker + Durable Object),
 * reached over WebSocket: wss://<worker>/ws?room=CODE&role=host|join.
 * The game's UDP endpoint is learned from Cloudflare's public STUN server
 * and exchanged through the Worker; the players then hole-punch a direct
 * UDP path. If that fails the Worker relays game packets over the
 * WebSocket.
 *
 * Star topology: every client links only to the host, which forwards each
 * client's input datagrams to the others (FORWARD packets inside the
 * engine protocol), so client<->client NAT punching is never needed.
 *
 * Peer-to-peer lobby messages ("SSBL" lines, over direct UDP or relay):
 *   SSBL HELLO | SSBL HELLOACK                punch / keepalive
 *   SSBL PICK <fkind> <time_ms> <tapjump_off> <version> [<build_sha>:<mod_hash>:<asset_hash>]
 *                                             character pick + RTT probe + own tap-jump setting + game version
 *                                             + session manifest (build sha + mod content hash + asset
 *                                             archive content hash; absent on pre-handshake builds)
 *   SSBL PONG <time_ms>                       immediate echo of a PICK's time
 *   SSBL START <stage> <stocks> <items> <seed> <delay> <hostfk> <hostcos> <clientfk> <clientcos>
 *              <autoz> <comp> <neutral> <hazards_off> <host_tapjump_off> <client_tapjump_off> [<session>]
 *                                             legacy 1v1 start (v1 rooms); the trailing session id is
 *                                             optional so older builds still interop (they use the seed)
 *   SSBL STARTM <stage> <stocks> <items> <seed> <delay> <autoz> <comp> <neutral> <hazards_off>
 *               <player_count> [<room_slot> <fk> <cos> <tapjump_off>] x player_count <session>
 *                                             multi-player start (v2 rooms). Entries are sorted by
 *                                             room slot; a player's position in the list is its game
 *                                             slot (room slots can be sparse when someone leaves). The
 *                                             session id is host-generated and independent of the seed:
 *                                             packets from another match can't be replayed into this
 *                                             one even when the seed is reused.
 *   SSBL GO <seed>                            client accepted the start packet */
#include "netlobby.h"
#include "udp_transport.h"
#include "websocket_transport.h"
#include "port_log.h"
#include "netrules.h"

#include <libultraship/bridge/consolevariablebridge.h>

#ifndef DISABLE_SCRIPTING
#include "game/modding/ModRegistry.h"
#endif

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include <ship/Context.h>

extern "C" void syNetPeerStartOnlineMatch(int local_slot, int player_count, unsigned int delay,
                                          unsigned int stage, unsigned int stocks, int items,
                                          unsigned int seed, unsigned int session,
                                          const int *fkinds, const int *costumes,
                                          const int *wire_slots);
extern "C" void syNetPeerStartOnlineSpectate(int player_count, unsigned int delay,
                                             unsigned int stage, unsigned int stocks, int items,
                                             unsigned int seed, unsigned int session,
                                             const int *fkinds, const int *costumes);
extern "C" void syNetPeerAddSpecLink(int link);
extern "C" void syNetPeerRemoveSpecLink(int link);
extern "C" void syNetPeerBeginSpecCatchUp(int link);
extern "C" unsigned int syNetInputGetTick(void);

namespace {

constexpr const char *kStunServer = "stun.cloudflare.com:3478";
constexpr const char *kCodeAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";   // no 0/O/1/I
constexpr unsigned int kStunGiveUpMs = 2000;
constexpr unsigned int kKeepaliveMs = 20000;
constexpr unsigned int kHelloMs = 100;
constexpr unsigned int kPunchTimeoutMs = 3000;
constexpr unsigned int kPeerJoinTimeoutMs = 15000;
constexpr unsigned int kGoTimeoutMs = 4000;
constexpr unsigned int kPickMs = 250;
constexpr unsigned int kIdlePingMs = 2000;
constexpr unsigned int kStartRetryMs = 250;
constexpr unsigned int kPeerTimeoutMs = 10000;

#ifdef SSB64_TEST_HOOKS
/* Test rigs may need a longer leash: several same-machine peers can stall
 * a process past the default 10s (rollback resims blocking the netloop on
 * starved CPU) without the peer actually being dead. SSB64_NETPLAY_DROP_MS
 * overrides this and the netpeer pacing-drop timeout together. */
unsigned int peer_timeout_ms() {
    if (const char *env = std::getenv("SSB64_NETPLAY_DROP_MS")) {
        int v = std::atoi(env);
        if (v > 0) {
            return static_cast<unsigned int>(v);
        }
    }
    return kPeerTimeoutMs;
}
#else
constexpr unsigned int peer_timeout_ms() { return kPeerTimeoutMs; }
#endif
constexpr int kMaxRemotePeers = 7;    /* room slots 1..3 are players, 4..7 spectators */
constexpr int kLanDefaultPort = 41700;

/* Spectators occupy room slots 4..7: receive-only relays that watch the
 * match live without joining pacing or publishing inputs. */
bool is_spec_slot(int slot) { return slot >= 4 && slot <= 7; }

#ifdef BATTLESHIP_CURRENT_VERSION
const char *const kGameVersion = BATTLESHIP_CURRENT_VERSION;
#else
const char *const kGameVersion = "dev";
#endif

#ifdef BATTLESHIP_BUILD_SHA
const char *const kBuildSha = BATTLESHIP_BUILD_SHA;
#else
const char *const kBuildSha = "dev";
#endif

static void fnv1a_accum(uint64_t &h, const void *data, size_t n)
{
    const unsigned char *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
}

static void fnv1a_accum_str(uint64_t &h, const std::string &s)
{
    fnv1a_accum(h, s.data(), s.size());
}

/* FNV-1a over a whole file (streamed; 0 when unreadable). The buffer must
 * stay small: this runs on the game thread inside port_netlobby_update,
 * and Windows' default thread stack is only 1 MiB — a 1 MiB buffer here
 * overflows it (0xC00000FD in __chkstk). */
static uint64_t fnv1a_file(const std::filesystem::path &path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return 0;
    }
    uint64_t h = 1469598103934665603ULL;
    char buf[64 << 10];
    while (f) {
        f.read(buf, sizeof(buf));
        const std::streamsize n = f.gcount();
        if (n > 0) {
            fnv1a_accum(h, buf, static_cast<size_t>(n));
        }
    }
    return h;
}

/* Content fingerprint of one mounted mod payload. Archive files (.o2r/.otr/
 * .zip) hash their full bytes; folder mods hash each contained file's
 * relative path + byte size (cheap; catches content drift under an unchanged
 * name@version without hashing arbitrarily large trees). */
static uint64_t mod_content_hash(const std::filesystem::path &path)
{
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        return fnv1a_file(path);
    }
    if (!std::filesystem::is_directory(path, ec)) {
        return 0;
    }
    uint64_t h = 1469598103934665603ULL;
    std::filesystem::recursive_directory_iterator it(path, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string rel =
            it->path().lexically_relative(path).generic_string();
        fnv1a_accum_str(h, rel);
        if (it->is_regular_file(ec)) {
            const uint64_t sz = it->file_size(ec);
            fnv1a_accum(h, &sz, sizeof(sz));
        }
    }
    return h;
}

/* FNV-1a over the base asset archive (BattleShip.o2r / BattleShip-JP.o2r).
 * Two peers with the same build but differently-extracted or hand-edited
 * archives would desync silently; the archive bytes are the ground truth.
 * Computed once and cached — the file can't change under a running process. */
static uint64_t asset_archive_hash(void)
{
    static uint64_t cached = 0;
    static bool computed = false;
    if (!computed) {
        computed = true;
        const std::filesystem::path p =
            Ship::Context::GetPathRelativeToAppDirectory(SSB64_O2R_NAME);
        cached = fnv1a_file(p);
        if (cached == 0) {
            port_log("SSB64 Lobby: WARNING could not hash %s — asset "
                     "compatibility check will be identity-only\n",
                     p.string().c_str());
        }
    }
    return cached;
}

/* Compatibility token exchanged in PICK: "<build_sha>:<mod_hash>:<asset_hash>".
 * A same-version build with different code (dev builds, local patches), a
 * different loaded-mod set, or a differently-extracted/modded asset archive
 * would desync silently; the token makes every participant verify it before
 * the match is allowed to start. Clients only ever exchange PICKs with the
 * host, and the host verifies every client against itself - transitive
 * equality covers client<->client pairs. */
std::string compat_token(void)
{
#ifdef SSB64_TEST_HOOKS
    /* test hook — identity forgery must not exist in shipping builds */
    if (const char *force = std::getenv("SSB64_COMPAT_TOKEN")) {
        return force;
    }
#endif
    uint64_t mods = 1469598103934665603ULL;    /* FNV-1a 64 */
#ifndef DISABLE_SCRIPTING
    for (const ssb64::mods::ModInfo &m : ssb64::mods::ModRegistry::Snapshot()) {
        /* Mods that declare "simulation": false in their manifest are
         * cosmetic-only by contract (textures/audio) and exempt from the
         * compatibility hash — they can't desync. The exemption trusts the
         * mod's own declaration: a liar that still shadows gameplay data
         * will desync, same as on console-era mods. */
        if (!m.simulation) {
            continue;
        }
        /* Every mounted mod counts, not just script-Loaded ones: a mounted
         * archive's files shadow game assets (movesets, physics, stage
         * data) even when no ModInit ran, so an asset-only mod would
         * otherwise desync silently. Key by state so the same archive
         * Loaded vs NotLoaded differs; when the manifest is invalid fall
         * back to the archive basename (the name is untrustworthy). The
         * trailing hash covers the payload bytes, so two archives sharing
         * name@version but differing in content are still rejected. */
        std::string entry = std::to_string(static_cast<int>(m.state)) + ":";
        entry += !m.name.empty()
            ? m.name + "@" + m.version
            : std::filesystem::path(m.archivePath).filename().string();
        char hashText[20];
        std::snprintf(hashText, sizeof(hashText), ":%016llx;",
                      static_cast<unsigned long long>(
                          mod_content_hash(m.archivePath)));
        entry += hashText;
        fnv1a_accum_str(mods, entry);
    }
#endif
    char text[96];
    std::snprintf(text, sizeof(text), "%s:%016llx:%016llx", kBuildSha,
                  static_cast<unsigned long long>(mods),
                  static_cast<unsigned long long>(asset_archive_hash()));
    return text;
}

struct Peer {
    int slot = -1;                        /* remote player index 0..3 */
    std::vector<std::string> candidates;  /* endpoints we can punch */
    bool connected = false;               /* transport chosen (link set) */
    bool go = false;                      /* acknowledged the start packet */
    bool got_pick = false;                /* PICK received */
    bool got_pong = false;                /* PONG for our PICK received */
    int fkind = -1;
    int tap_jump_off = 0;
    int ping_ms = -1;
    std::string version;
    unsigned int joined_ms = 0;           /* when punching began */
    unsigned int last_send_ms = 0;        /* HELLO/PICK pacing */
    unsigned int last_heard_ms = 0;
    int relay_requested = 0;              /* client: asked the room to relay */
};

struct AutoLobby {
    bool checked = false;
    bool host = false;
    bool join = false;
    bool spectate = false;
    bool autostart = false;
    bool code_written = false;
    unsigned int start_after_ms = 0;
    std::string server;
    std::string code_file;
};

struct Lobby {
    PortNetLobbyState state = PORT_NETLOBBY_IDLE;
    bool is_host = false;
    bool lan = false;                     /* direct-IP mode: no lobby server, no STUN, no relay */
    bool spectate = false;                /* joined as a read-only watcher (room slot >= 4) */
    std::string server;
    std::string code;
    std::string status = "Pick Host or Join to play online.";

    int my_slot = 0;
    int my_fkind = 0;
    int player_count = 1;                 /* roster size including us */
    int server_version = 1;               /* relay framing + message dialect */
    Peer peers[kMaxRemotePeers];

    int stage = 4, stocks = 3, items = 0, delay = 2;
    unsigned int seed = 0;
    unsigned int session = 0;             /* match session id, decoupled from the public seed */
    std::string up_key;                   /* uplink auth secret issued in "welcome" */
    unsigned int start_sent_ms = 0;
    unsigned int last_start_ms = 0;

    /* The roster is frozen the moment STARTM goes out: peers that already
     * answered GO applied it and are in the match, so a late joiner or a
     * dropped straggler must not change what everyone else runs. */
    int start_count = 0;
    int start_wire[4] = {};
    int start_fkinds[4] = {};
    int start_costumes[4] = {};
    int start_tjs[4] = {};

    /* signaling */
    uint8_t stun_txid[12] = {};
    bool stun_done = false;
    std::string stun_public;
    unsigned int stun_ms = 0;
    bool ws_welcomed = false;
    bool endpoint_sent = false;
    unsigned int last_keepalive_ms = 0;

    bool after_match = false;
    std::string last_start_payload;       /* last STARTM sent; specs arriving pre-start get a copy */

    std::mt19937 rng{std::random_device{}()};
};

Lobby sLobby;
AutoLobby sAuto;

void set_state(PortNetLobbyState state, const std::string &status)
{
    if (sLobby.state != state || sLobby.status != status) {
        sLobby.state = state;
        sLobby.status = status;
        port_log("SSB64 Lobby: %s\n", status.c_str());
    }
}

int peer_index_for_slot(int slot)
{
    for (int i = 0; i < kMaxRemotePeers; i++) {
        if (sLobby.peers[i].slot == slot) {
            return i;
        }
    }
    return -1;
}

Peer *peer_for_slot(int slot)
{
    int i = peer_index_for_slot(slot);
    return (i >= 0) ? &sLobby.peers[i] : nullptr;
}

Peer *peer_new(int slot)
{
    Peer *p = peer_for_slot(slot);
    if (p != nullptr) {
        return p;
    }
    for (Peer &cand : sLobby.peers) {
        if (cand.slot < 0) {
            cand = Peer{};
            cand.slot = slot;
            return &cand;
        }
    }
    return nullptr;
}

void peer_drop(int slot)
{
    if (peer_index_for_slot(slot) >= 0) {
        sLobby.peers[peer_index_for_slot(slot)] = Peer{};
        port_udp_link_clear(slot);
        if (is_spec_slot(slot)) {
            syNetPeerRemoveSpecLink(slot);
        }
    }
}

int count_connected_peers(void)      /* players only; spectators are never counted */
{
    int n = 0;
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.slot <= 3 && p.connected) {
            n++;
        }
    }
    return n;
}

int count_connected_specs(void)
{
    int n = 0;
    for (const Peer &p : sLobby.peers) {
        if (is_spec_slot(p.slot) && p.connected) {
            n++;
        }
    }
    return n;
}

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
    sLobby.ws_welcomed = false;
    sLobby.endpoint_sent = false;
    sLobby.stun_done = false;
    sLobby.stun_public.clear();
    sLobby.player_count = 1;
    sLobby.my_slot = sLobby.is_host ? 0 : 1;
    sLobby.session = 0;
    sLobby.up_key.clear();
    sLobby.start_sent_ms = 0;
    sLobby.last_start_ms = 0;
}

void fail(const std::string &why)
{
    shutdown_net();
    set_state(PORT_NETLOBBY_FAILED, why);
}

/* ---- relay over the lobby WebSocket ---- */

bool diag_relay(void)
{
    static const char *v = std::getenv("SSB64_NETPLAY_DEBUG_STAGES");
    return v != nullptr && *v != '\0';
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

/* ---- STUN (RFC 5389 binding request to stun.cloudflare.com) ---- */

constexpr uint32_t kStunCookie = 0x2112A442;

void send_stun(void)
{
    uint8_t req[20] = { 0x00, 0x01, 0x00, 0x00, 0x21, 0x12, 0xA4, 0x42 };
    std::memcpy(req + 8, sLobby.stun_txid, 12);
    sLobby.stun_ms = port_udp_now_ms();
    port_udp_send_to(kStunServer, req, sizeof(req));
}

bool parse_stun(const uint8_t *msg, int size)
{
    if (size < 20 || msg[0] != 0x01 || msg[1] != 0x01 ||
        msg[4] != 0x21 || msg[5] != 0x12 || msg[6] != 0xA4 || msg[7] != 0x42 ||
        std::memcmp(msg + 8, sLobby.stun_txid, 12) != 0) {
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
            return true;
        }
        at += 4 + ((alen + 3) & ~3);
    }
    return false;
}

/* ---- server (WebSocket) ---- */

std::string local_endpoint(void)
{
    char text[64];
    return port_udp_local_endpoint(text, sizeof(text)) ? std::string(text) : std::string("-");
}

std::string json_field(const std::string &json, const char *key)
{
    std::string needle = std::string("\"") + key + "\":";
    size_t at = json.find(needle);
    if (at == std::string::npos) {
        return std::string();
    }
    at += needle.size();
    while (at < json.size() && json[at] == ' ') at++;
    if (at < json.size() && json[at] == '"') {
        size_t end = json.find('"', at + 1);
        return (end == std::string::npos) ? std::string() : json.substr(at + 1, end - at - 1);
    }
    size_t end = json.find_first_of(",}", at);
    return json.substr(at, end - at);
}

const char *env(const char *name)
{
    const char *v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? v : nullptr;
}

std::string new_code(void)
{
    std::string code;
    for (int i = 0; i < 6; i++) {
        code += kCodeAlphabet[sLobby.rng() % 32];
    }
    return code;
}

int local_tap_jump_off(void)
{
    return CVarGetInteger("gEnhancements.TapJumpDisabled.P1", 0) != 0;
}

void host_rules(int *autoz, int *comp, int *neutral, int *hazards_off)
{
    *autoz = CVarGetInteger("gEnhancements.CasualRules.AutoZCancel", 0) != 0;
    *comp = CVarGetInteger("gEnhancements.CompRuleset", 0) != 0;
    *neutral = CVarGetInteger("gEnhancements.NeutralSpawns", 0) != 0;
    *hazards_off = CVarGetInteger("gEnhancements.StageHazardsDisabled", 0) != 0;
}

void connect_server(void)
{
    std::string url = sLobby.server;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/ws?room=" + sLobby.code + "&role=" +
           (sLobby.spectate ? "spec" : (sLobby.is_host ? "host" : "join")) + "&v=2";
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
    std::string url = sLobby.server;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/ws?room=" + sLobby.code + "&role=join&v=2&up=1&slot=" + std::to_string(sLobby.my_slot) +
           "&key=" + sLobby.up_key;
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
            sLobby.stun_done = true;    // proceed with just the LAN endpoint
            sLobby.stun_public = "-";
            port_log("SSB64 Lobby: STUN gave up, LAN endpoint only\n");
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

std::vector<std::string> split(const char *text)
{
    std::vector<std::string> parts;
    const char *at = text;
    while (*at != '\0') {
        while (*at == ' ') at++;
        const char *start = at;
        while (*at != '\0' && *at != ' ') at++;
        if (at > start) {
            parts.emplace_back(start, at - start);
        }
    }
    return parts;
}

/* ---- match metadata ---- */

/* Players sharing a fighter get distinct costumes in slot order. */
int costume_for(int slot, const int *fkinds, int count)
{
    int costume = 0;
    for (int i = 0; i < slot && i < count; i++) {
        if (fkinds[i] == fkinds[slot]) {
            costume++;
        }
    }
    return costume;
}

/* The match's roster in game-slot order (host side): game slot g is the
 * g-th active room slot, sorted. Room slots can be sparse when a middle
 * player leaves, so the wire_slots[] map tells the engine which transport
 * link each remote lives on. Returns the participant count. */
int gather_roster(int *wire_slots, int *fkinds, int *tjs)
{
    int wires[4];
    int count = 0;

    wires[count++] = sLobby.my_slot;
    for (const Peer &p : sLobby.peers) {
        if (p.slot > 0 && p.slot <= 3 && p.connected) {   /* spectators never join the roster */
            wires[count++] = p.slot;
        }
    }
    std::sort(wires, wires + count);
    for (int g = 0; g < count; g++) {
        int w = wires[g];
        Peer *p = peer_for_slot(w);

        wire_slots[g] = w;
        fkinds[g] = (w == sLobby.my_slot) ? sLobby.my_fkind : (p != nullptr ? p->fkind : 0);
        tjs[g] = (w == sLobby.my_slot) ? local_tap_jump_off() : (p != nullptr ? p->tap_jump_off : 0);
    }
    return count;
}

void apply_match_start(int player_count, int my_game_slot, unsigned int stage, unsigned int stocks,
                       int items, unsigned int seed, unsigned int delay, int autoz, int comp,
                       int neutral, int hazards, const int *fkinds, const int *costumes,
                       const int *tap_jumps, const int *wire_slots, unsigned int session)
{
    port_netrules_set(autoz, comp, neutral, hazards, tap_jumps);
    syNetPeerStartOnlineMatch(my_game_slot, player_count, delay, stage, stocks, items, seed,
                              session, fkinds, costumes, wire_slots);
}

void begin_match(void)
{
    sLobby.after_match = false;
    set_state(PORT_NETLOBBY_IN_MATCH, "Match running (rollback).");
}

void update_status(void)
{
    if (sLobby.state != PORT_NETLOBBY_CONNECTED) {
        return;
    }
    char text[160];
    int n = count_connected_peers() + 1;
    if (sLobby.spectate) {
        int specs_players = sLobby.player_count;
        std::snprintf(text, sizeof(text), "Spectating room %s (%d player%s) - waiting for the host to start.",
                      sLobby.code.c_str(), specs_players, specs_players == 1 ? "" : "s");
    } else if (sLobby.is_host) {
        int watching = count_connected_specs();
        if (watching > 0) {
            std::snprintf(text, sizeof(text), "%d player%s + %d watching - press Start match (up to 4).",
                          n, n == 1 ? "" : "s", watching);
        } else {
            std::snprintf(text, sizeof(text), "%d player%s connected - press Start match (up to 4).",
                          n, n == 1 ? "" : "s");
        }
    } else {
        /* clients only hold a direct link to the host - count_connected_peers
         * would read 1+1 in a 4-player room; the roster count is the truth */
        if (sLobby.player_count > n) {
            n = sLobby.player_count;
        }
        std::snprintf(text, sizeof(text), "Connected (%d player%s) - waiting for host to start.",
                      n, n == 1 ? "" : "s");
    }
    set_state(PORT_NETLOBBY_CONNECTED, text);
}

/* host: multicast the start packet. v2 rooms carry the full roster so every
 * participant gets every slot's fighter/costume/tap-jump. */
std::string build_start_payload(void)
{
    int autoz, comp, neutral, hazards_off;
    host_rules(&autoz, &comp, &neutral, &hazards_off);

    char buf[512];
    int len;
    if (sLobby.server_version >= 2) {
        /* the frozen start_* roster: identical bytes on every retry */
        int n = sLobby.start_count;
        len = std::snprintf(buf, sizeof(buf), "SSBL STARTM %d %d %d %u %d %d %d %d %d %d",
                            sLobby.stage, sLobby.stocks, sLobby.items, sLobby.seed, sLobby.delay,
                            autoz, comp, neutral, hazards_off, n);
        for (int i = 0; i < n && len > 0; i++) {
            len += std::snprintf(buf + len, sizeof(buf) - len, " %d %d %d %d",
                                 sLobby.start_wire[i], sLobby.start_fkinds[i],
                                 sLobby.start_costumes[i], sLobby.start_tjs[i]);
        }
        if (len > 0) {
            len += std::snprintf(buf + len, sizeof(buf) - len, " %u", sLobby.session);
        }
    } else {
        /* v1 dialect: fixed host+client fields understood by old clients */
        int cfk = 0, ctj = 0, cslot = 1;
        for (const Peer &p : sLobby.peers) {
            if (p.slot > 0) {
                cfk = p.fkind;
                ctj = p.tap_jump_off;
                cslot = p.slot;
                break;
            }
        }
        len = std::snprintf(buf, sizeof(buf), "SSBL START %d %d %d %u %d %d %d %d %d %d %d %d %d %d %d %u",
                            sLobby.stage, sLobby.stocks, sLobby.items, sLobby.seed, sLobby.delay,
                            sLobby.my_fkind, costume_for(0, sLobby.start_fkinds, 2), cfk,
                            costume_for(cslot, sLobby.start_fkinds, 2),
                            autoz, comp, neutral, hazards_off,
                            sLobby.start_tjs[0], ctj, sLobby.session);
    }
    return std::string(buf, len > 0 ? static_cast<size_t>(len) : 0);
}

/* ---- incoming game-plane packets ---- */

Peer *peer_for_source(const char *from)
{
    if (std::strncmp(from, "relay:", 6) == 0) {
        return peer_for_slot(std::atoi(from + 6));
    }
    if (std::strcmp(from, "relay") == 0) {
        for (Peer &p : sLobby.peers) {
            if (p.slot >= 0 && port_udp_link_is_relay(p.slot)) {
                return &p;
            }
        }
        /* unframed v1 relay: single peer even before its link is marked */
        if (sLobby.server_version < 2) {
            for (Peer &p : sLobby.peers) {
                if (p.slot >= 0) {
                    return &p;
                }
            }
        }
        return nullptr;
    }
    for (Peer &p : sLobby.peers) {
        if (p.slot < 0) {
            continue;
        }
        for (const std::string &ep : p.candidates) {
            if (ep == from) {
                return &p;
            }
        }
    }
    return nullptr;
}

void on_peer_contact(Peer &p, const char *from)
{
    p.last_heard_ms = port_udp_now_ms();
    if (p.connected) {
        return;
    }
    /* first datagram through wins: direct endpoint or relay path */
    if (std::strncmp(from, "relay", 5) == 0) {
        if (!port_udp_link_set(p.slot, "relay")) {
            return;
        }
        port_log("SSB64 Lobby: player %d reachable via relay\n", p.slot);
    } else {
        if (!port_udp_link_set(p.slot, from)) {
            return;
        }
        port_log("SSB64 Lobby: player %d reachable at %s (direct)\n", p.slot, from);
    }
    p.connected = true;
    if (sLobby.lan && sLobby.is_host) {
        /* LAN: the joiner learns which room slot the host gave it */
        send_to_peer(p, ("SSBL HELLOACK " + std::to_string(p.slot)).c_str());
    } else {
        send_to_peer(p, "SSBL HELLOACK");
    }
    if (sLobby.state == PORT_NETLOBBY_PUNCHING || sLobby.state == PORT_NETLOBBY_WAITING ||
        sLobby.state == PORT_NETLOBBY_JOINING) {
        sLobby.state = PORT_NETLOBBY_CONNECTED;
    }
    update_status();
}

void handle_game_message(Peer &p, const std::vector<std::string> &args)
{
    if (args.size() < 2) {
        return;
    }
    const std::string &verb = args[1];
    p.last_heard_ms = port_udp_now_ms();

    if (verb == "HELLO" || verb == "HELLOACK") {
        return;     // handled by on_peer_contact
    }
    if (verb == "PICK" && args.size() >= 6) {
        p.fkind = std::atoi(args[2].c_str());
        p.tap_jump_off = std::atoi(args[4].c_str());
        p.version = args[5];
        p.got_pick = true;
        if (p.version != kGameVersion) {
            /* On the host an incompatible peer must not kill the room for
             * everyone: drop just that peer and keep waiting. On a client a
             * mismatch with the host means WE should leave. */
            if (sLobby.is_host) {
                port_log("SSB64 Lobby: player %d version mismatch (%s != %s) - dropped\n",
                         p.slot, p.version.c_str(), kGameVersion);
                peer_drop(p.slot);
                sLobby.status = "Dropped a peer on an incompatible version - waiting for players.";
                update_status();
            } else {
                fail(std::string("Version mismatch - you're on ") + kGameVersion +
                     ", a peer is on " + p.version + ".");
            }
            return;
        }
        if (args.size() >= 7) {
            /* build+mod compatibility token: catches same-version builds that
             * differ (dev trees, patched builds, different loaded mods) before
             * they desync mid-match */
            const std::string mine = compat_token();
            if (args[6] != mine) {
                if (sLobby.is_host) {
                    port_log("SSB64 Lobby: player %d build/mod mismatch - dropped\n", p.slot);
                    peer_drop(p.slot);
                    sLobby.status = "Dropped a peer with a different build or mod set.";
                    update_status();
                } else {
                    fail("Build or mod mismatch - all players must run the same build and mods.");
                }
                return;
            }
        }
        std::string pong = "SSBL PONG " + args[3];
        send_to_peer(p, pong.c_str());
        update_status();
        return;
    }
    if (verb == "PONG" && args.size() >= 3) {
        unsigned int sent = static_cast<unsigned int>(std::strtoul(args[2].c_str(), nullptr, 10));
        p.ping_ms = static_cast<int>(port_udp_now_ms() - sent) / 2;
        p.got_pong = true;
        return;
    }
    if (verb == "GO") {
        p.go = true;
        return;
    }
    if (verb == "STARTM" && args.size() >= 12) {
        /* client: full roster start, [<room_slot> <fk> <cos> <tj>] sorted by
         * room slot - our position in the list is our game slot; the last
         * field is the host-generated session id */
        int n = std::atoi(args[11].c_str());
        if (sLobby.is_host || p.slot != 0 || n < 2 || n > 4 ||
            args.size() < 13u + 4u * static_cast<unsigned int>(n)) {
            return;
        }
        unsigned int session = std::strtoul(args[12 + n * 4].c_str(), nullptr, 10);
        int wires[4] = {}, fkinds[4] = {}, costumes[4] = {}, tjs[4] = {};
        int my_game = -1;
        for (int i = 0; i < n; i++) {
            wires[i] = std::atoi(args[12 + i * 4].c_str());
            fkinds[i] = std::atoi(args[13 + i * 4].c_str());
            costumes[i] = std::atoi(args[14 + i * 4].c_str());
            tjs[i] = std::atoi(args[15 + i * 4].c_str());
            if (wires[i] == sLobby.my_slot) {
                my_game = i;
            }
        }
        if (my_game < 0) {
            /* a spectator is never part of the player roster: it still gets
             * STARTM (the host sends it to every spec link) and begins
             * watching the moment the engine's forwarded inputs arrive */
            if (sLobby.spectate) {
                sLobby.player_count = n;
                port_netrules_set(std::atoi(args[7].c_str()), std::atoi(args[8].c_str()),
                                  std::atoi(args[9].c_str()), std::atoi(args[10].c_str()), tjs);
                syNetPeerStartOnlineSpectate(n, std::strtoul(args[6].c_str(), nullptr, 10),
                                             std::strtoul(args[2].c_str(), nullptr, 10),
                                             std::strtoul(args[3].c_str(), nullptr, 10),
                                             std::atoi(args[4].c_str()),
                                             std::strtoul(args[5].c_str(), nullptr, 10),
                                             session, fkinds, costumes);
                begin_match();
            }
            return;     // host started without us
        }
        sLobby.player_count = n;
        for (int i = 0; i < 5; i++) {
            std::string go = "SSBL GO " + std::to_string(std::strtoul(args[5].c_str(), nullptr, 10));
            send_to_peer(p, go.c_str());
        }
        apply_match_start(n, my_game, std::strtoul(args[2].c_str(), nullptr, 10),
                          std::strtoul(args[3].c_str(), nullptr, 10),
                          std::atoi(args[4].c_str()),
                          std::strtoul(args[5].c_str(), nullptr, 10),
                          std::strtoul(args[6].c_str(), nullptr, 10),
                          std::atoi(args[7].c_str()), std::atoi(args[8].c_str()),
                          std::atoi(args[9].c_str()), std::atoi(args[10].c_str()),
                          fkinds, costumes, tjs, wires, session);
        begin_match();
        return;
    }
    if (verb == "START" && args.size() >= 11) {
        /* v1 dialect (host fk/cos, client fk/cos, optional rules) */
        if (sLobby.is_host || sLobby.my_slot != 1) {
            return;
        }
        int stage = std::atoi(args[2].c_str());
        int stocks = std::atoi(args[3].c_str());
        int items = std::atoi(args[4].c_str());
        unsigned int seed = std::strtoul(args[5].c_str(), nullptr, 10);
        unsigned int delay = std::strtoul(args[6].c_str(), nullptr, 10);
        int fkinds[4] = {}, costumes[4] = {}, tjs[4] = {};
        int autoz = 0, comp = 0, neutral = 0, hazards = 0;
        fkinds[0] = std::atoi(args[7].c_str());
        costumes[0] = std::atoi(args[8].c_str());
        fkinds[1] = std::atoi(args[9].c_str());
        costumes[1] = std::atoi(args[10].c_str());
        if (args.size() >= 17) {
            autoz = std::atoi(args[11].c_str());
            comp = std::atoi(args[12].c_str());
            neutral = std::atoi(args[13].c_str());
            hazards = std::atoi(args[14].c_str());
            tjs[0] = std::atoi(args[15].c_str());
            tjs[1] = std::atoi(args[16].c_str());
        }
        sLobby.player_count = 2;
        for (int i = 0; i < 5; i++) {
            send_to_peer(p, ("SSBL GO " + std::to_string(seed)).c_str());
        }
        /* optional trailing session id; old hosts don't send it and the
         * engine falls back to the seed (legacy 1v1 compat) */
        unsigned int session = (args.size() >= 18)
            ? std::strtoul(args[17].c_str(), nullptr, 10) : 0;
        apply_match_start(2, 1, stage, stocks, items, seed, delay, autoz, comp, neutral, hazards,
                          fkinds, costumes, tjs, nullptr, session);
        begin_match();
        return;
    }
}

/* SSB64_LAN_KEY=<secret>: when the host sets it, LAN HELLOs must carry the
 * FNV-1a hash of the same secret (third field) or the endpoint is never
 * adopted. Without it any host on the LAN can squat all three slots with
 * anonymous HELLOs (AUD-26). The hash is only a join gate, not wire
 * encryption - the game protocol stays plaintext either way. */
uint32_t lan_key_hash(void)
{
    const char *k = env("SSB64_LAN_KEY");
    if (k == nullptr || *k == '\0') {
        return 0;
    }
    uint64_t h = 1469598103934665603ULL;
    for (const char *c = k; *c != '\0'; c++) {
        h ^= static_cast<unsigned char>(*c);
        h *= 1099511628211ULL;
    }
    return static_cast<uint32_t>(h ^ (h >> 32));
}

std::string lan_hello(void)
{
    uint32_t h = lan_key_hash();
    if (h == 0) {
        return "SSBL HELLO";
    }
    char buf[40];
    std::snprintf(buf, sizeof(buf), "SSBL HELLO %08X", h);
    return buf;
}

/* LAN host: an SSBL HELLO from an endpoint we have never seen is a new
 * joiner - there is no server roster to announce it, so the host assigns the
 * first free room slot and reports it back in the HELLOACK. */
Peer *lan_accept(const char *from, const std::vector<std::string> &args)
{
    if (!sLobby.lan || !sLobby.is_host) {
        return nullptr;
    }
    unsigned int now = port_udp_now_ms();
    uint32_t want = lan_key_hash();
    if (want != 0) {
        uint32_t got = (args.size() >= 3)
            ? static_cast<uint32_t>(std::strtoul(args[2].c_str(), nullptr, 16))
            : 0;
        if (got != want) {
            static unsigned int sLastRejectLogMs = 0;
            if (now - sLastRejectLogMs > 5000) {
                sLastRejectLogMs = now;
                port_log("SSB64 Lobby: LAN HELLO from %s rejected (key mismatch)\n", from);
            }
            return nullptr;
        }
    } else {
        /* Open LAN: still rate-limit anonymous adoption so a HELLO flood
         * can't occupy all three slots faster than the host can react. */
        static unsigned int sLastAcceptMs = 0;
        if (now - sLastAcceptMs < 500) {
            return nullptr;
        }
        sLastAcceptMs = now;
    }
    int slot = 1;
    while (slot <= 3 && peer_for_slot(slot) != nullptr) {
        slot++;
    }
    if (slot > 3) {
        return nullptr;
    }
    Peer *p = peer_new(slot);
    if (p == nullptr) {
        return nullptr;
    }
    p->candidates.push_back(from);
    p->joined_ms = p->last_heard_ms = now;
    port_log("SSB64 Lobby: LAN join from %s -> slot %d\n", from, slot);
    return p;
}

void pump_transport(void)
{
    uint8_t buf[2048];
    char from[128];

    for (;;) {
        int size = port_udp_recv_from(buf, sizeof(buf) - 1, from, sizeof(from));
        if (size <= 0) {
            return;
        }
        if (parse_stun(buf, size)) {
            continue;
        }
        buf[size] = '\0';
        if (std::strncmp(reinterpret_cast<char *>(buf), "SSBL ", 5) != 0) {
            continue;   // not ours (could be a stray SSNP packet)
        }
        std::vector<std::string> args = split(reinterpret_cast<char *>(buf));
        if (args.size() >= 2 && (args[1] == "HELLO" || args[1] == "HELLOACK")) {
            Peer *p = peer_for_source(from);
            if (p == nullptr) {
                p = lan_accept(from, args);
            }
            if (p == nullptr) {
                continue;
            }
            on_peer_contact(*p, from);
            if (sLobby.lan && !sLobby.is_host && args[1] == "HELLOACK" && args.size() >= 3) {
                sLobby.my_slot = std::atoi(args[2].c_str());   // host-assigned room slot
            }
            continue;
        }
        Peer *p = peer_for_source(from);
        if (p == nullptr) {
            continue;
        }
        handle_game_message(*p, args);
    }
}

/* ---- server messages ---- */

void update_peer_endpoints(int slot, const std::string &json_entry)
{
    /* clients only ever link to the host: other clients' traffic arrives
     * forwarded by the host once the match starts */
    if (!sLobby.is_host && slot != 0) {
        return;
    }
    std::string pub = json_field(json_entry, "public");
    std::string local = json_field(json_entry, "local");
    if (pub.empty() && local.empty()) {
        return;
    }
    Peer *p = peer_new(slot);
    if (p == nullptr || p->connected) {
        return;
    }
    if (p->joined_ms == 0) {
        p->joined_ms = port_udp_now_ms();
        p->last_heard_ms = p->joined_ms;
    }
    if (!pub.empty() && pub != "-" &&
        std::find(p->candidates.begin(), p->candidates.end(), pub) == p->candidates.end()) {
        p->candidates.push_back(pub);
    }
    if (!local.empty() && local != "-" &&
        std::find(p->candidates.begin(), p->candidates.end(), local) == p->candidates.end()) {
        p->candidates.push_back(local);
    }
}

/* The "peers" roster (v2) lists every participant; each entry is a JSON
 * object. Parse loosely: locate each "slot":N and read that object's fields. */
void on_roster(const std::string &text)
{
    bool seen[8] = {};
    if (sLobby.my_slot >= 0 && sLobby.my_slot < 8) {
        seen[sLobby.my_slot] = true;
    }

    size_t at = 0;
    while ((at = text.find("\"slot\":", at)) != std::string::npos) {
        size_t entry_end = text.find('}', at);
        std::string entry = (entry_end == std::string::npos)
            ? text.substr(at) : text.substr(at, entry_end - at);
        int slot = std::atoi(json_field(entry, "slot").c_str());
        if (slot >= 0 && slot < 8) {
            seen[slot] = true;
        }
        if (is_spec_slot(slot)) {
            /* spectators have no endpoints to exchange: the host just opens
             * a relay link and (mid-match) feeds it through the engine */
            if (sLobby.is_host && json_field(entry, "role") == "spec") {
                Peer *p = peer_new(slot);
                if (p != nullptr && !p->connected) {
                    p->connected = port_udp_link_set(slot, "relay") == 1;
                    p->last_heard_ms = p->joined_ms = port_udp_now_ms();
                    if (sLobby.state == PORT_NETLOBBY_IN_MATCH &&
                        !sLobby.last_start_payload.empty() &&
                        std::getenv("SSB64_SPECTATE_CATCHUP") == nullptr &&
                        syNetInputGetTick() < 21000) {   /* inside the 21600-tick recording cap */
                        /* mid-match catch-up: hand the spec the current
                         * match's STARTM so it boots at tick 0, then arm the
                         * engine's history stream - it resimulates the
                         * recorded inputs faster than realtime and lands on
                         * the live edge. */
                        port_log("SSB64 Lobby: spectator slot %d joined mid-match - catch-up stream armed\n", slot);
                        send_to_peer(*p, sLobby.last_start_payload.c_str());
                        /* the engine cleared its spec-link list at match
                         * start - register this late joiner before arming */
                        syNetPeerAddSpecLink(p->slot);
                        syNetPeerBeginSpecCatchUp(p->slot);
                    } else {
                        /* catch-up disabled or past the recording cap:
                         * wait for the NEXT match's STARTM */
                        port_log("SSB64 Lobby: spectator slot %d%s\n", slot,
                                 (sLobby.state == PORT_NETLOBBY_IN_MATCH) ? " joined mid-match - watching next match" : "");
                    }
                }
            }
        } else if (slot >= 0 && slot <= 3) {
            if (slot != sLobby.my_slot) {
                update_peer_endpoints(slot, entry);
            }
        }
        at = (entry_end == std::string::npos) ? entry_end : entry_end + 1;
    }
    int count = 0;
    for (int i = 0; i < 4; i++) {      /* player_count never includes specs */
        if (seen[i]) {
            count++;
        }
    }
    if (!sLobby.spectate) {
        sLobby.player_count = count;   // room slots can be sparse; count who is here
    } else if (count > 0) {
        sLobby.player_count = count;
    }

    /* drop peers that vanished from the roster (left before the match) */
    if (sLobby.state != PORT_NETLOBBY_IN_MATCH) {
        for (Peer &p : sLobby.peers) {
            if (p.slot >= 0 && !seen[p.slot]) {
                peer_drop(p.slot);
            }
        }
    }
    update_status();
}

void on_server_text(const char *text)
{
    std::string type = json_field(text, "type");

    if (type == "welcome") {
        sLobby.ws_welcomed = true;
        sLobby.code = json_field(text, "room");
        sLobby.up_key = json_field(text, "upkey");
        std::string slot = json_field(text, "slot");
        if (!slot.empty()) {
            sLobby.server_version = 2;
            sLobby.my_slot = std::atoi(slot.c_str());
            sLobby.up_key = json_field(text, "upkey");
            port_udp_set_relay_framed(1);
            port_udp_set_relay_hooks(relay_send, relay_recv);
            if (sLobby.spectate) {
                /* spectators are relay-only: a single framed link to the
                 * host (room slot 0), no uplink (a spec never sends game
                 * traffic - its PICKs ride the control socket framing) */
                Peer *p = peer_new(0);
                if (p != nullptr) {
                    p->connected = port_udp_link_set(0, "relay") == 1;
                    p->last_heard_ms = p->joined_ms = port_udp_now_ms();
                }
                sLobby.stun_done = true;
                sLobby.endpoint_sent = true;
            } else {
                open_uplink();      // send-only channel for relayed game traffic
                on_roster(text);    // v2 welcome carries the current roster
            }
        } else {
            sLobby.server_version = 1;
            sLobby.my_slot = sLobby.is_host ? 0 : 1;
        }
        if (sLobby.spectate) {
            set_state(PORT_NETLOBBY_CONNECTED,
                      "Spectating room " + sLobby.code + " - waiting for the host to start.");
        } else if (sLobby.is_host && sLobby.state == PORT_NETLOBBY_CREATING) {
            set_state(PORT_NETLOBBY_WAITING, "Room open - share the code, waiting for players.");
        } else if (!sLobby.is_host && sLobby.state == PORT_NETLOBBY_JOINING) {
            set_state(PORT_NETLOBBY_PUNCHING, "Joined room - connecting to host.");
        }
        send_endpoint_if_ready();
        return;
    }
    if (type == "peers") {         // v2 roster broadcast
        on_roster(text);
        return;
    }
    if (type == "peer") {          // v1: single opponent endpoints
        std::string entry = std::string("\"public\":\"") + json_field(text, "public") +
                            "\",\"local\":\"" + json_field(text, "local") + "\"";
        update_peer_endpoints(sLobby.is_host ? 1 : 0, entry);
        sLobby.player_count = 2;
        if (sLobby.state == PORT_NETLOBBY_WAITING || sLobby.state == PORT_NETLOBBY_JOINING) {
            set_state(PORT_NETLOBBY_PUNCHING, "Peer joined - punching through NAT.");
        }
        return;
    }
    if (type == "peer_relay") {    // v2: one peer fell back to the relay
        int slot = std::atoi(json_field(text, "slot").c_str());
        Peer *p = peer_for_slot(slot);
        if (p != nullptr && !p->connected) {
            port_udp_link_set(slot, "relay");
        }
        return;
    }
    if (type == "relaying") {      // v1: whole session goes through the relay
        port_udp_set_relay_hooks(relay_send, relay_recv);
        for (Peer &p : sLobby.peers) {
            if (p.slot >= 0 && !p.connected) {
                port_udp_link_set(p.slot, "relay");
            }
        }
        return;
    }
    if (type == "peer_left") {
        std::string slot = json_field(text, "slot");
        if (!slot.empty()) {       // v2: one participant left
            int left = std::atoi(slot.c_str());
            if (left == 0) {       // host left: the room is over
                if (sLobby.state != PORT_NETLOBBY_IN_MATCH) {
                    fail("Host left the room.");
                }
                return;
            }
            peer_drop(left);
            update_status();
            return;
        }
        /* v1: opponent left */
        for (Peer &p : sLobby.peers) {
            if (p.slot >= 0) {
                peer_drop(p.slot);
            }
        }
        sLobby.player_count = 1;
        if (sLobby.is_host && sLobby.state != PORT_NETLOBBY_IN_MATCH) {
            set_state(PORT_NETLOBBY_WAITING, "Peer left - waiting for players.");
        } else if (!sLobby.is_host && sLobby.state != PORT_NETLOBBY_IN_MATCH) {
            fail("Host left the room.");
        }
        return;
    }
    if (type == "host_left") {
        if (sLobby.state != PORT_NETLOBBY_IN_MATCH) {
            fail("Host left the room.");
        }
        return;
    }
    if (type == "error") {
        std::string msg = json_field(text, "reason");
        if (msg.empty()) {
            msg = json_field(text, "message");
        }
        if (msg == "room-full") {
            fail("Room is full (4 players).");
        } else if (msg == "version-mismatch") {
            fail("Room runs a different netplay build.");
        } else if (msg == "room-in-use") {
            fail("Room code already in use - try hosting again.");
        } else if (msg == "no-such-room") {
            fail("No such room.");
        } else if (msg == "bad-code") {
            fail("Invalid room code.");
        } else {
            fail("Server error: " + msg);
        }
        return;
    }
}

/* ---- per-frame update ---- */

void pump_peers(unsigned int now)
{
    bool any_connecting = false;
    for (Peer &p : sLobby.peers) {
        if (p.slot < 0) {
            continue;
        }
        if (!p.connected) {
            any_connecting = true;
            /* SSB64_LOBBY_FORCE_RELAY skips the direct path entirely
             * (unattended relay testing); otherwise the client (or anyone
             * on a v1 room) asks the room to relay once punching has had
             * its chance */
            const bool force_relay = env("SSB64_LOBBY_FORCE_RELAY") != nullptr;
            if (p.relay_requested == 0 && !sLobby.lan &&
                (!sLobby.is_host || sLobby.server_version < 2) &&
                (force_relay || now - p.joined_ms > kPunchTimeoutMs)) {
                port_ws_send_text("{\"type\":\"relay\"}");
                p.relay_requested = 1;
            }
            if (now - p.last_send_ms >= kHelloMs) {
                p.last_send_ms = now;
                if (port_udp_link_is_set(p.slot)) {
                    send_to_peer(p, lan_hello().c_str());   // relay path
                }
                if (!force_relay) {
                    const std::string hello = lan_hello();
                    for (const std::string &endpoint : p.candidates) {
                        port_udp_send_to(endpoint.c_str(), hello.c_str(), (int)hello.size());
                    }
                }
            }
            if (now - p.joined_ms > kPeerJoinTimeoutMs) {
                port_log("SSB64 Lobby: player %d unreachable (punch + relay failed)\n", p.slot);
                if (!sLobby.is_host) {
                    fail("Could not reach the host.");
                    return;
                }
                peer_drop(p.slot);
                update_status();
            }
            continue;
        }
        /* connected: PICK doubles as the game-plane keepalive - fast until
         * the peer echoes PONG, then a slower cadence so a long lobby wait
         * doesn't read as silence and trip the peer timeout */
        unsigned int interval = (p.got_pong != false) ? kIdlePingMs : kPickMs;
        if (now - p.last_send_ms >= interval) {
            p.last_send_ms = now;
            char msg[160];
            /* compat_token() always returns "<sha>:<hash>:<hash>" —
             * pre-handshake peers just omit the field, we always send ours. */
            const std::string token = compat_token();
            std::snprintf(msg, sizeof(msg), "SSBL PICK %d %u %d %s %s", sLobby.my_fkind,
                          port_udp_now_ms(), local_tap_jump_off(), kGameVersion,
                          token.c_str());
            send_to_peer(p, msg);
        }
        /* spec peers are optional and can sit silent through a whole match
         * (lobby traffic stops while the engine owns the socket); the
         * roster's peer_left is the truth for them. Same for a spectator's
         * host link: the websocket already reports the host's death. */
        if (is_spec_slot(p.slot) || (sLobby.spectate && p.slot == 0)) {
            continue;
        }
        if (sLobby.state != PORT_NETLOBBY_STARTING && p.last_heard_ms > 0 &&
            now - p.last_heard_ms > kPeerTimeoutMs) {
            port_log("SSB64 Lobby: player %d timed out\n", p.slot);
            if (!sLobby.is_host) {
                fail("Lost connection to the host.");
                return;
            }
            peer_drop(p.slot);
            update_status();
        }
    }
    if (any_connecting && sLobby.state == PORT_NETLOBBY_CONNECTED) {
        update_status();    // roster text may have changed
    }
}

int roster_peer_go_count(void)
{
    int n = 0;
    for (Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.connected && p.go) {
            n++;
        }
    }
    return n;
}

/* Peers that belong to the frozen start roster (their room slot is in
 * start_wire); a player that joined after STARTM was sent is not in it. */
bool in_start_roster(const Peer &p)
{
    for (int i = 0; i < sLobby.start_count; i++) {
        if (sLobby.start_wire[i] == p.slot) {
            return true;
        }
    }
    return false;
}

void host_apply_start(void)
{
    int autoz, comp, neutral, hazards_off;
    host_rules(&autoz, &comp, &neutral, &hazards_off);
    sLobby.player_count = sLobby.start_count;
    apply_match_start(sLobby.start_count, 0, sLobby.stage, sLobby.stocks, sLobby.items,
                      sLobby.seed, sLobby.delay, autoz, comp, neutral, hazards_off,
                      sLobby.start_fkinds, sLobby.start_costumes, sLobby.start_tjs,
                      sLobby.start_wire, sLobby.session);
    /* register each connected spectator link with the engine (it cleared
     * the list when the match was configured) and push one STARTM so a
     * spec that joined while GOs were pending still sees the start */
    sLobby.last_start_payload = build_start_payload();
    std::string payload = sLobby.last_start_payload;
    for (Peer &p : sLobby.peers) {
        if (is_spec_slot(p.slot) && p.connected) {
            syNetPeerAddSpecLink(p.slot);
            send_to_peer(p, payload.c_str());
        }
    }
    begin_match();
}

void host_start_sequence(unsigned int now)
{
    if (sLobby.state != PORT_NETLOBBY_STARTING) {
        return;
    }
    bool all_go = true;
    bool any_peer = false;
    for (Peer &p : sLobby.peers) {
        if (p.slot < 0 || !p.connected || !in_start_roster(p)) {
            continue;
        }
        any_peer = true;
        if (!p.go) {
            all_go = false;
        }
    }
    if (!all_go && now - sLobby.last_start_ms >= kStartRetryMs) {
        std::string payload = build_start_payload();
        sLobby.last_start_ms = now;
        for (Peer &p : sLobby.peers) {
            if (p.slot >= 0 && p.connected && !p.go && in_start_roster(p)) {
                send_to_peer(p, payload.c_str());
            }
            /* spectators get the same STARTM so they can start watching;
             * they never answer GO, so they're not gated on it */
            if (is_spec_slot(p.slot) && p.connected) {
                send_to_peer(p, payload.c_str());
            }
        }
    }
    if (all_go && any_peer) {
        host_apply_start();
        return;
    }
    if (now - sLobby.start_sent_ms > kGoTimeoutMs) {
        if (roster_peer_go_count() > 0) {
            /* at least one peer already locked this roster in: start with
             * it anyway; a silent peer's engine-side remote simply drops */
            port_log("SSB64 Lobby: GO timeout - starting with %d of %d roster peers\n",
                     roster_peer_go_count(), sLobby.start_count - 1);
            host_apply_start();
            return;
        }
        /* nobody answered: drop the silent ones and retry a fresh roster */
        for (Peer &p : sLobby.peers) {
            if (p.slot >= 0 && p.connected && !p.go) {
                port_log("SSB64 Lobby: player %d missed GO - dropping\n", p.slot);
                peer_drop(p.slot);
            }
        }
        if (count_connected_peers() == 0) {
            fail("No players responded - match aborted.");
            return;
        }
        int n = gather_roster(sLobby.start_wire, sLobby.start_fkinds, sLobby.start_tjs);
        sLobby.start_count = n;
        for (int i = 0; i < n; i++) {
            sLobby.start_costumes[i] = costume_for(i, sLobby.start_fkinds, n);
        }
        sLobby.player_count = n;
        for (Peer &p : sLobby.peers) {
            p.go = false;
        }
        sLobby.start_sent_ms = now;
        sLobby.last_start_ms = 0;
    }
}

void update_auto(void)
{
    if (!sAuto.checked) {
        sAuto.checked = true;
        if (const char *server = env("SSB64_LOBBY_HOST")) {
            sAuto.host = true;
            sAuto.server = server;
        } else if (const char *joinAddr = env("SSB64_LOBBY_JOIN")) {
            sAuto.join = true;
            sAuto.server = joinAddr;
            /* SSB64_LOBBY_SPECTATE=1 turns the auto-join into a spectator */
            sAuto.spectate = env("SSB64_LOBBY_SPECTATE") != nullptr;
        }
        sAuto.code_file = env("SSB64_LOBBY_CODE_FILE") ? env("SSB64_LOBBY_CODE_FILE") : "lobby_code.txt";
        sAuto.autostart = env("SSB64_LOBBY_AUTOSTART") != nullptr;
        int fkind = env("SSB64_LOBBY_FKIND") ? std::atoi(env("SSB64_LOBBY_FKIND")) : 0;
        int stage = env("SSB64_LOBBY_STAGE") ? std::atoi(env("SSB64_LOBBY_STAGE")) : 4;

        /* LAN test hooks: SSB64_LAN_HOST=<port> / SSB64_LAN_JOIN=<ip[:port]> */
        if (const char *lan_port = env("SSB64_LAN_HOST")) {
            sAuto.host = true;
            port_netlobby_lan_host(std::atoi(lan_port), fkind, stage, 3, 0, 2);
            sLobby.my_fkind = fkind;
        /* The menu's "Your fighter" combo and the "(you)" roster entry read
         * gNetplay.Fighter — keep it in sync with the env-forced pick. */
        CVarSetInteger("gNetplay.Fighter", fkind);
            return;
        }
        if (const char *lan_ep = env("SSB64_LAN_JOIN")) {
            port_netlobby_lan_join(lan_ep, fkind);
            sLobby.my_fkind = fkind;
        /* The menu's "Your fighter" combo and the "(you)" roster entry read
         * gNetplay.Fighter — keep it in sync with the env-forced pick. */
        CVarSetInteger("gNetplay.Fighter", fkind);
            sAuto.join = false;
            return;
        }

        if (sAuto.host) {
            std::remove(sAuto.code_file.c_str());
            port_netlobby_host(sAuto.server.c_str(), fkind, stage, 3, 0, 2);
        }
        sLobby.my_fkind = fkind;
        /* The menu's "Your fighter" combo and the "(you)" roster entry read
         * gNetplay.Fighter — keep it in sync with the env-forced pick. */
        CVarSetInteger("gNetplay.Fighter", fkind);
    }
    if (sAuto.host && !sAuto.code_written && sLobby.state == PORT_NETLOBBY_WAITING) {
        if (FILE *f = std::fopen(sAuto.code_file.c_str(), "w")) {
            std::fputs(sLobby.code.c_str(), f);
            std::fclose(f);
            sAuto.code_written = true;
        }
    }
    if (sAuto.join && sLobby.state == PORT_NETLOBBY_IDLE) {
        if (FILE *f = std::fopen(sAuto.code_file.c_str(), "r")) {
            char code[16] = {};
            if (std::fgets(code, sizeof(code), f) != nullptr && std::strlen(code) >= 6) {
                code[6] = '\0';
                if (sAuto.spectate) {
                    port_netlobby_spectate(sAuto.server.c_str(), code);
                } else {
                    port_netlobby_join(sAuto.server.c_str(), code, sLobby.my_fkind);
                }
                sAuto.join = false;
            }
            std::fclose(f);
        }
    }
    if (sAuto.host && sAuto.autostart && sLobby.state == PORT_NETLOBBY_CONNECTED) {
        /* SSB64_LOBBY_MIN_PLAYERS: wait for a full room before autostarting
         * (multi-player tests), default: start as soon as anyone is ready */
        int min_players = env("SSB64_LOBBY_MIN_PLAYERS") ? std::atoi(env("SSB64_LOBBY_MIN_PLAYERS")) : 2;
        int players = 1;
        for (const Peer &p : sLobby.peers) {
            if (p.slot >= 0 && p.slot <= 3 && p.connected) {
                players++;
            }
        }
        bool ready = players >= min_players;
        for (const Peer &p : sLobby.peers) {
            /* spectators don't gate the start - they only need the STARTM */
            if (p.slot >= 0 && p.slot <= 3 && p.connected && !p.got_pick) {
                ready = false;
            }
        }
        if (ready) {
            if (sAuto.start_after_ms == 0) {
                sAuto.start_after_ms = port_udp_now_ms() + 1000;
            } else if (static_cast<int>(port_udp_now_ms() - sAuto.start_after_ms) >= 0) {
                port_netlobby_start_match();
                sAuto.autostart = false;
            }
        }
    }
}

} // namespace

extern "C" void port_netlobby_update(void)
{
    update_auto();

    if (sLobby.state == PORT_NETLOBBY_IDLE || sLobby.state == PORT_NETLOBBY_FAILED) {
        return;
    }
    /* while the engine runs the match it owns the socket; only watch the WS
     * for room-level events (host leaving mid-match etc.) */
    if (port_ws_state() == PORT_WS_FAILED && sLobby.state != PORT_NETLOBBY_IN_MATCH) {
        fail("Lost connection to the lobby server.");
        return;
    }
    char text[2048];
    while (port_ws_poll_text(text, sizeof(text)) > 0) {
        on_server_text(text);
    }
    unsigned int now = port_udp_now_ms();
    if (sLobby.endpoint_sent && now - sLobby.last_keepalive_ms > kKeepaliveMs) {
        sLobby.last_keepalive_ms = now;
        port_ws_send_text("{\"type\":\"ping\"}");
    }
    if (sLobby.state == PORT_NETLOBBY_IN_MATCH) {
        return;
    }
    pump_transport();
    send_endpoint_if_ready();
    /* pump_transport() may have started the match (begin_match) or refreshed
     * peer last_heard_ms to a time after `now` was captured - recheck the
     * state and re-read the clock before the timeout sweep, or a 1ms skew
     * reads as a 4-billion-ms silence and instantly "times out" the peer */
    if (sLobby.state == PORT_NETLOBBY_IN_MATCH) {
        return;
    }
    now = port_udp_now_ms();
    pump_peers(now);
    host_start_sequence(now);
}

extern "C" void port_netlobby_host(const char *server, int fkind, int stage, int stocks, int items, int delay)
{
    port_netlobby_cancel();
    sLobby.is_host = true;
    sLobby.my_slot = 0;
    sLobby.my_fkind = fkind;
    sLobby.stage = stage;
    sLobby.stocks = stocks;
    sLobby.items = items;
    sLobby.delay = delay;
    sLobby.seed = sLobby.rng();
    sLobby.player_count = 1;
    sLobby.server_version = 1;
    sLobby.server = (server != nullptr && *server != '\0') ? server : PORT_NETLOBBY_DEFAULT_SERVER;
    for (int i = 0; i < 12; i++) {
        sLobby.stun_txid[i] = static_cast<uint8_t>(sLobby.rng() & 0xFF);
    }
    if (!port_udp_open("0.0.0.0:0")) {
        fail("Could not open a UDP socket.");
        return;
    }
    sLobby.code = new_code();
    sLobby.last_keepalive_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: hosting, STUN + room create...\n");
    set_state(PORT_NETLOBBY_CREATING, "Creating room...");
    send_stun();
    connect_server();
}

extern "C" void port_netlobby_join(const char *server, const char *code, int fkind)
{
    port_netlobby_cancel();
    sLobby.is_host = false;
    sLobby.my_slot = 1;   // a v2 server assigns the real slot in "welcome"
    sLobby.my_fkind = fkind;
    sLobby.player_count = 2;
    sLobby.server_version = 1;
    sLobby.server = (server != nullptr && *server != '\0') ? server : PORT_NETLOBBY_DEFAULT_SERVER;
    sLobby.code = code ? code : "";
    for (char &c : sLobby.code) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    for (int i = 0; i < 12; i++) {
        sLobby.stun_txid[i] = static_cast<uint8_t>(sLobby.rng() & 0xFF);
    }
    if (!port_udp_open("0.0.0.0:0")) {
        fail("Could not open a UDP socket.");
        return;
    }
    sLobby.last_keepalive_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: joining room %s\n", sLobby.code.c_str());
    set_state(PORT_NETLOBBY_JOINING, "Joining room " + sLobby.code + "...");
    send_stun();
    connect_server();
}

/* Spectate a room: the Worker assigns a slot in 4..7; the host's engine
 * forwards every player's input packets (FORWARD-wrapped) over the
 * spectator's relay link and the STARTM tells the engine which match to
 * render. The spectator never punches, never sends game packets and never
 * joins pacing - it watches a few ticks behind the players. */
extern "C" void port_netlobby_spectate(const char *server, const char *code)
{
    port_netlobby_cancel();
    sLobby.is_host = false;
    sLobby.spectate = true;
    sLobby.my_slot = 4;                 // the server assigns the real spec slot
    sLobby.my_fkind = 0;
    sLobby.player_count = 0;
    sLobby.server_version = 1;
    sLobby.server = (server != nullptr && *server != '\0') ? server : PORT_NETLOBBY_DEFAULT_SERVER;
    sLobby.code = code ? code : "";
    for (char &c : sLobby.code) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (!port_udp_open("0.0.0.0:0")) {
        fail("Could not open a UDP socket.");
        return;
    }
    sLobby.last_keepalive_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: spectating room %s\n", sLobby.code.c_str());
    set_state(PORT_NETLOBBY_JOINING, "Joining room " + sLobby.code + " as spectator...");
    connect_server();
}

/* ---- LAN (direct IP, no lobby server) ----
 * The host binds a fixed UDP port and adopts every new HELLO sender as the
 * next room slot (no room code, no STUN, no relay - the same SSBL/STARTM
 * flow runs over the direct link). */

extern "C" void port_netlobby_lan_host(int port, int fkind, int stage, int stocks, int items, int delay)
{
    port_netlobby_cancel();
    sLobby.is_host = true;
    sLobby.lan = true;
    sLobby.my_slot = 0;
    sLobby.my_fkind = fkind;
    sLobby.stage = stage;
    sLobby.stocks = stocks;
    sLobby.items = items;
    sLobby.delay = delay;
    sLobby.seed = sLobby.rng();
    sLobby.player_count = 1;
    sLobby.server_version = 2;      // STARTM dialect; no server to version-check
    char bind[32];
    std::snprintf(bind, sizeof(bind), "0.0.0.0:%d", (port > 0) ? port : kLanDefaultPort);
    if (!port_udp_open(bind)) {
        fail("Could not open the UDP port.");
        return;
    }
    port_log("SSB64 Lobby: LAN host on %s\n", local_endpoint().c_str());
    set_state(PORT_NETLOBBY_WAITING, "LAN host listening on " + local_endpoint() + " - share this address.");
}

extern "C" void port_netlobby_lan_join(const char *endpoint, int fkind)
{
    port_netlobby_cancel();
    sLobby.is_host = false;
    sLobby.lan = true;
    sLobby.my_slot = 1;             // the host's HELLOACK assigns the real slot
    sLobby.my_fkind = fkind;
    sLobby.player_count = 2;
    sLobby.server_version = 2;
    std::string ep = (endpoint != nullptr) ? endpoint : "";
    if (ep.find(':') == std::string::npos) {
        ep += ":" + std::to_string(kLanDefaultPort);
    }
    if (!port_udp_open("0.0.0.0:0")) {
        fail("Could not open a UDP socket.");
        return;
    }
    Peer *p = peer_new(0);
    if (p == nullptr) {
        fail("Internal error.");
        return;
    }
    p->candidates.push_back(ep);
    p->joined_ms = p->last_heard_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: LAN join %s\n", ep.c_str());
    set_state(PORT_NETLOBBY_PUNCHING, "Connecting to " + ep + "...");
    send_to_peer(*p, lan_hello().c_str());
}

extern "C" void port_netlobby_cancel(void)
{
    shutdown_net();
    sLobby.state = PORT_NETLOBBY_IDLE;
    sLobby.status = "Pick Host or Join to play online.";
}

extern "C" void port_netlobby_start_match(void)
{
    if (!sLobby.is_host || sLobby.state != PORT_NETLOBBY_CONNECTED) {
        return;
    }
    /* every connected player must have sent its pick; unconnected stragglers
     * are left behind (they time out on their own). Spectators don't gate
     * the start - they only need the STARTM. */
    int n = 1;
    for (Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.slot <= 3 && p.connected) {
            if (!p.got_pick || p.fkind < 0) {
                return;
            }
            n++;
        }
    }
    for (Peer &p : sLobby.peers) {
        if (p.slot >= 0 && !p.connected) {
            peer_drop(p.slot);
        }
    }
    sLobby.player_count = n;
    sLobby.seed = sLobby.rng();
    /* the match's session id is a fresh random token, not the seed: the
     * seed is broadcast in STARTM and can't double as a replay guard */
    sLobby.session = sLobby.rng();
    /* freeze the roster every participant will run on */
    sLobby.start_count = gather_roster(sLobby.start_wire, sLobby.start_fkinds, sLobby.start_tjs);
    for (int i = 0; i < sLobby.start_count; i++) {
        sLobby.start_costumes[i] = costume_for(i, sLobby.start_fkinds, sLobby.start_count);
    }
    for (Peer &p : sLobby.peers) {
        p.go = false;
    }
    sLobby.start_sent_ms = port_udp_now_ms();
    sLobby.last_start_ms = 0;
    set_state(PORT_NETLOBBY_STARTING, "Starting match...");
    if (sLobby.server_version >= 2) {
        port_ws_send_text("{\"type\":\"start\"}");
    }
}

extern "C" void port_netlobby_set_fkind(int fkind)
{
    sLobby.my_fkind = fkind;
    for (Peer &p : sLobby.peers) {
        p.got_pong = false;   // re-announce the pick
    }
}

extern "C" void port_netlobby_on_match_end(void)
{
    /* the engine calls this when an online match finishes: back to the room
     * so the host can start a rematch on the still-open links */
    if (sLobby.state != PORT_NETLOBBY_IN_MATCH) {
        return;
    }
    for (Peer &p : sLobby.peers) {
        p.go = false;
        p.last_heard_ms = port_udp_now_ms();
    }
    sLobby.after_match = true;
    if (sLobby.is_host) {
        set_state(PORT_NETLOBBY_CONNECTED, "Match ended - press Start match for a rematch.");
    } else {
        set_state(PORT_NETLOBBY_CONNECTED, "Match ended - waiting for host.");
    }
}

extern "C" int port_netlobby_state(void) { return sLobby.state; }
extern "C" const char *port_netlobby_status(void) { return sLobby.status.c_str(); }
extern "C" const char *port_netlobby_code(void) { return sLobby.code.c_str(); }
extern "C" int port_netlobby_is_host(void) { return sLobby.is_host ? 1 : 0; }
extern "C" int port_netlobby_is_spectator(void) { return sLobby.spectate ? 1 : 0; }
extern "C" int port_netlobby_player_count(void) { return sLobby.player_count; }
extern "C" int port_netlobby_my_slot(void) { return sLobby.my_slot; }

extern "C" int port_netlobby_peer_fkind(void)
{
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.connected) {
            return p.fkind;
        }
    }
    return -1;
}

extern "C" int port_netlobby_peer_count(void)
{
    int n = 0;
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.slot <= 3) {     /* player peers only for the UI */
            n++;
        }
    }
    return n;
}

extern "C" int port_netlobby_peer_slot(int index)
{
    int seen = 0;
    for (const Peer &p : sLobby.peers) {
        if (p.slot < 0 || p.slot > 3) {
            continue;
        }
        if (seen++ == index) {
            return p.slot;
        }
    }
    return -1;
}

extern "C" int port_netlobby_peer_fkind_at(int index)
{
    Peer *p = peer_for_slot(port_netlobby_peer_slot(index));
    return (p != nullptr) ? p->fkind : -1;
}

extern "C" int port_netlobby_peer_connected_at(int index)
{
    Peer *p = peer_for_slot(port_netlobby_peer_slot(index));
    return (p != nullptr && p->connected) ? 1 : 0;
}

extern "C" int port_netlobby_peer_ping_at(int index)
{
    Peer *p = peer_for_slot(port_netlobby_peer_slot(index));
    return (p != nullptr) ? p->ping_ms : -1;
}

extern "C" int port_netlobby_peer_stats_at(int index, unsigned *gaps, unsigned *dups, unsigned *ooo,
                                           int *advantage)
{
    /* sys/netpeer.c keeps per-remote counters keyed by game slot. */
    extern void portNetPeerGetRemoteStats(int player, unsigned *g, unsigned *d, unsigned *o, int *a);
    const int slot = port_netlobby_peer_slot(index);
    unsigned g = 0, d = 0, o = 0;
    int adv = 0;

    if (slot < 0) {
        return 0;
    }
    portNetPeerGetRemoteStats(slot, &g, &d, &o, &adv);
    if (g == 0 && d == 0 && o == 0 && adv == 0) {
        return 0; /* no stats yet (or never netpeer'd — local slot etc.) */
    }
    if (gaps != nullptr) *gaps = g;
    if (dups != nullptr) *dups = d;
    if (ooo != nullptr) *ooo = o;
    if (advantage != nullptr) *advantage = adv;
    return 1;
}

extern "C" int port_netlobby_is_relay(void)
{
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.connected && port_udp_link_is_relay(p.slot)) {
            return 1;
        }
    }
    return port_udp_is_relay();
}

extern "C" int port_netlobby_ping_ms(void)
{
    int worst = -1;
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.connected && p.ping_ms >= 0) {
            worst = (std::max)(worst, p.ping_ms);
        }
    }
    return worst;
}

extern "C" int port_netlobby_after_match(void) { return sLobby.after_match ? 1 : 0; }
