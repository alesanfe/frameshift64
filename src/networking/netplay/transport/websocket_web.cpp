/* websocket_web.cpp — Emscripten WebSocket backend for the netplay lobby.
 *
 * Browsers have no raw sockets; the WebSocket API is the only transport.
 * emscripten_websocket_* gives a real ws/wss client (TLS is the browser's,
 * so wss:// works here even though the portable BSD backend can't do it).
 *
 * Threading model: the browser delivers events on the main thread between
 * frames. Emscripten's fibers mean the game coroutine yields back to the
 * event loop every frame, so callbacks fire then — messages land in the
 * queues below and the lobby polls them exactly as on other platforms.
 * No threads, no blocking I/O.
 *
 * The uplink is a second connection that never reads (as on desktop):
 * browser ws sends serialize per-connection the same way.
 *
 * Guarded by __EMSCRIPTEN__; websocket_transport.cpp carries the
 * WinHTTP/BSD-socket implementations for desktop builds.
 */

#ifdef __EMSCRIPTEN__

#include "websocket_transport.h"

#include <emscripten/websocket.h>

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

struct WsQueues {
	std::deque<std::string> text;
	std::deque<std::vector<uint8_t>> binary;
};

EMSCRIPTEN_WEBSOCKET_T sWs = 0;
WsQueues sQueues;
PortWsState sState = PORT_WS_CLOSED;
std::string sError;

EMSCRIPTEN_WEBSOCKET_T sUplink = 0;
PortWsState sUplinkState = PORT_WS_CLOSED;
std::string sUplinkError;

EM_BOOL on_open(int /*type*/, const EmscriptenWebSocketOpenEvent */*ev*/, void *user)
{
	*static_cast<PortWsState *>(user) = PORT_WS_OPEN;
	return EM_TRUE;
}

EM_BOOL on_error(int /*type*/, const EmscriptenWebSocketErrorEvent */*ev*/, void *user)
{
	PortWsState *state = static_cast<PortWsState *>(user);
	if (*state != PORT_WS_OPEN) {
		*state = PORT_WS_FAILED;
	}
	if (user == &sState) {
		sError = "websocket error";
	} else {
		sUplinkError = "websocket error";
	}
	return EM_TRUE;
}

EM_BOOL on_close(int /*type*/, const EmscriptenWebSocketCloseEvent *ev, void *user)
{
	PortWsState *state = static_cast<PortWsState *>(user);
	*state = (ev->wasClean || *state == PORT_WS_OPEN) ? PORT_WS_CLOSED : PORT_WS_FAILED;
	if (user == &sState && ev->code != 1000 && ev->code != 1005) {
		char buf[96];
		std::snprintf(buf, sizeof(buf), "closed: code=%u %.*s", ev->code,
		              static_cast<int>(sizeof(ev->reason)), ev->reason);
		sError = buf;
	}
	return EM_TRUE;
}

EM_BOOL on_message(int /*type*/, const EmscriptenWebSocketMessageEvent *ev, void */*user*/)
{
	if (ev->isText) {
		/* numText bytes are NOT NUL-terminated; copy with an explicit 0. */
		sQueues.text.emplace_back(reinterpret_cast<const char *>(ev->data),
		                          ev->numBytes);
	} else {
		sQueues.binary.emplace_back(ev->data, ev->data + ev->numBytes);
	}
	return EM_TRUE;
}

EMSCRIPTEN_WEBSOCKET_T ws_open(const char *url, PortWsState *state,
                               bool with_recv)
{
	EmscriptenWebSocketCreateAttributes attr = {};
	attr.url = url;
	attr.protocols = nullptr;          /* no subprotocol — matches desktop */
	attr.createOnMainThread = EM_TRUE;
	EMSCRIPTEN_WEBSOCKET_T ws = emscripten_websocket_new(&attr);
	if (ws <= 0) {
		return 0;
	}
	*state = PORT_WS_CONNECTING;
	emscripten_websocket_set_onopen_callback(ws, state, on_open);
	emscripten_websocket_set_onerror_callback(ws, state, on_error);
	emscripten_websocket_set_onclose_callback(ws, state, on_close);
	if (with_recv) {
		emscripten_websocket_set_onmessage_callback(ws, nullptr, on_message);
	}
	return ws;
}

