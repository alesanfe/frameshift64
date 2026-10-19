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
#include "netlobby_internal.h"
#include "netlobby_util.h"
#include "netfriends.h"
#include "netleaderboard.h"
#include "netstats.h"
#include "udp_transport.h"
#include "websocket_transport.h"
#include "port_log.h"
#include "netrules.h"
#include "PortI18n.h"

#include <libultraship/bridge/consolevariablebridge.h>

#ifndef DISABLE_SCRIPTING
#include "ModRegistry.h"
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

/* Internal state + helpers live in netlobby_{state,transport,protocol}.cpp
 * (see netlobby_internal.h); state-free helpers in netlobby_util.cpp. */
using namespace ssb64::netlobby;

extern "C" void port_netlobby_update(void)
{
    update_auto();

    if (sLobby.state == PORT_NETLOBBY_IDLE || sLobby.state == PORT_NETLOBBY_FAILED) {
        return;
    }
    /* while the engine runs the match it owns the socket; only watch the WS
     * for room-level events (host leaving mid-match etc.) */
    if (port_ws_state() == PORT_WS_FAILED && sLobby.state != PORT_NETLOBBY_IN_MATCH) {
        fail(TR("Lost connection to the lobby server."));
        return;
    }
    char text[2048];
    while (port_ws_poll_text(text, sizeof(text)) > 0) {
        on_server_text(text);
    }
    unsigned int now = port_udp_now_ms();
    if (sLobby.endpoint_sent && now - sLobby.last_keepalive_ms > kKeepaliveMs) {
        sLobby.last_keepalive_ms = now;
        sLobby.ping_sent_ms = now;
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
        fail(TR("Could not open a UDP socket."));
        return;
    }
    sLobby.code = new_code();
    sLobby.last_keepalive_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: hosting, STUN + room create...\n");
    set_state(PORT_NETLOBBY_CREATING, TR("Creating room..."));
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
        fail(TR("Could not open a UDP socket."));
        return;
    }
    sLobby.last_keepalive_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: joining room %s\n", sLobby.code.c_str());
    char join_msg[96];
    std::snprintf(join_msg, sizeof(join_msg), TR("Joining room %s..."), sLobby.code.c_str());
    set_state(PORT_NETLOBBY_JOINING, join_msg);
    send_stun();
    connect_server();
}

/* Quick play: join the matchmaking queue instead of a friend code. Uses the
 * well-known room code QUEUE<2..9> with role=queue — the first arrival hosts,
 * later arrivals join, so two queued players always pair up. On room-full
 * the lobby rolls to the next code (see the "error" handler).
 *
 * Banding (client-side, zero server changes): the 8 queue rooms are
 * split into 2x2 pools by rating band and last-measured lobby RTT —
 * proven players (rating >= 1500, out of provisional) take the upper
 * half, and within each rating pool the higher-RTT band (>150 ms to the
 * lobby) starts two rooms up. A populated queue then pairs same-band
 * opponents first (ping-vs-ping, MMR-vs-MMR); sparse-queue convergence
 * is preserved because the roll-forward on room-full walks the whole
 * pool. First-ever sessions carry no RTT sample and land in the low
 * band — conservative: they widen toward higher-RTT rooms rather than
 * trapping fast players with a guessed-slow peer. */
