#pragma once

/* Online lobby: room codes through server/lobby-server.js, UDP hole punching
 * (public + LAN candidates) with relay fallback, character picks, and the
 * hand-off into a rollback netplay match (sys/netpeer.c). Non-blocking;
 * port_netlobby_update() runs once per frame from PortPushFrame. */

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
void port_netlobby_cancel(void);
void port_netlobby_start_match(void);
void port_netlobby_set_fkind(int fkind);

int port_netlobby_state(void);
const char *port_netlobby_status(void);
const char *port_netlobby_code(void);
int port_netlobby_peer_fkind(void);
int port_netlobby_is_host(void);
int port_netlobby_is_relay(void);
int port_netlobby_ping_ms(void);

/* Called by sys/netpeer.c when an online match's VS session ends. */
void port_netlobby_on_match_end(void);

#ifdef __cplusplus
}
#endif
