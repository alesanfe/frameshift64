/* Host start sequence and auto-lobby (SSB64_AUTOLOBBY env) — split from netlobby.cpp; the wire-protocol overview lives there. */
#include "netlobby_internal.h"
#include <cctype>
/* State-free helpers (compat token, env, split, rules reads)
 * live in netlobby_util.cpp; wire JSON parsing in lobby_json.cpp. */
using namespace ssb64::netlobby;

namespace ssb64::netlobby {

static int roster_peer_go_count(void)
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
static bool in_start_roster(const Peer &p)
{
    for (int i = 0; i < sLobby.start_count; i++) {
        if (sLobby.start_wire[i] == p.slot) {
            return true;
        }
    }
    return false;
}

static void host_apply_start(void)
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

/* Whether every connected roster peer answered GO. any_peer reports
 * whether the frozen roster has any remote member at all — a solo host
 * must not start. */
static bool roster_all_go(bool *any_peer)
{
    bool all_go = true;
    *any_peer = false;
    for (Peer &p : sLobby.peers) {
        if (p.slot < 0 || !p.connected || !in_start_roster(p)) {
            continue;
        }
        *any_peer = true;
        if (!p.go) {
            all_go = false;
        }
    }
    return all_go;
}

/* Roster peers that still owe a GO get a fresh STARTM every
 * kStartRetryMs; spectators get the same STARTM so they can start
 * watching — they never answer GO, so they're not gated on it. */
static void resend_start_packet(unsigned int now)
{
    std::string payload = build_start_payload();
    sLobby.last_start_ms = now;
    for (Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.connected && !p.go && in_start_roster(p)) {
            send_to_peer(p, payload.c_str());
        }
        if (is_spec_slot(p.slot) && p.connected) {
            send_to_peer(p, payload.c_str());
        }
    }
}

/* GO timeout with zero answers: rebuild the frozen roster from whoever
 * is still connected and reset the GO window. */
static void regather_start_roster(unsigned int now)
{
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

static void host_go_timeout(unsigned int now)
{
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
        fail(TR("No players responded - match aborted."));
        return;
    }
    regather_start_roster(now);
}

void host_start_sequence(unsigned int now)
{
    if (sLobby.state != PORT_NETLOBBY_STARTING) {
        return;
    }
    bool any_peer = false;
    const bool all_go = roster_all_go(&any_peer);
    if (!all_go && now - sLobby.last_start_ms >= kStartRetryMs) {
        resend_start_packet(now);
    }
    if (all_go && any_peer) {
        host_apply_start();
        return;
    }
    if (now - sLobby.start_sent_ms > kGoTimeoutMs) {
        host_go_timeout(now);
    }
}

/* The menu's "Your fighter" combo and the "(you)" roster entry read
 * gNetplay.Fighter — keep it in sync with the env-forced pick. */
static void auto_sync_fkind(int fkind)
{
    sLobby.my_fkind = fkind;
    CVarSetInteger("gNetplay.Fighter", fkind);
}

/* One-shot env bootstrap (SSB64_LOBBY_* + the SSB64_LAN_* test hooks).
 * Returns true when a LAN path consumed the session — the server-side
 * phases below don't apply to it. */
static bool auto_bootstrap(void)
{
    if (sAuto.checked) {
        return false;
    }
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
        auto_sync_fkind(fkind);
        return true;
    }
    if (const char *lan_ep = env("SSB64_LAN_JOIN")) {
        port_netlobby_lan_join(lan_ep, fkind);
        auto_sync_fkind(fkind);
        sAuto.join = false;
        return true;
    }

    if (sAuto.host) {
        std::remove(sAuto.code_file.c_str());
        port_netlobby_host(sAuto.server.c_str(), fkind, stage, 3, 0, 2);
    }
    auto_sync_fkind(fkind);
    return false;
}

/* host: publish the minted room code to the code file once the room is
 * open — joiners poll it (same-machine test rigs). */
static void auto_code_write(void)
{
    if (!sAuto.host || sAuto.code_written || sLobby.state != PORT_NETLOBBY_WAITING) {
        return;
    }
    if (FILE *f = std::fopen(sAuto.code_file.c_str(), "w")) {
        std::fputs(sLobby.code.c_str(), f);
        std::fclose(f);
        sAuto.code_written = true;
    }
}

/* joiner: poll the code file until the host publishes a code.
 * SSB64_LOBBY_CODE supplies the code directly — needed where a shared
 * code file can't exist (browser tabs, remote machines). */
static void auto_join_poll(void)
{
    if (!sAuto.join || sLobby.state != PORT_NETLOBBY_IDLE) {
        return;
    }
    if (const char *direct = env("SSB64_LOBBY_CODE")) {
        char code[16] = {};
        if (std::strlen(direct) >= 6) {
            std::snprintf(code, sizeof(code), "%.6s", direct);
            if (sAuto.spectate) {
                port_netlobby_spectate(sAuto.server.c_str(), code);
            } else {
                port_netlobby_join(sAuto.server.c_str(), code, sLobby.my_fkind);
            }
            sAuto.join = false;
        }
        return;
    }
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

/* autostart gate: everyone in the room must have picked (PICK received);
 * SSB64_LOBBY_MIN_PLAYERS waits for a full room (default: start as soon
 * as anyone is ready). Spectators don't gate the start. */
static bool auto_room_ready(void)
{
    int min_players = env("SSB64_LOBBY_MIN_PLAYERS") ? std::atoi(env("SSB64_LOBBY_MIN_PLAYERS")) : 2;
    int players = 1;
    for (const Peer &p : sLobby.peers) {
        if (p.slot >= 0 && p.slot <= 3 && p.connected) {
            players++;
        }
    }
    if (players < min_players) {
        return false;
    }
    for (const Peer &p : sLobby.peers) {
        /* spectators don't gate the start - they only need the STARTM */
        if (p.slot >= 0 && p.slot <= 3 && p.connected && !p.got_pick) {
            return false;
        }
    }
    return true;
}

static void auto_start_check(void)
{
    if (!sAuto.host || !sAuto.autostart || sLobby.state != PORT_NETLOBBY_CONNECTED) {
        return;
    }
    if (!auto_room_ready()) {
        return;
    }
    if (sAuto.start_after_ms == 0) {
        sAuto.start_after_ms = port_udp_now_ms() + 1000;
    } else if (static_cast<int>(port_udp_now_ms() - sAuto.start_after_ms) >= 0) {
        port_netlobby_start_match();
        sAuto.autostart = false;
    }
}

void update_auto(void)
{
    if (auto_bootstrap()) {
        return;
    }
    auto_code_write();
    auto_join_poll();
    auto_start_check();
}

} // namespace ssb64::netlobby
