# Message catalog

IDs are assigned once and never reused. Packet size is the primary
discriminator for INPUT; the `type` field discriminates within the
fixed-size families.

| ID | Name | Family | Direction | Valid states | Notes |
|----|------|--------|-----------|--------------|-------|
| 0 | INPUT | implied by size (172 B) | peer→peer, host→clients via FORWARD | Running, Recovering | frames + timesync + confirmed-hash |
| 1 | MATCH_CONFIG | BOOTSTRAP (96+80 B) | host→peer | Negotiating | carries full match metadata |
| 2 | READY | CONTROL (20 B) | peer→host | Negotiating | peer consumed config |
| 3 | START | CONTROL | host→peer | Negotiating→Synchronizing | leave lobby phase |
| 4 | BATTLE_READY | CONTROL | both | Synchronizing | in-battle barrier |
| 5 | BATTLE_START | CONTROL | host→all, repeated 30× | Synchronizing→Running | barrier release |
| 6 | FORWARD | FORWARD (189 B) | client→host→clients | Running | star relay; src stamped by host |

New message IDs must be chosen from never-used values and documented here
in the same PR that adds the code.

## Validation applied at parse (INPUT)

    magic == SSNP          version == 3          size == 172
    session_id == ours     src fields consistent (player == src_player ==
                           packet_local_player)
    dst == us (skipped for FORWARD)              frame_count <= 16
    FNV-1a checksum match                        seq tracked (gap/dup/ooo)

Each failed check → `sSYNetPeerPacketsDropped++`, no state change.

## Lobby messages (WebSocket, JSON text / binary)

| Direction | Message | Notes |
|-----------|---------|-------|
| C→S | `?role=host|join|spec|queue&v=1|2&room=&slot=&key=` | URL params on upgrade; `queue` (v2 only) is matchmaking — the room assigns `host` when slot 0 is free, `join` otherwise, and the welcome reports the resolved role. Quick-play clients walk the well-known `QUEUE2..9` codes; the client picks its start room by rating band (proven ≥1500 → QUEUE8). |
| S→C | `{type:"welcome", upkey, ...}` | upkey is the slot-attach secret |
| S→C | `{type:"error", reason}` then close 4000 | see error-codes.md |
| S→C/C→S | `{type:"peer", ...}` text | join/part/topology |
| C→S | binary endpoint announce | hole-punch offer |
| S→C | binary endpoint + relay flag | when direct path is impossible |
| both | binary game frames | v2: slot-indexed routing |

Room codes: `^[A-HJ-NP-Z2-9]{6}$` (6 chars, ambiguous glyphs excluded).
Text messages ≤ 2048 B; binary ≤ 1024 B; rate limits 20 text/s and
600 binary/s per socket, plus a bounded join/uplink rate per room.

## HTTP endpoints (no WebSocket upgrade)

| Method | Path | Purpose |
|--------|------|---------|
| GET | `/health` | uptime probe `{"ok":true,...}` |
| GET | `/metrics` | Prometheus exposition (node-lobby only) |
| GET | `/leaderboard` | `{"entries":[{name,rating,wins,losses,updated,disputes}]}` top-100 by rating |
| POST | `/submit` | upsert `{"name",key,rating,wins,losses}` — `key` is the client's persisted claim token (≥16 hex); `400 name-taken` on key mismatch, `400 bad-*` on out-of-range fields |
| POST | `/result` | corroborated match report `{"name",opp_name,key,match_id,won:1|0|-1,duration}` — see below |

Leaderboard state lives on the dedicated `leaderboard-global` DO (worker)
or `leaderboard.json` (node-lobby).

### Verified results (`POST /result`)

`/submit` is the self-reported claim path — it binds a display name to a
claim key and sets the entry's self-reported rating. `/result` is the
**verified** path that moves ratings:

1. Both clients of a 1v1 POST the same `match_id` (a deterministic hash
   of `code|session|sorted-names|duration` each side derives locally).
   `won` is from the reporter's perspective (1 win, 0 loss, -1 draw).
2. The first report is stored as `{"kind":"pending"}`; pending reports
   expire after 10 minutes.
3. A second report for the same `match_id` from a *different* claim key
   is verified if it mirrors the first (`A:"I beat B"` ⇔ `B:"I lost to
   A"`, or draw/draw). On verification the server applies its own Elo
   update (K=32, floor 100) to both entries and returns
   `{"kind":"verified",winner,loser}`.
4. A contradictory second report returns `{"kind":"mismatch"}` and
   increments `disputes` on both names — the anti-grief signal. Either
   way the `match_id` is settled: re-reporting it gets `400
   already-settled`.

A lone client cannot award itself wins — moving a rating takes two
distinct claim keys agreeing. Two colluding clients can still farm wins;
that is the ceiling of account-less verification.
