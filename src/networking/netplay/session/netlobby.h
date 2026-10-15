#pragma once

/* Online lobby: room codes through services/netplay-lobby (Worker + Durable
 * Object), UDP hole punching via Cloudflare STUN with WebSocket relay fallback, character picks, and the
 * hand-off into a rollback netplay match (sys/netpeer.c). Non-blocking;
 * port_netlobby_update() runs once per frame from PortPushFrame. */

/* Lobby server (services/netplay-lobby). Release builds bake the deployed Worker
 * in via -DSSB64_NETPLAY_SERVER=...; players can override it in the menu. */
#ifdef SSB64_NETPLAY_SERVER
#define PORT_NETLOBBY_DEFAULT_SERVER SSB64_NETPLAY_SERVER
#else
#define PORT_NETLOBBY_DEFAULT_SERVER "wss://smash-netplay.jumpcircuit.workers.dev"
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum PortNetLobbyState {
    PORT_NETLOBBY_IDLE,
    PORT_NETLOBBY_CREATING,     /* host: waiting for a room code */
    PORT_NETLOBBY_WAITING,      /* host: room open, waiting for someone to join */
    PORT_NETLOBBY_JOINING,      /* client: waiting for the server introduction */
    PORT_NETLOBBY_PUNCHING,     /* both: establishing the direct (or relayed) path */
    PORT_NETLOBBY_CONNECTED,    /* both: talking; host can start */
    PORT_NETLOBBY_STARTING,     /* host: sent START, waiting for the client's GO */
    PORT_NETLOBBY_IN_MATCH,
    PORT_NETLOBBY_FAILED
};

void port_netlobby_update(void);

void port_netlobby_host(const char *server, int fkind, int stage, int stocks, int items, int delay);
void port_netlobby_join(const char *server, const char *code, int fkind);
/* LAN (direct IP): no lobby server or STUN. Host binds a UDP port and hands
 * out room slots to whoever says HELLO; joiner passes "host-ip[:port]". */
void port_netlobby_lan_host(int port, int fkind, int stage, int stocks, int items, int delay);
void port_netlobby_lan_join(const char *endpoint, int fkind);
/* Read-only spectator: joins the room on a spectator slot (4..7) and
 * receives the match's confirmed input streams forwarded by the host over
 * the relay. Never sends game packets, never joins pacing. */
void port_netlobby_spectate(const char *server, const char *code);
void port_netlobby_cancel(void);
void port_netlobby_start_match(void);
void port_netlobby_set_fkind(int fkind);

int port_netlobby_state(void);
const char *port_netlobby_status(void);
const char *port_netlobby_code(void);
int port_netlobby_peer_fkind(void);
int port_netlobby_is_host(void);
int port_netlobby_is_spectator(void);
int port_netlobby_is_relay(void);
int port_netlobby_ping_ms(void);

/* Multi-player roster (up to 4 participants; this instance is slot
 * port_netlobby_my_slot(), remote participants are the peers). */
int port_netlobby_player_count(void);
int port_netlobby_my_slot(void);
int port_netlobby_peer_count(void);
int port_netlobby_peer_slot(int index);          /* remote's game slot, -1 if none */
int port_netlobby_peer_fkind_at(int index);      /* -1 until the peer picks */
int port_netlobby_peer_connected_at(int index);
int port_netlobby_peer_ping_at(int index);       /* -1 unknown */

/* Per-peer wire stats during a match (packet sequence gaps/duplicates/
 * out-of-order counts + the remote's declared advantage, i.e. our rollback
 * depth for its inputs). Returns 0 when the peer has no live netpeer stats. */
int port_netlobby_peer_stats_at(int index, unsigned *gaps, unsigned *dups, unsigned *ooo, int *advantage);

/* 1 after an online match has ended while the players are still connected
 * (each is back in their own local menus; the host can start a rematch). */
int port_netlobby_after_match(void);

/* Called by sys/netpeer.c when an online match's VS session ends. */
void port_netlobby_on_match_end(void);

#ifdef __cplusplus
}
#endif
