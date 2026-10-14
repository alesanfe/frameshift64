# Forced-relay netplay stalls under `wrangler dev` (workerd ws proxy drops frames)

**Status:** RESOLVED (test-environment issue, not a game bug). Verified 2026-10-13.

## Symptom

Multi-player (and 1v1) matches forced through the WebSocket relay
(`SSB64_LOBBY_FORCE_RELAY=1`, `scripts/testing/netplay_lobbytest_multi.ps1 -ForceRelay`)
against a local `wrangler dev` Durable Object stalled a few seconds in:

- Signaling worked: `endpoint` exchange, `relay` negotiation, `BATTLE_READY`,
  `BATTLE_START` all flowed.
- Then all binary game traffic silently stopped reaching peers after ~30–45
  frames. WinHTTP `WinHttpWebSocketSend` returned `ERROR_SUCCESS` for every
  frame; the Durable Object's per-socket counters simply stopped incrementing.
- Peers eventually logged `player N unreachable (punch + relay failed)` /
  watchdog hangs; confirmed-state traces showed 0–1 ticks.

## Dead ends ruled out

- **WinHTTP send/receive concurrency**: a standalone repro (`wsblock`/`wsflood`
  harnesses) showed sends complete in 0–16 ms even with a pending blocking
  receive on the same handle — no inherent block.
- **DO hibernation losing socket state**: rewrote the Durable Object from the
  hibernatable API (`acceptWebSocket`) to the manual API (`accept()` +
  `addEventListener`). Dev-mode behavior unchanged.
- **Client framing/routing**: the `[to_slot][payload]` / `[from_slot][payload]`
  v2 framing, star-topology forwarding, and uplink sockets were all correct.

## Root cause

`wrangler dev` proxies WebSocket connections through a local Node<->workerd
bridge that drops binary frames under sustained load and intermittently resets
the DO (observed: binary counters restarting mid-run, "Network connection lost"
errors in wrangler output). The exact same client build and protocol work
flawlessly against `services/netplay-lobby/src/node-lobby.js` — a standalone Node implementation of
the same wire protocol — with zero code changes in between.

**Audit hook:** when ws traffic "succeeds" at the sender but never arrives, and
only under `wrangler dev`, suspect the dev proxy before the protocol. Use
`services/netplay-lobby/src/node-lobby.js` (or a deployed Worker) for local relay testing.

## What changed anyway

- `services/netplay-lobby/src/index.js`: Durable Object uses the non-hibernating
  WebSocket API (simpler state model, no hibernation edge cases; required for
  in-memory room maps) — kept, it's the safer production shape.
- `services/netplay-lobby/src/node-lobby.js` (new): local lobby for tests; the test script
  auto-prefers it over wrangler when `services/netplay-lobby/node_modules/ws`
  exists.
- `port_ws.cpp`: send-only **uplink** ws connection (`/ws?...&up=1&slot=N`) for
  all outbound binary game frames. Isolating directions sidesteps the dev-proxy
  issue class entirely and is harmless in production (wrangler still drops
  uplink frames in dev mode; real Cloudflare + node-lobby do not).
- `SSB64_NETPLAY_DEBUG_STAGES=1` env gates the verbose per-frame relay
  send/recv logs and `SSB64 DBG:` stage markers in `netpeer.c`.

## Verified

`scripts/testing/netplay_lobbytest_multi.ps1` against node-lobby:

| Scenario | Result |
|---|---|
| 2 players, direct UDP | IN SYNC, 1598 ticks |
| 3 players, direct UDP | IN SYNC, 2521 ticks |
| 4 players, direct UDP | IN SYNC, 2283 ticks |
| 3 players, forced relay | IN SYNC, 2161 ticks |
| 4 players, forced relay | IN SYNC, 2461 ticks |
| 3 players, relay + 80 ms + 25 ms jitter + 3% loss | IN SYNC, 398 ticks |
