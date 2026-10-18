/* SSBL wire protocol: message handlers and roster tracking — split from netlobby.cpp; the wire-protocol overview lives there. */
#include "netlobby.h"
#include "netlobby_internal.h"
#include "netlobby_util.h"
#include "lobby_json.h"
#include "netfriends.h"
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

/* State-free helpers (compat token, env, split, rules reads) live in
 * netlobby_util.cpp; wire JSON parsing in lobby_json.cpp — the names
 * resolve unqualified here. */
using namespace ssb64::netlobby;

namespace ssb64::netlobby {

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
    sLobby.my_game_slot = my_game_slot;
    syNetPeerStartOnlineMatch(my_game_slot, player_count, delay, stage, stocks, items, seed,
                              session, fkinds, costumes, wire_slots);
}

void begin_match(void)
{
    sLobby.after_match = false;
    set_state(PORT_NETLOBBY_IN_MATCH, TR("Match running (rollback)."));
}

static void update_status(void)
{
    if (sLobby.state != PORT_NETLOBBY_CONNECTED) {
        return;
    }
    char text[160];
    int n = count_connected_peers() + 1;
    if (sLobby.spectate) {
        int specs_players = sLobby.player_count;
        std::snprintf(text, sizeof(text),
                      specs_players == 1
                          ? TR("Spectating room %s (%d player) - waiting for the host to start.")
                          : TR("Spectating room %s (%d players) - waiting for the host to start."),
                      sLobby.code.c_str(), specs_players);
    } else if (sLobby.is_host) {
        int watching = count_connected_specs();
        if (watching > 0) {
            std::snprintf(text, sizeof(text),
                          n == 1 ? TR("%d player + %d watching - press Start match (up to 4).")
                                 : TR("%d players + %d watching - press Start match (up to 4)."),
                          n, watching);
        } else {
            std::snprintf(text, sizeof(text),
                          n == 1 ? TR("%d player connected - press Start match (up to 4).")
                                 : TR("%d players connected - press Start match (up to 4)."),
                          n);
        }
    } else {
        /* clients only hold a direct link to the host - count_connected_peers
         * would read 1+1 in a 4-player room; the roster count is the truth */
        if (sLobby.player_count > n) {
            n = sLobby.player_count;
        }
        std::snprintf(text, sizeof(text),
                      n == 1 ? TR("Connected (%d player) - waiting for host to start.")
                             : TR("Connected (%d players) - waiting for host to start."),
                      n);
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

static Peer *peer_for_source(const char *from)
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

static void on_peer_contact(Peer &p, const char *from)
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

static void handle_game_message(Peer &p, const std::vector<std::string> &args)
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
        p.version = args[5].substr(0, 32);   /* display-only, but cap it */
        p.got_pick = true;
        if (args.size() >= 8) {   /* optional trailing fields: rating, name */
            /* Self-declared Elo — clamp to the leaderboard's accepted band
             * (0..4000) so a hostile PICK can't push the local rating math
             * with an absurd value. */
            p.rating = std::clamp(std::atoi(args[7].c_str()), 0, 4000);
            if (args.size() >= 9 && p.name.empty()) {
                /* Wire contract: names are sanitized to the [alnum._-]
                 * alphabet and <=15 chars by local_player_name() — enforce
                 * both bounds on receive so a hostile PICK can't smuggle an
                 * oversized or quote/newline-laden label into the log/menu
                 * or, if it is ever persisted, into the JSON stores. */
                for (const char c : args[8]) {
                    const unsigned char u = static_cast<unsigned char>(c);
                    if ((std::isalnum(u) || c == '_' || c == '-' || c == '.') &&
                        p.name.size() < 15) {
                        p.name += c;
                    }
                }
            }
        }
        if (!p.name_checked && !p.name.empty()) {
            p.name_checked = true;
            if (port_netfriends_is_blocked(p.name.c_str())) {
                /* names are self-claimed, so this is a heads-up rather than
                 * an auto-drop: the host decides after seeing the warn */
                port_log("SSB64 Lobby: BLOCKED player '%s' connected on slot %d\n",
                         p.name.c_str(), p.slot);
                sLobby.status = TR("A player on your block list joined - check the name above.");
                update_status();
            }
        }
        if (p.version != kGameVersion) {
            /* On the host an incompatible peer must not kill the room for
             * everyone: drop just that peer and keep waiting. On a client a
             * mismatch with the host means WE should leave. */
            if (sLobby.is_host) {
                port_log("SSB64 Lobby: player %d version mismatch (%s != %s) - dropped\n",
                         p.slot, p.version.c_str(), kGameVersion);
                peer_drop(p.slot);
                sLobby.status = TR("Dropped a peer on an incompatible version - waiting for players.");
                update_status();
            } else {
                char ver_msg[256];
                std::snprintf(ver_msg, sizeof(ver_msg), TR("Version mismatch - you're on %s, a peer is on %s."),
                              kGameVersion, p.version.c_str());
                fail(ver_msg);
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
                    sLobby.status = TR("Dropped a peer with a different build or mod set.");
                    update_status();
                } else {
                    fail(TR("Build or mod mismatch - all players must run the same build and mods."));
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
static uint32_t lan_key_hash(void)
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
static Peer *lan_accept(const char *from, const std::vector<std::string> &args)
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
        if (diag_relay()) {
            port_log("SSB64 Lobby: udp recv %d bytes from=%s\n", size, from);
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
                if (diag_relay()) {
                    port_log("SSB64 Lobby: HELLO from unmatched source %s\n", from);
                }
                continue;
            }
            if (p->connected && args[1] == "HELLO") {
                /* the peer is still handshaking — its HELLO means it never
                 * heard our ack; resend it every time it asks */
                send_to_peer(*p, "SSBL HELLOACK");
            }
            on_peer_contact(*p, from);
            if (sLobby.lan && !sLobby.is_host && args[1] == "HELLOACK" && args.size() >= 3) {
                sLobby.my_slot = std::atoi(args[2].c_str());   // host-assigned room slot
            }
            continue;
        }
        Peer *p = peer_for_source(from);
        if (p == nullptr) {
            if (diag_relay()) {
                port_log("SSB64 Lobby: packet from unmatched source %s\n", from);
            }
            continue;
        }
        /* any authenticated message from a known endpoint proves the path —
         * promote even if the one-shot HELLOACK was lost in transit */
        on_peer_contact(*p, from);
        handle_game_message(*p, args);
    }
}

/* ---- server messages ---- */

static void update_peer_endpoints(int slot, const nlohmann::json &entry)
{
    /* clients only ever link to the host: other clients' traffic arrives
     * forwarded by the host once the match starts */
    if (!sLobby.is_host && slot != 0) {
        return;
    }
    std::string pub = json_str(entry, "public");
    std::string local = json_str(entry, "local");
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

/* The "peers" roster (v2) lists every participant in a top-level "players"
 * array of objects — iterate it structurally (the old substring scan could
 * match "slot": inside a string value or a nested object). */
static void on_roster(const std::string &text)
{
    bool seen[8] = {};
    if (sLobby.my_slot >= 0 && sLobby.my_slot < 8) {
        seen[sLobby.my_slot] = true;
    }

    const nlohmann::json doc = json_parse(text);
    const auto players = doc.find("players");
    if (players == doc.end() || !players->is_array()) {
        return;
    }
    for (const nlohmann::json &entry : *players) {
        if (!entry.is_object()) {
            continue;
        }
        const int slot = json_int(entry, "slot");
        if (slot >= 0 && slot < 8) {
            seen[slot] = true;
        }
        if (is_spec_slot(slot)) {
            /* spectators have no endpoints to exchange: the host just opens
             * a relay link and (mid-match) feeds it through the engine */
            if (sLobby.is_host && json_str(entry, "role") == "spec") {
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
                Peer *p = peer_for_slot(slot);
                if (p != nullptr) {
                    p->ready = json_str(entry, "ready") == "true";
                }
            } else if (sLobby.server_version == 2 &&
                       (json_str(entry, "ready") == "true") != sLobby.my_ready) {
                /* the room's copy of our flag went stale (a reconnect resets
                 * it server-side) - re-announce our truth */
                send_ready_state();
            }
        }
    }
    int count = 0;
    for (int i = 0; i < 4; i++) {      /* player_count never includes specs */
        if (seen[i]) {
            count++;
        }
    }
    // room slots can be sparse; count who is here. Spectators keep their
    // own count unless the roster shows actual players.
    if (!sLobby.spectate || count > 0) {
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
    const nlohmann::json doc = json_parse(text);
    const std::string type = json_str(doc, "type");

    if (type == "welcome") {
        sLobby.ws_welcomed = true;
        sLobby.code = json_str(doc, "room");
        sLobby.up_key = json_str(doc, "upkey");
        if (sLobby.quickplay) {
            /* role=queue: the room tells us which side we landed on */
            sLobby.is_host = (json_str(doc, "role") == "host");
        }
        port_netstats_note_room(sLobby.code.c_str());
        const int slot = json_int(doc, "slot");
        if (slot >= 0) {
            sLobby.server_version = 2;
            sLobby.my_slot = slot;
            sLobby.up_key = json_str(doc, "upkey");
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
            char room_msg[160];
            std::snprintf(room_msg, sizeof(room_msg),
                          TR("Spectating room %s - waiting for the host to start."), sLobby.code.c_str());
            set_state(PORT_NETLOBBY_CONNECTED, room_msg);
        } else if (sLobby.is_host &&
                   (sLobby.state == PORT_NETLOBBY_CREATING ||
                    (sLobby.quickplay && sLobby.state == PORT_NETLOBBY_JOINING))) {
            set_state(PORT_NETLOBBY_WAITING,
                      sLobby.quickplay ? TR("Queue room open - waiting for an opponent.")
                                       : TR("Room open - share the code, waiting for players."));
        } else if (!sLobby.is_host && sLobby.state == PORT_NETLOBBY_JOINING) {
            set_state(PORT_NETLOBBY_PUNCHING, TR("Joined room - connecting to host."));
        }
        send_endpoint_if_ready();
        return;
    }
    if (type == "peers") {         // v2 roster broadcast
        on_roster(text);
        return;
    }
    if (type == "peer") {          // v1: single opponent endpoints
        update_peer_endpoints(sLobby.is_host ? 1 : 0, doc);
        sLobby.player_count = 2;
        if (sLobby.state == PORT_NETLOBBY_WAITING || sLobby.state == PORT_NETLOBBY_JOINING) {
            set_state(PORT_NETLOBBY_PUNCHING, TR("Peer joined - punching through NAT."));
        }
        return;
    }
    if (type == "peer_relay") {    // v2: one peer fell back to the relay
        const int slot = json_int(doc, "slot");
        if (slot < 0) {
            return;                /* absent/invalid — free peers are slot -1 */
        }
        Peer *p = peer_for_slot(slot);
        /* A peer that asked for relay but already connected via direct is
         * fine to leave alone *only* if the direct path is proven both ways
         * (got_pong). Asymmetric NATs do receive our hellos but never deliver
         * our replies — without the flip, that peer sits connected-but-deaf
         * until it times out. */
        if (p != nullptr && (!p->connected || !p->got_pong)) {
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
        const int left = json_int(doc, "slot");
        if (left >= 0) {           // v2: one participant left
            if (left == 0) {       // host left: the room is over
                if (sLobby.state != PORT_NETLOBBY_IN_MATCH) {
                    fail(TR("Host left the room."));
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
            set_state(PORT_NETLOBBY_WAITING, TR("Peer left - waiting for players."));
        } else if (!sLobby.is_host && sLobby.state != PORT_NETLOBBY_IN_MATCH) {
            fail(TR("Host left the room."));
        }
        return;
    }
    if (type == "host_left") {
        if (sLobby.state != PORT_NETLOBBY_IN_MATCH) {
            fail(TR("Host left the room."));
        }
        return;
    }
    if (type == "error") {
        std::string msg = json_str(doc, "reason");
        if (msg.empty()) {
            msg = json_str(doc, "message");
        }
        if (msg == "room-full") {
            if (sLobby.quickplay && sLobby.queue_idx + 1 < kQueueRoomCount) {
                /* queue code is full — roll to the next well-known room */
                sLobby.queue_idx++;
                char code[16];
                std::snprintf(code, sizeof(code), "QUEUE%d", 2 + sLobby.queue_idx);
                sLobby.code = code;
                port_ws_close();
                port_ws_uplink_close();
                connect_server();
                char msgbuf[128];
                std::snprintf(msgbuf, sizeof(msgbuf), TR("Queue %d full - trying the next one..."),
                              2 + sLobby.queue_idx);
                sLobby.status = msgbuf;
                return;
            }
            fail(TR("Room is full (4 players)."));
        } else if (msg == "version-mismatch") {
            fail(TR("Room runs a different netplay build."));
        } else if (msg == "room-in-use") {
            fail(TR("Room code already in use - try hosting again."));
        } else if (msg == "no-such-room") {
            fail(TR("No such room."));
        } else if (msg == "bad-code") {
            fail(TR("Invalid room code."));
        } else {
            char err[256];
            std::snprintf(err, sizeof(err), TR("Server error: %s"), msg.c_str());
            fail(err);
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
                    fail(TR("Could not reach the host."));
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
            const std::string pname = local_player_name();
            std::snprintf(msg, sizeof(msg), "SSBL PICK %d %u %d %s %s %d %s", sLobby.my_fkind,
                          port_udp_now_ms(), local_tap_jump_off(), kGameVersion,
                          token.c_str(), port_netstats_rating(), pname.c_str());
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
                fail(TR("Lost connection to the host."));
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

} // namespace ssb64::netlobby