extern "C" void port_netlobby_quickplay(const char *server, int fkind,
                                        int stage, int stocks, int items, int delay)
{
    port_netlobby_cancel();
    sLobby.quickplay = true;
    const int rating = port_netstats_rating();
    /* 2x2 band: rated pool starts at idx 4, high-RTT band shifts +2
     * inside each pool. lobby_rtt_ms is 0 until a prior session measured
     * the lobby-path ping — unknown latency maps to the low band. */
    const bool rated = (rating >= 1500 && !port_netstats_provisional());
    const bool highRtt = (sLobby.lobby_rtt_ms > 150 && sLobby.lobby_rtt_ms < 10000);
    sLobby.queue_idx = (rated ? 4 : 0) + (highRtt ? 2 : 0);
    sLobby.is_host = false;             // the room assigns our real role in "welcome"
    sLobby.my_slot = 1;
    sLobby.my_fkind = fkind;
    sLobby.stage = stage;
    sLobby.stocks = stocks;
    sLobby.items = items;
    sLobby.delay = delay;
    sLobby.seed = sLobby.rng();
    sLobby.player_count = 2;
    sLobby.server_version = 1;
    sLobby.server = (server != nullptr && *server != '\0') ? server : PORT_NETLOBBY_DEFAULT_SERVER;
    char code[16];
    std::snprintf(code, sizeof(code), "QUEUE%d", 2 + sLobby.queue_idx);
    sLobby.code = code;
    for (int i = 0; i < 12; i++) {
        sLobby.stun_txid[i] = static_cast<uint8_t>(sLobby.rng() & 0xFF);
    }
    if (!port_udp_open("0.0.0.0:0")) {
        fail(TR("Could not open a UDP socket."));
        return;
    }
    sLobby.last_keepalive_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: quick play (queue room %s)\n", sLobby.code.c_str());
    set_state(PORT_NETLOBBY_JOINING, TR("Searching for a match..."));
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
        fail(TR("Could not open a UDP socket."));
        return;
    }
    sLobby.last_keepalive_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: spectating room %s\n", sLobby.code.c_str());
    char join_msg[96];
    std::snprintf(join_msg, sizeof(join_msg), TR("Joining room %s as spectator..."), sLobby.code.c_str());
    set_state(PORT_NETLOBBY_JOINING, join_msg);
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
        fail(TR("Could not open the UDP port."));
        return;
    }
    port_log("SSB64 Lobby: LAN host on %s\n", local_endpoint().c_str());
    char lan_msg[160];
    std::snprintf(lan_msg, sizeof(lan_msg), TR("LAN host listening on %s - share this address."),
                  local_endpoint().c_str());
    set_state(PORT_NETLOBBY_WAITING, lan_msg);
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
        fail(TR("Could not open a UDP socket."));
        return;
    }
    Peer *p = peer_new(0);
    if (p == nullptr) {
        fail(TR("Internal error."));
        return;
    }
    p->candidates.push_back(ep);
    p->joined_ms = p->last_heard_ms = port_udp_now_ms();
    port_log("SSB64 Lobby: LAN join %s\n", ep.c_str());
    char conn_msg[160];
    std::snprintf(conn_msg, sizeof(conn_msg), TR("Connecting to %s..."), ep.c_str());
    set_state(PORT_NETLOBBY_PUNCHING, conn_msg);
    send_to_peer(*p, lan_hello().c_str());
}

extern "C" void port_netlobby_cancel(void)
{
    shutdown_net();
    sLobby.state = PORT_NETLOBBY_IDLE;
    sLobby.status = TR("Pick Host or Join to play online.");
}

/* Every connected player must have sent its pick; unconnected stragglers
 * are left behind (they time out on their own). Spectators don't gate
 * the start - they only need the STARTM. Returns the participant count
 * (host included), or 0 when someone hasn't picked yet. */
static int picked_player_count(void)
{
    int n = 1;
    for (Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.slot <= 3 && p.connected) {
            if (!p.got_pick || p.fkind < 0) {
                return 0;
            }
            n++;
        }
    }
    return n;
}

/* Drops connected-lost stragglers and freezes the roster every
 * participant will run on (order, fighter kinds, team joins, costumes). */
static void freeze_start_roster(void)
{
    for (Peer &p : sLobby.peers) {
        if (p.slot >= 0 && !p.connected) {
            peer_drop(p.slot);
        }
    }
    sLobby.start_count = gather_roster(sLobby.start_wire, sLobby.start_fkinds, sLobby.start_tjs);
    for (int i = 0; i < sLobby.start_count; i++) {
        sLobby.start_costumes[i] = costume_for(i, sLobby.start_fkinds, sLobby.start_count);
    }
}


