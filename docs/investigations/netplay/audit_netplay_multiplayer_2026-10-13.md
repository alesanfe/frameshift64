# Audit — netplay multiplayer diff (port/net, decomp net*, cloudflare worker)

Reviewed the uncommitted multiplayer work (protocol v2, star topology,
FORWARD relay, ws uplink, per-slot rules) against the multi-layer debt
audit criteria. Ordered by severity.

## Confirmed findings

### SEC-1 (P0): Worker routes binary frames to ANY slot — star not enforced
`services/netplay-lobby/src/index.js` `webSocketMessage`: `const target =
this.socketForSlot(bytes[0])`. The comment says "Clients only address
slot 0 (star)", but nothing enforces it — a client can relay a binary
frame to another client. On the receiver, `link_of` (port_udp.cpp)
returns `bytes[0]` as the link index, and `syNetPeerCheckForwardPacket`
runs **before** link attribution in `syNetPeerReceiveRemoteInput`.

### SEC-2 (P0): FORWARD packets are not restricted to the host link —
input injection for any player
`syNetPeerHandleForwardPacket` (netpeer.c:1513) validates magic, version,
type, session_id and the FNV checksum — all publicly computable and the
session_id travels in every packet. It never checks:

- that the receiver is a client (`sSYNetPeerBootstrapIsHost == FALSE`), or
- that the datagram arrived on the host's link (the `link` from
  `port_udp_link_recv` is ignored entirely).

