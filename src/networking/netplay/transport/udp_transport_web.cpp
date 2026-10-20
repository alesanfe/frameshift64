/* udp_transport_web.cpp — Emscripten UDP endpoint, relay-only.
 *
 * Browsers have no UDP. Netplay on web rides the lobby relay over
 * websockets end-to-end: every link is a relay link, port_udp_send routes
 * through the relay hook, and "receiving" is polling the relay queue.
 * The [slot]-byte framing and the trusted-link check are identical to
 * udp_transport.cpp so the v2 worker routes frames the same way.
 *
 * What this means for callers: link_set("relay") works, link_set("ip:port")
 * fails (no direct paths), STUN never fires, LAN is absent — matching the
 * documented web-relay-only scope.
 *
 * Guarded by __EMSCRIPTEN__; udp_transport.cpp carries the socket backend.
 */

#ifdef __EMSCRIPTEN__

#include "udp_transport.h"
#include "port_log.h"

#include <chrono>
#include <cstring>
#include <stdint.h>
#include <string>

namespace {

port_udp_relay_send_fn sRelaySend;
port_udp_relay_recv_fn sRelayRecv;
int sRelayFramed;
int sOpen;

struct Link {
	bool active = false;
	/* `relay` is implicit on web — a set link is always a relay link. */
};

Link sLinks[PORT_UDP_LINK_MAX];

bool valid_link(int link)
{
	return link >= 0 && link < PORT_UDP_LINK_MAX;
}

} // namespace

extern "C" int port_udp_open(const char * /*bind_spec*/)
{
	/* "Open" just means ready to route relay traffic — there is no socket. */
	sOpen = 1;
	return 1;
}

extern "C" int port_udp_set_peer(const char * /*peer_spec*/)
{
	/* Direct peers don't exist on web; the lobby always installs relay
	 * hooks + per-slot links, so this is accepted as a no-op. */
	return sOpen;
}

extern "C" void port_udp_close(void)
{
	sOpen = 0;
	sRelaySend = nullptr;
	sRelayRecv = nullptr;
	for (Link &l : sLinks) {
		l.active = false;
	}
}

extern "C" int port_udp_is_open(void)
{
	return sOpen;
}

extern "C" int port_udp_send(const void *data, int size)
{
	if (sRelaySend == nullptr || data == nullptr || size <= 0) {
		return -1;
	}
	return sRelaySend(data, size) ? size : -1;
}

extern "C" int port_udp_recv(void *buffer, int capacity)
{
	if (sRelayRecv == nullptr || buffer == nullptr || capacity <= 0) {
		return 0;
	}
	return sRelayRecv(buffer, capacity);
}

extern "C" int port_udp_send_to(const char *spec, const void *data, int size)
{
	(void)spec;                          /* all traffic goes to the relay */
	return port_udp_send(data, size);
}

extern "C" int port_udp_recv_from(void *buffer, int capacity,
                                  char *from, int from_capacity)
{
	/* Same contract as the socket backend: relayed datagrams arrive
	 * framed [from_slot][payload] under the v2 protocol — strip the slot
	 * byte into the "relay:<slot>" source name so peer_for_source can
	 * attribute the packet, and copy out only the payload. Unframed
	 * (v1/legacy) traffic keeps the plain "relay" name. */
	if (buffer == nullptr || capacity <= 0) {
		return -1;
	}
	uint8_t bytes[2048];
	const int size = port_udp_recv(bytes, sizeof(bytes));
	if (size <= 0) {
		return size;
	}
	const int skip = sRelayFramed ? 1 : 0;
	if (size - skip > capacity) {
		return -1;
	}
	std::memcpy(buffer, bytes + skip, size - skip);
	if (from != nullptr && from_capacity > 0) {
		if (sRelayFramed && size >= 1) {
			std::snprintf(from, from_capacity, "relay:%u", bytes[0]);
		} else {
			std::snprintf(from, from_capacity, "relay");
		}
	}
	return size - skip;
}

extern "C" void port_udp_set_relay_hooks(port_udp_relay_send_fn send,
                                         port_udp_relay_recv_fn recv)
{
	sRelaySend = send;
	sRelayRecv = recv;
}

extern "C" int port_udp_is_relay(void)
{
	return (sRelaySend != nullptr) ? 1 : 0;
}

extern "C" void port_udp_set_relay_framed(int framed)
{
	sRelayFramed = (framed != 0);
}

extern "C" int port_udp_link_set(int link, const char *spec)
{
	if (!valid_link(link)) {
		return 0;
	}
	/* Only the "relay" spec is meaningful on web — a literal endpoint has
	 * nothing to bind to, so reject it instead of silently misrouting. */
	if (spec == nullptr || std::strcmp(spec, "relay") != 0) {
		sLinks[link].active = false;
		return 0;
	}
	sLinks[link].active = true;
	return 1;
}

