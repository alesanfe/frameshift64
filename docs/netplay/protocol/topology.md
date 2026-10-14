# Topology

## Signaling vs data

- **Signaling**: WebSocket to the lobby Worker (Cloudflare Durable Object)
  or the local `node-lobby.mjs` reference implementation.
- **Data**: UDP between players. Two shapes:

### Direct mesh (2 players)

Host and joiner punch UDP directly via endpoints announced over the
lobby. FORWARD is unused.

### Star relay (3-4 players, and any unreachable pair)

The host owns the relay. Clients send INPUT to the host; the host wraps it
in FORWARD and redistributes to the other clients. Clients never see each
other's endpoints on relayed paths — the lobby marks such links
`relay: true` and the client accepts FORWARD only on the host link, only
for remotes flagged forward-only.

### Spectators

Relay-only listeners on slots 4..7. They never punch, never send binary
traffic; the host pushes the confirmed input stream to them.

## Fallbacks

- Direct UDP fails or is impossible → relay via the host (lobby supplies
  relay endpoint frames).
- `wrangler dev` local testing has a known dev-proxy frame-drop issue —
  `node-lobby.mjs` is the reference for local protocol testing
  (`docs/bugs/README.md` → `netplay_relay_wrangler_dev_drop`).
