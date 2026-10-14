# netplay_relay_production_blob_drop — 2026-10-14

**Status**: FIXED (worker)
**Component**: `services/netplay-lobby/src/index.js` (Durable Object relay)

## Symptom

After deploying the v2 worker to `wss://smash-netplay.alesanfe.workers.dev`,
forced-relay matches never synchronized: both peers sent binary `SSBL HELLO`
frames (`relay send ... ptype=17740`) but no `relay recv` ever logged, and the
lobby fell back to `unreachable (punch + relay failed)`. Identical code passed
2–4 player relay tests locally against `services/netplay-lobby/src/node-lobby.js` and under
`wrangler dev`'s text path.

## Root cause

Production `workerd` delivers WebSocket **binary** frames to a Durable Object's
`message` event as `Blob` objects — not `ArrayBuffer`. `wrangler dev` and the
local Node lobby deliver `ArrayBuffer`/`Buffer` instead.

The DO binary path did:

```js
const bytes = new Uint8Array(message);   // Blob -> empty view
if (bytes.length < 2) return;            // silently dropped every frame
```

`new Uint8Array(blob)` produces a zero-length view (a Blob is not iterable as
bytes), so every binary relay frame was silently dropped while all JSON text
frames (welcome, peers, endpoint, relay, peer_relay) flowed normally — masking
the failure completely.

## Diagnosis path

`wrangler tail` + a standalone `ws` client (`wstest/prodtest.js`) showed the DO
*did* receive the frames — `event.data` constructor was `Blob` with
`byteLength === undefined`. That ruled out the client's send path, the relay
gate (`!src.relay`), and the uplink key check before identifying the payload
type mismatch.

## Fix

Extracted binary routing into `onBinary(ws, arrayBuffer)` and normalize at the
top of `onMessage`: Blob payloads are awaited through a per-socket promise
chain (`info.bin`) so frames keep arrival order while `message.arrayBuffer()`
resolves asynchronously. `ArrayBuffer` payloads (wrangler dev, hypothetically
other runtimes) go straight through.

## Verification

| Scenario | Result |
|----------|--------|
| prodtest.js uplink + control binary, both directions | routed, `from=` slot correct |
| 2 players, forced relay, production worker | IN SYNC, 2161 ticks |
| 4 players, forced relay, production worker | IN SYNC, 2078 ticks (real rollbacks under live CF jitter) |

## Lessons

- `wrangler dev` does not faithfully emulate the production ws message types;
  assume `Blob` for binary in workerd and normalize at the boundary.
- A binary-drop failure that preserves all text traffic looks like a
  "negotiation" bug — instrument `onMessage` first, not the gates.
