/* Internal shared declarations for the netlobby translation units — the
 * protocol overview lives in netlobby.cpp. Only session/netlobby*.cpp files
 * include this header; the public ABI stays in netlobby.h.
 *
 * Split layout:
 *   netlobby_state.cpp      Lobby/Peer/AutoLobby state + peer bookkeeping
 *   netlobby_transport.cpp  STUN, websocket control/uplink, relay hooks, sends
 *   netlobby_protocol.cpp   SSBL wire messages and roster tracking
 *   netlobby_start.cpp      host start sequence + auto-lobby env bootstrap
 *   netlobby.cpp            public extern "C" API + per-frame update
 *
 * Everything below sits in ssb64::netlobby so the unqualified call sites
 * moved out of the original anonymous namespace keep resolving. */
#pragma once

/* Umbrella for the netlobby TUs' shared include set — each file used to
 * carry the same ~25-line block; only per-file extras remain at the TU. */
#include "netlobby.h"
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

#include <ship/Context.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

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

namespace ssb64::netlobby {

/* STUN endpoints tried in order — the first is Cloudflare's, the second a
 * public fallback so one provider's outage doesn't kill hole-punching.
 * gNetplay.StunServer overrides the whole list when set. */
constexpr const char *kStunServers[] = {
    "stun.cloudflare.com:3478",
    "stun.l.google.com:19302",
};
constexpr int kStunServerCount = 2;
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

constexpr int kMaxRemotePeers = 7;    /* room slots 1..3 are players, 4..7 spectators */
constexpr int kLanDefaultPort = 41700;
constexpr int kQueueRoomCount = 8;    /* QUEUE2..QUEUE9 — quick-play pool */

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
    unsigned int spec_start_resend_ms = 0;/* host: last STARTM resend to this spec link */
    int relay_requested = 0;              /* client: asked the room to relay */
    bool ready = false;                   /* v2 roster "ready" flag */
    int rating = -1;                      /* Elo declared in PICK (-1 unknown) */
    std::string name;                     /* display name declared in PICK (self-claimed) */
    bool name_checked = false;            /* friends/block check ran for this name */
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
    int stun_attempt = 0;                 /* which STUN server we're on */
    std::string stun_public;
    unsigned int stun_ms = 0;
    bool ws_welcomed = false;
    bool endpoint_sent = false;
    unsigned int last_keepalive_ms = 0;
    unsigned int ping_sent_ms = 0;          /* last {"type":"ping"} send time */
    unsigned int lobby_rtt_ms = 0;          /* ping -> keepalive_ack RTT; survives across sessions */

    bool after_match = false;
    bool my_ready = false;                /* our ready-check flag (v2 rooms) */
    bool quickplay = false;               /* joined via role=queue (matchmaking) */
    int queue_idx = 0;                    /* which QUEUE<n> code we're trying */
    int my_game_slot = -1;                /* our slot in the running match */
    int match_winner = -1;                /* game slot that won; -1 draw/unknown */
    unsigned int match_duration = 0;      /* ticks the finished match ran */
    std::string last_start_payload;       /* last STARTM sent; specs arriving pre-start get a copy */

    std::mt19937 rng{std::random_device{}()};
};

extern Lobby sLobby;
extern AutoLobby sAuto;

/* ---- netlobby_state.cpp ---- */
void set_state(PortNetLobbyState state, const std::string &status);
Peer *peer_for_slot(int slot);
Peer *peer_new(int slot);
void peer_drop(int slot);
int count_connected_peers(void);
int count_connected_specs(void);

/* ---- netlobby_transport.cpp ---- */
void shutdown_net(void);
void fail(const std::string &why);
bool diag_relay(void);
int relay_send(const void *data, int size);      /* hooked by protocol via port_udp_set_relay_hooks */
int relay_recv(void *buffer, int capacity);
void send_stun(void);
bool parse_stun(const uint8_t *msg, int size);
std::string new_code(void);
void connect_server(void);
void open_uplink(void);
void send_endpoint_if_ready(void);
void send_ready_state(void);
void send_to_peer(Peer &p, const char *text);

/* ---- netlobby_protocol.cpp ---- */
int costume_for(int slot, const int *fkinds, int count);
int gather_roster(int *wire_slots, int *fkinds, int *tjs);
void apply_match_start(int player_count, int my_game_slot, unsigned int stage,
                       unsigned int stocks, int items, unsigned int seed,
                       unsigned int delay, int autoz, int comp, int neutral,
                       int hazards, const int *fkinds, const int *costumes,
                       const int *tap_jumps, const int *wire_slots,
                       unsigned int session);
void begin_match(void);
std::string build_start_payload(void);
std::string lan_hello(void);
void pump_transport(void);
void on_server_text(const char *text);
void pump_peers(unsigned int now);

/* ---- netlobby_start.cpp ---- */
void host_start_sequence(unsigned int now);
void update_auto(void);

} // namespace ssb64::netlobby
