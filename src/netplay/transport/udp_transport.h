#pragma once

/* Minimal non-blocking UDP endpoint for netplay (sys/netpeer.c).
 *
 * Lives in the port layer so decomp TUs never include winsock2.h/windows.h
 * (whose near/far macros collide with decomp struct fields). One endpoint per
 * process; addresses are "host:port" strings ("*" or "0.0.0.0" binds all).
 *
 * Optional network simulation for testing (host-side only, never game state):
 *   SSB64_NETPLAY_SIM_LATENCY_MS  one-way delay added to every sent packet
 *   SSB64_NETPLAY_SIM_JITTER_MS   extra random delay in [0, jitter]
 *   SSB64_NETPLAY_SIM_LOSS_PCT    percent of sent packets dropped
 *   SSB64_NETPLAY_SIM_SEED        PRNG seed for jitter/loss (default 1) */

#ifdef __cplusplus
extern "C" {
#endif

int port_udp_open(const char *bind_spec);      /* 1 on success */
int port_udp_set_peer(const char *peer_spec);  /* 1 on success */
void port_udp_close(void);
int port_udp_is_open(void);

/* Sends to the peer. Returns size on success (including when the simulator
 * queued or dropped it, as a real lossy network would), -1 on error. */
int port_udp_send(const void *data, int size);

/* Returns the size of one received datagram, 0 when none is pending, -1 on
 * error. Datagrams from addresses other than the peer are ignored. */
int port_udp_recv(void *buffer, int capacity);

/* Lobby / NAT traversal (src/netplay/session/netlobby.cpp). */
int port_udp_send_to(const char *spec, const void *data, int size);

/* Like port_udp_recv but from anyone; writes the sender's "ip:port" (or
 * "relay" for traffic unwrapped from the server relay) into `from`. */
int port_udp_recv_from(void *buffer, int capacity, char *from, int from_capacity);

/* Relay fallback: with hooks installed, port_udp_send hands every payload to
 * `send` and receives poll `recv` first (the lobby routes both through its
 * WebSocket to services/netplay-lobby). NULL hooks return to direct UDP. */
typedef int (*port_udp_relay_send_fn)(const void *data, int size);   /* 1 = sent */
typedef int (*port_udp_relay_recv_fn)(void *buffer, int capacity);  /* size, 0 = none */
void port_udp_set_relay_hooks(port_udp_relay_send_fn send, port_udp_relay_recv_fn recv);
int port_udp_is_relay(void);

/* Multi-peer links (online matches with more than 2 players). A link is
 * keyed by the remote's room slot: 0..3 are players (the local slot stays
 * unused), 4..7 are spectator links (host side only, always relayed).
 * Relayed frames are prefixed with a slot byte: [to] on send, [from] on
 * receive, which is what the v2 lobby worker routes on. */
#define PORT_UDP_LINK_MAX 8
int port_udp_link_set(int link, const char *spec);   /* "ip:port" or "relay" */
void port_udp_link_clear(int link);
void port_udp_link_clear_all(void);
int port_udp_link_is_set(int link);
int port_udp_link_is_relay(int link);
int port_udp_link_send(int link, const void *data, int size);

/* Receives one datagram addressed to any configured link; *out_link gets
 * the remote slot it arrived from (-1 when the sender matches no link).
 * Returns size, 0 none, -1 error. */
int port_udp_link_recv(int *out_link, void *buffer, int capacity);

/* Relayed frames carry a slot byte only in the v2 lobby protocol; the lobby
 * switches this on when the server answered a v2 welcome. Legacy 1v1 rooms
 * forward binary frames verbatim. */
void port_udp_set_relay_framed(int framed);

/* The primary local IPv4 of this machine plus the bound port ("ip:port"),
 * for peers behind the same NAT. Returns 1 on success. */
int port_udp_local_endpoint(char *out, int capacity);

void port_udp_sleep_us(unsigned int usecs);
unsigned int port_udp_now_ms(void);

#ifdef __cplusplus
}
#endif