extern "C" void port_netlobby_start_match(void)
{
    if (!sLobby.is_host || sLobby.state != PORT_NETLOBBY_CONNECTED) {
        return;
    }
    const int n = picked_player_count();
    if (n == 0) {
        return;
    }
    freeze_start_roster();
    sLobby.player_count = n;
    sLobby.seed = sLobby.rng();
    /* the match's session id is a fresh random token, not the seed: the
     * seed is broadcast in STARTM and can't double as a replay guard */
    sLobby.session = sLobby.rng();
    for (Peer &p : sLobby.peers) {
        p.go = false;
    }
    sLobby.start_sent_ms = port_udp_now_ms();
    sLobby.last_start_ms = 0;
    set_state(PORT_NETLOBBY_STARTING, TR("Starting match..."));
    if (sLobby.server_version >= 2) {
        port_ws_send_text("{\"type\":\"start\"}");
    }
}

extern "C" void port_netlobby_set_fkind(int fkind)
{
    sLobby.my_fkind = fkind;
    /* changing the pick revokes our "ready": the room re-announces it so
     * nobody starts on a fighter we no longer want */
    if (sLobby.my_ready) {
        sLobby.my_ready = false;
        send_ready_state();
    }
    for (Peer &p : sLobby.peers) {
        p.got_pong = false;   // re-announce the pick
    }
}

extern "C" void port_netlobby_set_ready(int ready)
{
    const bool want = (ready != 0);
    if (sLobby.spectate || want == sLobby.my_ready) {
        return;
    }
    sLobby.my_ready = want;
    send_ready_state();
}

extern "C" int port_netlobby_is_ready(void)
{
    return sLobby.my_ready ? 1 : 0;
}

extern "C" int port_netlobby_peer_ready_at(int index)
{
    Peer *p = peer_for_slot(port_netlobby_peer_slot(index));
    return (p != nullptr && p->ready) ? 1 : 0;
}

/* 1 when every connected player peer has flagged ready. v1 and LAN rooms
 * have no ready channel - they report 1 so the PICK handshake stays the only
 * gate there. Spectators and unconnected stragglers never block the start. */
extern "C" int port_netlobby_all_ready(void)
{
    if (sLobby.lan || sLobby.server_version != 2) {
        return 1;
    }
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.slot <= 3 && p.connected && !p.ready) {
            return 0;
        }
    }
    return 1;
}

/* Engine → lobby: called from syNetPeerStopVSSession right before
 * port_netlobby_on_match_end, while gSCManagerBattleState is still valid.
 * winner_slot is the game slot with the highest remaining stock count
 * (-1 on tie/draw — a stock tie on timeout can't be told apart here; the
 * results screen knows the real verdict but the stock mirror is the
 * netplay-accessible signal). */
extern "C" void port_netlobby_set_match_result(int winner_slot, unsigned int duration_ticks)
{
    sLobby.match_winner = winner_slot;
    sLobby.match_duration = duration_ticks;
}

/* Folds the finished match into local stats and, with the SubmitStats
 * opt-in, pushes rating + corroborated result to the lobby board.
 * Spectators never had a game slot: their watch must not land in
 * history as a loss. */
/* Verified path: the server only moves ratings when both clients of a
 * 1v1 report the mirrored outcome under distinct claim keys. Needs
 * exactly one named playing opponent. */
static void report_verified_result(void)
{
    std::string opp_name;
    int opponents = 0;
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.slot <= 3 && p.connected) {
            opponents++;
            if (!p.name.empty()) {
                opp_name = p.name;
            }
        }
    }
    if (opponents == 1 && !opp_name.empty()) {
        const int won = sLobby.match_winner < 0 ? -1
            : (sLobby.my_game_slot == sLobby.match_winner ? 1 : 0);
        port_netleaderboard_report_result(sLobby.server.c_str(),
                                          sLobby.code.c_str(),
                                          sLobby.session,
                                          opp_name.c_str(), won,
                                          sLobby.match_duration);
    }
}

/* Folds the finished match into local stats and, with the SubmitStats
 * opt-in, pushes rating + corroborated result to the lobby board.
 * Spectators never had a game slot: their watch must not land in
 * history as a loss. */