extern "C" void port_udp_link_clear(int link)
{
	if (valid_link(link)) {
		sLinks[link].active = false;
	}
}

extern "C" void port_udp_link_clear_all(void)
{
	for (Link &l : sLinks) {
		l.active = false;
	}
}

extern "C" int port_udp_link_is_set(int link)
{
	return valid_link(link) && sLinks[link].active ? 1 : 0;
}

extern "C" int port_udp_link_is_relay(int link)
{
	return valid_link(link) && sLinks[link].active ? 1 : 0;
}

extern "C" int port_udp_link_send(int link, const void *data, int size)
{
	if (sRelaySend == nullptr || data == nullptr || size <= 0 ||
	    !valid_link(link) || !sLinks[link].active) {
		return -1;
	}
	const uint8_t *bytes = static_cast<const uint8_t *>(data);
	if (!sRelayFramed) {
		return sRelaySend(bytes, size) ? size : -1;
	}
	uint8_t framed[1500];
	if (size + 1 > static_cast<int>(sizeof(framed))) {
		return -1;
	}
	framed[0] = static_cast<uint8_t>(link);
	std::memcpy(framed + 1, bytes, size);
	return sRelaySend(framed, size + 1) ? size : -1;
}

namespace {

/* Unframed relay: single-peer room — attribute the datagram to the first
 * active relay link, matching the desktop fallback. Returns the delivered
 * size, or -1 when nothing can take it (caller reports 0 and leaves the
 * queue alone — the original drain loop exited rather than discarding). */
int deliver_unframed(const uint8_t *bytes, int size, int *out_link,
                     void *buffer, int capacity)
{
	for (int i = 0; i < PORT_UDP_LINK_MAX; i++) {
		if (sLinks[i].active) {
			if (size > capacity) {
				return -1;
			}
			std::memcpy(buffer, bytes, size);
			*out_link = i;
			return size;
		}
	}
	return -1;
}

/* Framed: [from] slot byte, only trusted if that link is active — same
 * drop rule as link_of() in the socket backend. Returns the delivered
 * payload size, or 0 when the datagram was stray/unrouteable (caller
 * keeps draining). */
int deliver_framed(const uint8_t *bytes, int size, int *out_link,
                   void *buffer, int capacity)
{
	const int link = (size >= 1 && bytes[0] < PORT_UDP_LINK_MAX)
		? bytes[0] : -1;
	const int payload = size - 1;
	if (link < 0 || !sLinks[link].active || payload <= 0 || payload > capacity) {
		return 0;
	}
	std::memcpy(buffer, bytes + 1, payload);
	*out_link = link;
	return payload;
}

} // namespace

extern "C" int port_udp_link_recv(int *out_link, void *buffer, int capacity)
{
	if (sRelayRecv == nullptr || out_link == nullptr ||
	    buffer == nullptr || capacity <= 0) {
		return -1;
	}
	uint8_t bytes[2048];
	for (;;) {
		const int size = sRelayRecv(bytes, sizeof(bytes));
		if (size <= 0) {
			return size;
		}
		const int delivered = sRelayFramed
			? deliver_framed(bytes, size, out_link, buffer, capacity)
			: deliver_unframed(bytes, size, out_link, buffer, capacity);
		if (delivered > 0) {
			return delivered;
		}
		if (delivered < 0) {
			return 0;    /* unrouteable unframed datagram — leave queue, report empty */
		}
	}
}

extern "C" int port_udp_local_endpoint(char *out, int capacity)
{
	if (out != nullptr && capacity > 0) {
		std::snprintf(out, capacity, "relay");   /* no addressable endpoint */
	}
	return 1;
}

extern "C" void port_udp_sleep_us(unsigned int usecs)
{
	/* No spinning on web — the fiber yields to the browser event loop. */
	(void)usecs;
}

extern "C" unsigned int port_udp_now_ms(void)
{
	using namespace std::chrono;
	return static_cast<unsigned int>(
		duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/* No raw UDP in the browser — all traffic is relayed over WebSocket, so the
 * STUN probe is meaningless here. report start-failure and let the UI say so. */
extern "C" int port_udp_stun_probe_start(const char *const *specs, int count)
{
	(void)specs;
	(void)count;
	return 0;
}

extern "C" int port_udp_stun_probe_poll(void)
{
	return 0;
}

extern "C" int port_udp_stun_probe_result(int index, char *out, int capacity)
{
	(void)index;
	(void)out;
	(void)capacity;
	return 0;
}

extern "C" void port_udp_stun_probe_close(void)
{
}

#endif /* __EMSCRIPTEN__ */
