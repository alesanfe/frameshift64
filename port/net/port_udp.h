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

void port_udp_sleep_us(unsigned int usecs);
unsigned int port_udp_now_ms(void);

#ifdef __cplusplus
}
#endif