static void record_match_result(void)
{
    if (!sLobby.spectate && sLobby.my_game_slot >= 0) {
        int opp = -1, n = 0, sum = 0;
        for (const Peer &p : sLobby.peers) {
            if (p.slot >= 0 && p.slot <= 3 && p.rating > 0) {
                sum += p.rating;
                n++;
            }
        }
        if (n > 0) {
            opp = sum / n;
        }
        port_netstats_set_opponent_rating(opp);
        port_netstats_record_match(sLobby.my_game_slot, sLobby.match_winner,
                                   sLobby.match_duration);
        /* Publish the updated rating + corroborated result to the lobby
         * board. Opt-IN via gNetplay.SubmitStats=1 (sharing a name/rating
         * on a public board needs consent) — fire-and-forget POSTs, the
         * match is unaffected whether they land or not. */
        if (!sLobby.lan && CVarGetInteger("gNetplay.SubmitStats", 0) != 0) {
            port_netleaderboard_submit(sLobby.server.c_str());
            report_verified_result();
        }
    }
}



extern "C" void port_netlobby_on_match_end(void)
{
    /* the engine calls this when an online match finishes: back to the room
     * so the host can start a rematch on the still-open links */
    if (sLobby.state != PORT_NETLOBBY_IN_MATCH) {
        return;
    }
    /* record the result + fold it into the local rating — the mean of the
     * opponents' declared ratings stands in for a per-opponent ladder.
     * Spectators sit IN_MATCH but never had a game slot: their watch must
     * not land in history as a loss. */
    record_match_result();

    for (Peer &p : sLobby.peers) {
        p.go = false;
        p.ready = false;
        p.last_heard_ms = port_udp_now_ms();
    }
    sLobby.after_match = true;
    /* rematch needs a fresh ready-check: clear ours and let peers' clients
     * publish their own clears */
    if (sLobby.my_ready) {
        sLobby.my_ready = false;
        send_ready_state();
    }
    if (sLobby.is_host) {
        set_state(PORT_NETLOBBY_CONNECTED, TR("Match ended - press Start match for a rematch."));
    } else {
        set_state(PORT_NETLOBBY_CONNECTED, TR("Match ended - waiting for host."));
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

extern "C" const char *port_netlobby_peer_name_at(int index)
{
    Peer *p = peer_for_slot(port_netlobby_peer_slot(index));
    return (p != nullptr) ? p->name.c_str() : "";
}

extern "C" int port_netlobby_peer_stats_at(int index, unsigned *gaps, unsigned *dups, unsigned *ooo,
                                           int *advantage, unsigned *recv_highwater)
{
    /* sys/netpeer.c keeps per-remote counters keyed by game slot. */
    extern void portNetPeerGetRemoteStats(int player, unsigned *g, unsigned *d, unsigned *o, int *a,
                                          unsigned *h);
    const int slot = port_netlobby_peer_slot(index);
    unsigned g = 0, d = 0, o = 0, h = 0;
    int adv = 0;

    if (slot < 0) {
        return 0;
    }
    portNetPeerGetRemoteStats(slot, &g, &d, &o, &adv, &h);
    if (g == 0 && d == 0 && o == 0 && adv == 0) {
        return 0; /* no stats yet (or never netpeer'd — local slot etc.) */
    }
    if (gaps != nullptr) *gaps = g;
    if (dups != nullptr) *dups = d;
    if (ooo != nullptr) *ooo = o;
    if (advantage != nullptr) *advantage = adv;
    if (recv_highwater != nullptr) *recv_highwater = h;
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

/* STUN endpoint list for the connection assistant — mirrors stun_server():
 * the CVar override (when set) is the whole list, otherwise kStunServers. */
extern "C" int port_netlobby_stun_server_count(void)
{
    const char *ov = CVarGetString("gNetplay.StunServer", "");
    return (ov != nullptr && *ov != '\0') ? 1 : kStunServerCount;
}

extern "C" const char *port_netlobby_stun_server_at(int index)
{
    const char *ov = CVarGetString("gNetplay.StunServer", "");
    if (ov != nullptr && *ov != '\0') {
        return index == 0 ? ov : "";
    }
    return (index >= 0 && index < kStunServerCount) ? kStunServers[index] : "";
}

extern "C" int port_netlobby_after_match(void) { return sLobby.after_match ? 1 : 0; }