int ws_send(EMSCRIPTEN_WEBSOCKET_T ws, const void *data, int size, bool text)
{
	if (ws <= 0) {
		return 0;
	}
	EMSCRIPTEN_RESULT rc = text
		? emscripten_websocket_send_utf8_text(ws, static_cast<const char *>(data))
		: emscripten_websocket_send_binary(ws, const_cast<void *>(data), size);
	return (rc == EMSCRIPTEN_RESULT_SUCCESS) ? 1 : 0;
}

template <typename T>
int poll_queue(std::deque<T> &q, void *out, int capacity)
{
	if (q.empty()) {
		return 0;
	}
	const T &front = q.front();
	const int n = static_cast<int>(front.size());
	if (n > capacity) {
		return 0;                      /* oversized frame: drop silently */
	}
	std::memcpy(out, front.data(), n);
	if constexpr (std::is_same_v<T, std::string>) {
		if (n < capacity) {
			static_cast<char *>(out)[n] = '\0';
		}
	}
	q.pop_front();
	return n;
}

} // namespace

extern "C" int port_ws_connect(const char *url)
{
	if (emscripten_websocket_is_supported() == EM_FALSE) {
		sError = "websockets unsupported in this browser";
		sState = PORT_WS_FAILED;
		return 0;
	}
	port_ws_close();
	sQueues = WsQueues{};
	sError.clear();
	sWs = ws_open(url, &sState, /*with_recv=*/true);
	if (sWs <= 0) {
		sError = "emscripten_websocket_new failed";
		sState = PORT_WS_FAILED;
		return 0;
	}
	return 1;
}

extern "C" void port_ws_close(void)
{
	if (sWs > 0) {
		emscripten_websocket_close(sWs, 1000, "");
		emscripten_websocket_delete(sWs);
		sWs = 0;
	}
	sState = PORT_WS_CLOSED;
}

extern "C" int port_ws_state(void)    { return sState; }
extern "C" const char *port_ws_error(void) { return sError.c_str(); }

extern "C" int port_ws_send_text(const char *text)
{
	return ws_send(sWs, text, static_cast<int>(std::strlen(text)), true);
}

extern "C" int port_ws_send_binary(const void *data, int size)
{
	/* Prefer the uplink once it is open — mirrors the desktop behaviour the
	 * lobby relies on (relay traffic never queues behind a control recv). */
	if (sUplinkState == PORT_WS_OPEN) {
		return ws_send(sUplink, data, size, false);
	}
	return ws_send(sWs, data, size, false);
}

extern "C" int port_ws_poll_text(char *out, int capacity)
{
	return poll_queue(sQueues.text, out, capacity);
}

extern "C" int port_ws_poll_binary(void *out, int capacity)
{
	return poll_queue(sQueues.binary, out, capacity);
}

extern "C" int port_ws_uplink_connect(const char *url)
{
	port_ws_uplink_close();
	sUplinkError.clear();
	sUplink = ws_open(url, &sUplinkState, /*with_recv=*/false);
	return (sUplink > 0) ? 1 : 0;
}

extern "C" void port_ws_uplink_close(void)
{
	if (sUplink > 0) {
		emscripten_websocket_close(sUplink, 1000, "");
		emscripten_websocket_delete(sUplink);
		sUplink = 0;
	}
	sUplinkState = PORT_WS_CLOSED;
}

extern "C" int port_ws_uplink_state(void) { return sUplinkState; }

extern "C" int port_ws_uplink_send(const void *data, int size)
{
	return ws_send(sUplink, data, size, false);
}

#endif /* __EMSCRIPTEN__ */
