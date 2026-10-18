/* Host start sequence and auto-lobby (SSB64_AUTOLOBBY env) — split from netlobby.cpp; the wire-protocol overview lives there. */
#include "netlobby.h"
#include "netlobby_internal.h"
#include "netlobby_util.h"
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
            fail(TR("No players responded - match aborted."));
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

} // namespace ssb64::netlobby
