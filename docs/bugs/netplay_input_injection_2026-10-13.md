# Netplay: arbitrary input injection via unauthenticated relay routing

Date: 2026-10-13
Severity: P0 (any room participant could inject inputs as any other player)
Found by: multiplayer security audit (`docs/investigations/netplay/audit_netplay_multiplayer_2026-10-13.md`)

## Root cause

Three missing checks combined into a full spoofing chain:

1. The Worker's v2 binary path routed `bytes[0]` (attacker-controlled
   `to_slot`) to any slot — the "clients only address slot 0" star
   existed only in a comment, not in code.
2. `syNetPeerHandleForwardPacket` accepted FORWARD datagrams on any
   link, on the host as well as clients, and never verified that the
   claimed `src_player` was a remote only reachable via forwarding.
3. `up=1&slot=N` uplinks required nothing beyond the room code, so an
   attacker could attach to another player's slot, rewrite its endpoint
   in the roster and send binary with the stolen slot identity.
   `session_id` equalled the broadcast `rng_seed`, so neither packets nor
   sessions carried any secret.

## Fix

- `services/netplay-lobby/src/index.js`, `services/netplay-lobby/src/node-lobby.js`: per-slot
  `upkey` issued in `welcome`, required on the uplink upgrade; star
  topology enforced (clients → slot 0 only, host → client slots only);
  relayed binary only flows on links the owner marked `relay`;
  `from_slot` stamped from the authenticated socket; per-socket and
  per-room rate limits.
- `decomp/src/sys/netpeer.c`: FORWARD packets rejected on the host, on
  links other than the host link, and when the inner `src_player` has a
  direct link; `session_id` is now a lobby-generated token independent
  of `rng_seed` (0 = legacy seed fallback for old peers).
- `src/netplay/port_udp.cpp`: `link_of` validates the relayed frame's slot
  byte against an active relay-backed link.
- `src/netplay/netlobby.cpp`: `upkey` from `welcome` sent as `&key=` on the
  uplink; session id generated on match start and carried in
  `STARTM`/`START`.

## Verification

`node services/netplay-lobby/tests/test-lobby-security.js` — 15/15 checks covering uplink key
accept/reject, client→client drops, relay-flag gating in both
directions, `from_slot` stamping and the join/text rate limits.

## Notes for reviewers

The packet checksum is FNV-1a and the session id travels in every
packet — neither is authentication. Sender identity comes from the
authenticated WebSocket connection (server side) and from link
attribution + the star invariant (client side). Room codes remain the
only room secret, now with bounded guessing cost.
