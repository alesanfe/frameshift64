#pragma once

/* Minimal WebSocket client for the netplay lobby (services/netplay-lobby). One
 * connection per process. Connecting and receiving happen on a background
 * thread; the lobby polls once per frame from the game thread.
 * Windows: WinHTTP (ws:// and wss://, TLS via the OS). Other platforms: a
 * portable BSD-socket client — ws:// only; wss:// fails because this tree
 * has no TLS library. SSB64_PORTABLE_WS forces the portable backend on
 * Windows (verification builds). */

#ifdef __cplusplus
extern "C" {
#endif

enum PortWsState {
    PORT_WS_CLOSED,
    PORT_WS_CONNECTING,
    PORT_WS_OPEN,
    PORT_WS_FAILED
};

/* url: ws://host[:port]/path?query or wss://... Returns 1 if the attempt started. */
int port_ws_connect(const char *url);
void port_ws_close(void);
int port_ws_state(void);
const char *port_ws_error(void);

int port_ws_send_text(const char *text);
int port_ws_send_binary(const void *data, int size);

/* Next received text message (NUL-terminated) into `out`; 1 if one was
 * returned, 0 if none pending. Binary messages go to the binary queue. */
int port_ws_poll_text(char *out, int capacity);

/* Next received binary message; returns its size, 0 if none pending. */
int port_ws_poll_binary(void *out, int capacity);

/* Uplink: a second, send-only websocket. Synchronous WinHTTP websocket
 * sends serialize behind a pending receive on the same handle (measured
 * multi-second stalls), so relayed game traffic goes out on this dedicated
 * connection which never issues a receive. port_ws_uplink_connect performs
 * the handshake synchronously on the calling thread. */
int port_ws_uplink_connect(const char *url);
void port_ws_uplink_close(void);
int port_ws_uplink_state(void);
int port_ws_uplink_send(const void *data, int size);

#ifdef __cplusplus
}
#endif
