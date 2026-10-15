# Netplay: residential stall + spectator catch-up desync (four root causes)

**Status:** RESOLVED
**Date:** 2026-10-15
**Area:** `decomp/src/sys/netinput.c`, `decomp/src/sys/netpeer.c`, `src/netplay/protocol/netpacket.c`, `tests/unit/netplay/protocol/netpacket_test.c`

## Symptoms

Under the `residential` impairment profile (45 ms latency, 12 ms jitter):

- 2-player match deadlocked permanently at ~tick 541-548.
- Mid-match spectator never advanced past tick 2.
- On a clean network the spectator caught up but desynced deterministically
  at tick 1964 (Fighters/Input/Joints/Vars columns diverged permanently).

The cluster looked like transport failure (`recv` froze, `dropped` grew) but
decomposed into four independent root causes.

## Root cause 1 — INPUT packets misclassified as FORWARD

`nbNetPacketIsForward()` in `netpacket.c` tested only the packet-type field.
In the INPUT wire layout, the `remote_advantage` field sits at the same
offset the FORWARD type occupies — so any INPUT packet with
`remote_advantage == PACKET_FORWARD (6)` was parsed as a FORWARD envelope.
Under sustained latency the advantage stays >= 6, so peers dropped each
other's input packets as malformed forwards. `src` then decoded as the high
byte of `ack_tick` (0), making the drops look like spoofed host forwards.

**Fix:** `nbNetPacketIsForward` now requires the inner payload to begin with
the SSNP magic — FORWARDs only ever wrap complete game datagrams. A
collision now needs `ack_tick >= 0x53534Exx` (~16 days of match).
Regression test in `netpacket_test.c` mutates a valid INPUT packet so
`remote_advantage == PACKET_FORWARD` and asserts it is not a forward.

## Root cause 2 — adaptive input delay punches a hole in LocalSchedule

`syNetInputMakeLocalFrame()` scheduled exactly one sample per consume tick at
`tick + sSYNetInputLocalDelay`. When adaptive delay bumped 2 -> 3 mid-match,
the scheduled frontier was at `T+2`; the next call scheduled `T+4`, and
`T+3` was never written. INPUT packets repeat only ticks present in
`LocalSchedule`, so the hole never reached the wire. The peer's
`sSYNetInputRemoteNextUnconfirmed` stopped at the gap while future frames
kept arriving, and pacing (`tick >= confirmed + MAX_PREDICTION`) stalled
forever — symmetrically on both peers.

Diagnostic evidence: packets carrying `[536,551]` arrived continuously while
`unconf=542` and `delay -> 3` had just fired.

**Fix:** `sSYNetInputLocalScheduledThrough[player]` tracks the scheduled
frontier; each call fills every tick in `(frontier, tick + delay]` with the
current sample instead of writing a single point. Delay increases no longer
create gaps, delay decreases need no backfill, and repeated reads of a
stalled tick stay idempotent.

## Root cause 3 — spec remote ring: old retransmit evicts pending newer frame

`RemoteHistory` is a 720-slot ring indexed by `tick % 720` and validates the
embedded tick on read. While a spec catches up, the host's paced history
stream rewinds to the spec's reported progress and resends old ticks. A
retransmitted tick `T` and the still-unconsumed tick `T+720` share a slot;
the resend overwrote the pending frame, and first-write-wins then pinned the
older frame. Every tick in `(progress, progress+window]` that shared a slot
with a resend was destroyed before consumption — the spec at consume ~1244
permanently lost tick 1964 onward.

**Fix:** `syNetInputSetRemoteInput` compares the embedded tick of the stored
frame against the incoming frame on the same slot and skips the store when
the stored frame is newer and still unconsumed. The mispredict-check path is
preserved (the early store-skip does not bypass rollback bookkeeping).

## Root cause 4 — predicted replay records streamed to the spec as final

`syNetPeerServeSpecHistory` streamed replay records up to the host's live
tick. A remote player's record for tick `T` is only final once `T` is below
that player's confirmed edge; above it the record can hold a *predicted*
input that a later rollback rewrites (the recording does get overwritten on
republish — fixed in the same file — but the spec's first-write-wins store
keeps the first copy it received). Under latency, mispredicted ticks exist,
so the spec pinned a wrong input permanently — the deterministic desync.

**Fix:** the streaming ceiling is now
`min(progress + SYNETPEER_SPEC_WINDOW, min_p remote_next_unconfirmed[p])`
over remote players `p`. Live FORWARDs need no gate — they carry the
sender's own scheduled frames, which are final by definition.

## Verification

- `netpacket_test`: PASS (incl. new forward-misclassification regression).
- `lobbytest_multi --players 2 --profile residential`: IN SYNC, 1838 ticks
  (previously deadlocked at 541).
- `spectate_test --profile residential`: IN SYNC across 2 players +
  spectator, 2101 ticks (spec previously stuck at tick 2, then desynced at
  579 before the confirmed-edge gate).
- `spectate_test` clean: IN SYNC, 2161 ticks.

## Diagnostics kept (gated)

- `SSB64_DBG_INPUT_TICKS=lo:hi` — spec store/resolve dump (`netinput.c`).
- `SSB64_NETPLAY_PKTDIAG` — per-400-packet frame-window + unconfirmed-edge
  dump (`netpeer.c`).
- Drop paths log their first ~8 occurrences unconditionally (parse,
  size, fwd-guard, unrouteable) — abnormal events, bounded volume.
