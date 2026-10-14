# Session lifecycle

## State machine

    Disconnected
      └→ Connecting        lobby WS open, room joined/uplinked
      └→ Lobby             slot assigned, upkey held (lobby v2)
      └→ Negotiating       MATCH_CONFIG + READY exchange (host → peers)
      └→ Synchronizing     START broadcast + battle barrier
      └→ Running           INPUT packets flow, rollback active
      └→ Recovering        remote silent < REMOTE_DROP_MS, prediction holds
      └→ Closed            teardown, sockets released

## Per-state valid traffic

| State | Valid packets |
|-------|---------------|
| Negotiating | MATCH_CONFIG (host), READY (peer) |
| Synchronizing | START, BATTLE_READY, BATTLE_START |
| Running / Recovering | INPUT, FORWARD (host-relayed) |

Packets arriving in a state where they are not valid are dropped; the
barrier repeats (`SYNETPEER_BATTLE_START_REPEAT_FRAMES`) make the
Synchronizing exchange loss-tolerant.

## Rollback loop (Running)

- Each peer simulates ahead at most `SYNETPEER_MAX_PREDICTION` = 6 ticks
  past the newest contiguously confirmed remote input.
- Remote silence past `SYNETPEER_REMOTE_DROP_MS` = 10 s stops pacing:
  that fighter freezes on last input; the session continues (Recovering).
- Timesync: `remote_advantage` (s16 in each INPUT header) — the peer ahead
  skips one frame at a time, at most every `SYNETPEER_TIMESYNC_INTERVAL` =
  10 frames, past `SYNETPEER_TIMESYNC_THRESHOLD` = 2.
- Confirmed-state hashes piggyback on every INPUT packet (`check_tick`/
  `check_hash`); a mismatch is a desync report, not a crash.

## Spectating (lobby v2)

Spectators occupy relay-only slots 4..7 (`MAX_SPECS = 4`), never send
binary game traffic, and receive the host-relayed confirmed input stream.
`sSYNetPeerIsSpectator` suppresses input publishing entirely.

## Teardown

`Closed` releases links back to the lobby owner (the lobby owns the UDP
endpoint when `sSYNetPeerIsSocketExternal`), saves replays, and emits the
final session counters (sent/received/dropped/gaps/duplicates/ooo).
