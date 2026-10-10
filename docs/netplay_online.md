# Online Play (Room Codes + Rollback)

## Playing

1. Both players open the **Esc** menu → **Online** → **Netplay**.
2. Both players enter the same **Lobby server** address (`host:port`) and choose a fighter.
3. **Hosting.** One player:
   - picks the stage, stocks, items and input delay;
   - presses **Host**;
   - shares the 6-character **room code** (the **Copy** button puts it on the clipboard).
4. **Joining.** The other player types the code and presses **Join**.
5. **Connecting.** The status line shows **Connected (direct)**, or **Connected (via relay)** when the routers won't allow a direct path. It also shows the opponent's fighter and the ping.
6. **Starting.** The host presses **Start match**. Both games jump straight into the VS battle.
7. **Rematch.** After the match, the room stays open; the host can press **Start match** again.

Online matches use rollback netcode. Your input applies after a short **input delay** (2 frames by default). The opponent's input is predicted, and the game silently rewinds and replays (up to 6 frames) when a prediction was wrong. Raise the input delay on high-ping connections for fewer visible corrections.

## Running the lobby server

The server is `server/lobby-server.js`. It is a single file using Node's built-in `dgram` module, with no `npm install`.

```bash
node server/lobby-server.js 7000
```

- **Where to run it.** It must be reachable by both players on **UDP 7000** (or the port you pass). A small VPS, or a home PC with UDP 7000 forwarded, both work.
- **Players don't need port forwarding.** The server introduces the two games to each other (UDP hole punching). If that fails, for example behind a symmetric or carrier-grade NAT, the server relays the match traffic.
- **The relay costs latency.** The relay adds the server's distance to the ping, so host the server close to the players.
- **What the server keeps.** It holds no accounts or match state. Rooms expire after 10 idle minutes.

## How it works

| Piece | File |
|---|---|
| Rendezvous server (room codes, introductions, relay) | `server/lobby-server.js` |
| Lobby state machine (create/join, hole punch, picks, START/GO) | `port/net/netlobby.cpp` |
| UDP endpoint (Winsock/BSD, relay framing, network simulator) | `port/net/port_udp.cpp` |
| Match transport, pacing, time sync, desync check | `decomp/src/sys/netpeer.c` |
| Input delay, prediction, misprediction detection | `decomp/src/sys/netinput.c` |
| Snapshots, resimulation, shadow draws | `decomp/src/sys/netrollback.c`, `docs/rollback_design.md` |

**Connection sequence:**
1. The game binds one UDP socket and talks to the server from it, so the server sees each player's public (NAT-mapped) address.
2. After **PEER**, both games send hellos to the other's public and LAN addresses.
3. The first reply wins. After 3 seconds without one, they switch to the relay.
4. The host sends **START** with the stage, rules, seed, delay and fighters, and the client answers **GO**.
5. Both call `syNetPeerStartOnline`, and the in-battle start barrier aligns tick 0.

**During the match:**
- Every input packet carries the sender's frame advantage, used for time sync.
- It also carries the full-state hash of a recently confirmed tick. A mismatch is logged as `SSB64 Rollback: DESYNC detected` in `ssb64.log`.

## Testing

```powershell
# Two instances over localhost, env-configured (no lobby); fuzzed inputs, optional bad network.
.\scripts\netplay_localtest.ps1 -Seconds 60 -LatencyMs 60 -JitterMs 20 -LossPct 5

# Full flow: lobby server + host + client joining by code, then a match.
.\scripts\netplay_lobbytest.ps1 -Seconds 60 -LatencyMs 50 -LossPct 3
.\scripts\netplay_lobbytest.ps1 -ForceRelay

# Rollback determinism without networking (7-frame rollback every tick).
.\scripts\rollback_synctest.ps1 -Stage 5 -Fighters "9,10,11,0" -Distance 7
```

Each script compares both peers' confirmed-state traces tick by tick. Exit code 0 means in sync.

## Limitations

- **Players.** Two players only. Slots 0/1 are human, and there are no CPUs.
- **Lockstep moments.** The countdown, announcer and sudden-death intros run in lockstep: they wait for the opponent's confirmed input instead of predicting. See "Not rollback-able yet" in `docs/rollback_design.md`.
- **Rematches.** The fighter and rules carry over; the host changes them by leaving and hosting again.
- **Audio.** Sounds started by a frame that later gets corrected still play once; rewound frames are silent.
