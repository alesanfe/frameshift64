# Online Play (Room Codes + Rollback)

## Playing

1. Both players open the **Esc** menu → **Online** → **Netplay**.
2. Choose a fighter. The lobby server is built in (`wss://smash-netplay.jumpcircuit.workers.dev`); it can be changed under **Advanced**.
3. **Hosting.** One player:
   - picks the stage, stocks, items and input delay;
   - presses **Host**;
   - shares the 6-character **room code** (the **Copy** button puts it on the clipboard).
4. **Joining.** The other player types the code and presses **Join**.
5. **Connecting.** The status line shows **Connected (direct)**, or **Connected (via relay)** when the routers won't allow a direct path. It also shows the opponent's fighter and the ping.
6. **Starting.** The host presses **Start match**. Both games jump straight into the VS battle.
7. **Rematch.** After the match, the room stays open; the host can press **Start match** again.

Online matches use rollback netcode. Your input applies after a short **input delay** (2 frames by default). The opponent's input is predicted, and the game silently rewinds and replays (up to 6 frames) when a prediction was wrong. Raise the input delay on high-ping connections for fewer visible corrections.

## The lobby server (Cloudflare)

The server is `services/netplay-lobby`: a Cloudflare Worker plus one Durable Object per room. It follows the same pattern as the Jump Circuit relay.

**What it does:**
- **Room codes:** it hands them out.
- **Endpoint exchange:** each game learns its public UDP address from Cloudflare's STUN server (`stun.cloudflare.com:3478`). The Worker passes it, plus the LAN address, to the other player.
- **Direct connection:** the games then hole-punch a direct UDP connection. Gameplay doesn't touch Cloudflare.
- **Relay fallback:** if punching fails (symmetric or carrier-grade NAT), the Worker relays game packets over the same WebSocket.
- **No player setup:** no accounts, no secrets, no port forwarding.

To deploy from `services/netplay-lobby`:

```bash
npm install
npx wrangler login
npm run deploy:dry-run
npm run deploy
```

| Command | Purpose |
|---|---|
| `npm run dev` | Local Worker on `ws://127.0.0.1:8799`. `scripts/testing/netplay_lobbytest.ps1` starts it automatically. |
| `GET /health` | Returns `{"ok":true,"service":"smash-netplay"}`. |

Release builds default to the deployed Worker. Override it at configure time with `-DSSB64_NETPLAY_SERVER="wss://..."`, or set it in the menu.

## How it works

| Piece | File |
|---|---|
| Lobby server (room codes, endpoint exchange, relay) | `services/netplay-lobby/src/infrastructure/cloudflare/worker.js` |
| Lobby state machine (STUN, create/join, hole punch, picks, START/GO) | `src/networking/netplay/session/netlobby.cpp` |
| WebSocket client (WinHTTP, wss://) | `src/networking/netplay/transport/websocket_transport_win32.cpp` |
| UDP endpoint (Winsock/BSD, relay hooks, network simulator) | `src/networking/netplay/transport/udp_transport.cpp` |
| Match transport, pacing, time sync, desync check | `components/decomp/src/sys/netpeer.c` |
| Input delay, prediction, misprediction detection | `components/decomp/src/sys/netinput.c` |
| Snapshots, resimulation, shadow draws | `components/decomp/src/sys/netrollback.c`, `docs/netplay/rollback_design.md` |

**Connection sequence:**
1. The game binds one UDP socket and sends a STUN request to Cloudflare from it, which returns the socket's public (NAT-mapped) address.
2. It connects to `wss://.../ws?room=CODE&role=host|join` and posts `{type:"endpoint", public, local}`.
3. After the Worker sends `peer`, both games send hellos to the other's public and LAN addresses.
4. The first reply wins. After 3 seconds without one, they send `{type:"relay"}` and game packets go over the WebSocket as binary frames.
5. The host sends **START** with the stage, rules, seed, delay and fighters, and the client answers **GO**.
6. Both call `syNetPeerStartOnline`, and the in-battle start barrier aligns tick 0.

**During the match:**
- Every input packet carries the sender's frame advantage, used for time sync.
- It also carries the full-state hash of a recently confirmed tick. A mismatch is logged as `SSB64 Rollback: DESYNC detected` in `ssb64.log`.

## Testing

```powershell
# Two instances over localhost, env-configured (no lobby); fuzzed inputs, optional bad network.
.\scripts\netplay_localtest.ps1 -Seconds 60 -LatencyMs 60 -JitterMs 20 -LossPct 5

# Full flow: host + client joining by code, then a match (local wrangler dev, or the live Worker).
.\scripts\netplay_lobbytest.ps1 -Seconds 60 -LatencyMs 50 -LossPct 3
.\scripts\netplay_lobbytest.ps1 -ForceRelay
.\scripts\netplay_lobbytest.ps1 -Server wss://smash-netplay.jumpcircuit.workers.dev

# Rollback determinism without networking (7-frame rollback every tick).
.\scripts\rollback_synctest.ps1 -Stage 5 -Fighters "9,10,11,0" -Distance 7
```

Each script compares both peers' confirmed-state traces tick by tick. Exit code 0 means in sync.

## Limitations

- **Players.** Two players only. Slots 0/1 are human, and there are no CPUs.
- **Lockstep moments.** The countdown, announcer and sudden-death intros run in lockstep: they wait for the opponent's confirmed input instead of predicting. See "Not rollback-able yet" in `docs/netplay/rollback_design.md`.
- **Rematches.** The fighter and rules carry over; the host changes them by leaving and hosting again.
- **Audio.** Sounds started by a frame that later gets corrected still play once; rewound frames are silent.
- **Platforms.** The lobby's WebSocket client is Windows-only for now (WinHTTP); other platforms can still use env-configured direct netplay.
- **Mid-match spectators.** A spectator joining while a match is in progress receives no state snapshot and cannot reconstruct the current game state — they begin observing at the next match. Deliberate for now: there is no catch-up/sync path for spectators yet (AUD-48).
- **Replay takeover tail.** `SSB64_REPLAY_TAKEOVER` flips one slot to live input at a chosen tick, but the recording itself still ends where it ends: once the replay is exhausted, the slots still on `Saved` input receive zeroed frames and stand idle. Fine for the intended "train from this exact moment" workflow; there is no synthesized continuation for the other players (AUD-47).