A malicious room participant can craft a FORWARD datagram claiming
`src_player=X` with an inner INPUT packet for X and inject inputs for
any player — on the host AND on clients (via SEC-1 relayed delivery or
direct UDP to the host where the endpoint matches the sender's link).
Result: targeted desync / match manipulation.

Fix sketch: in the handler require `!sSYNetPeerBootstrapIsHost` and that
`link` is the host's link; in the Worker, reject binary frames where
`bytes[0] !== 0` and the sender's slot !== 0.

### SEC-3 (P1): uplink sockets are unauthenticated — anyone with the room
code can attach to any slot
`fetch` handler: `up=1&slot=N` only requires `socketForSlot(slot)` to
exist. There is no token binding the uplink to the owner's control
socket. An attacker who knows the 6-char code can open an uplink for the
host's slot and:

- send text `{type:"endpoint"}` → overwrites the victim's endpoint in
  `broadcastRoster` → other players hole-punch/relay toward an
  attacker-controlled address;
- send binary frames routed with `from_slot = victim's slot` → injects
  game traffic as that player.

Fix: issue a random `up_secret` in `welcome`, require it on uplink
connect, and cap one uplink per slot.

### SEC-4 (P1): room code is the only secret and there is no rate limiting
`room` is a 6-char code (32^6 ≈ 1e9) used as the DO name and as the sole
authorization for joining, posting endpoints, and attaching uplinks.
No per-IP/per-socket rate limits on text or binary traffic — a member
(or code guesser) can spam `endpoint`/`relay`/binary at line rate.
`endpoint` also lets a member make peers send UDP to an arbitrary
ip:port (small amplification).

### SEC-5 (P2): session_id == rng_seed
`sSYNetPeerSessionID = metadata->rng_seed` (netpeer.c:1991). The "session"
is just the match seed chosen by the host — no replay protection across
rooms and it's fully predictable once you see one packet. Checksums are
FNV-1a, not MACs: fine for corruption detection, not for authentication.
Any auth story (SEC-2 fix) can't rely on them.

### REL-1 (P1): `link_of` for framed relay traffic trusts `bytes[0]`
`port_udp.cpp`: `return bytes[0] < PORT_UDP_LINK_MAX ? bytes[0] : -1`.
The slot byte is whatever the Worker stamped — combined with SEC-1 a
sender slot ≠ configured link can route frames onto an arbitrary link
index on the receiver. At minimum verify `sLinks[bytes[0]].active &&
sLinks[bytes[0]].relay`.

### REL-2 (P2): bootstrap sleeps on the game thread
`syNetPeerRunBootstrap` busy-loops with `port_udp_sleep_us` between
retransmits (netpeer.c:810-852). Fine in the headless bootstrap path,
but if it ever runs on the UI thread it freezes the window. Confirm it's
only used by the test harness.

### OPS-1 (P1): the whole multiplayer diff is uncommitted, and `decomp`
is a detached HEAD
`git -C decomp status`: `HEAD (no branch)` with 7 modified files
(+656/-86). ~1700 lines of new netplay logic in the outer tree, none of
it committed, no worktree (CLAUDE.md mandates one per session). One
`git checkout` anywhere and the netpeer rewrite is gone.

### OPS-2 (P2): `port_netrules_set` signature change is uncommitted API
`const int *tap_jump_disabled[4]` — silently NULL-tolerant (`nullptr` →
zeros). Callers in netlobby pass `tjs` built from untrusted `atoi` on
SSBL fields — fine, but any negative/garbage value just becomes "off/on"
with no validation.

## Things that checked out OK

- Packet framing validates magic/version/session/player/frame_count and
  an FNV checksum before touching state (netpeer.c:1364-1396).
- Input history indexing is `tick % LEN` with `player` bounds-checked by
  `syNetInputCheckPlayer` — no OOB from a forged tick.
- STARTM roster parsing is bounds-checked (`args.size() >= 12+4n`,
  `n` in 2..4, `my_game` found before use).
- Worker v2 slot assignment is atomic (no await between `freeSlot` and
  `acceptWebSocket`); host disconnect kills the room; uplinks can't
  kill the room on close; text/binary caps exist (2KB/1KB).
- Rollback snapshots exclude reloc file bytes correctly and disable
  themselves on range-table overflow instead of silently missing state.
- Netsync hashing is field-structured (floats by bit pattern, no
  pointers, draw-order-independent) — the §6 concerns don't apply.

## Remediation (2026-10-13, same day)

All P0/P1 findings were fixed in `netplay: harden the relay path
end-to-end` (outer `5d3b0e8`, decomp `3fef14e` on
`netplay-v2-multiplayer`):

- **SEC-1/SEC-2**: the Worker now enforces the star it documented —
  clients may only address slot 0 and the host only client slots, and a
  relayed hop additionally requires the link to be marked relayed
  (`relay` on the sender for client→host, on the target for
  host→client). The relay `from_slot` is stamped from the authenticated
  socket, never from frame bytes. Client side, FORWARD datagrams are
  only accepted by non-host participants, only on the link to the host,
  and only when the claimed inner sender is a forward-only remote
  (`link == -1`); `link_of` validates the relay framing byte against an
  active relay-backed link.
- **SEC-3**: uplinks authenticate with a per-connection `upkey` issued
  in the v2 `welcome` (`&key=` on the upgrade). Keys are bound to the
  owner socket — a stolen or replayed key for another slot fails, a
  control socket's uplinks die with it, and a reconnect replaces the
  stale uplink.
- **SEC-4**: per-socket token windows (20 text/s, 600 binary/s, close
  4029), ≤10 endpoint updates per connection, ≤12 join/uplink attempts
  per room per 10 s. node-lobby also garbage-collects empty rooms and
  rejects joins to nonexistent rooms without minting a Room.
- **SEC-5**: `session_id` is a fresh lobby-generated u32 carried in
  `STARTM`/`START`, decoupled from the broadcast `rng_seed`; 0 keeps the
  legacy seed-derived value for interop with pre-session builds.
- **OPS-1**: decomp work lives on branch `netplay-v2-multiplayer`
  (49f339d + 3fef14e) and the outer tree is committed; nothing netplay
  is left uncommitted.

Verified: `node services/netplay-lobby/tests/test-lobby-security.js` — 15/15 checks (uplink
key accept/reject paths, client→client drop, relay-flag gating both
directions, from_slot stamping, join/text flood limits). The C++ builds
clean (MSVC Release); the final link needs BattleShip.exe instances to
be closed. Same hardening mirrored in `services/netplay-lobby/src/node-lobby.js` so local
tests match production.

### Residual limitations

- The room code remains the only room secret (now rate-limited). A
  Cloudflare-level per-IP limit (e.g. Workers rate limiting API or
  Turnstile on the join path) is the remaining hardening if abuse
  appears.
- v1 rooms keep the legacy wire format; they get the session id only
  when both ends run a build that sends the trailing field.
