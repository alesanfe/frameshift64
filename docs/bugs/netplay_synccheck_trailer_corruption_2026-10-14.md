# Netplay: sync-check trailer rode outside the packet checksum — corruption produced false DESYNCs

**Status:** FIXED (decomp `netpeer.c`, wire protocol v4). Found by the new
adversarial network simulator (`SSB64_NETPLAY_SIM_CORRUPT_PCT`), testplan
layer 1.

## Symptom

`netplay_localtest.ps1` with corruption enabled (60 ms latency, 20 ms jitter,
3 % loss, 5 % dup, 10 % reorder, **1 % corrupt**) produced:

```
SSB64 Rollback: DESYNC detected at tick=620 local=0C4C9124 remote=6E52159C
```

— every column diverged (full, rng, battle, fighters, items, weapons, stage,
objman, input, joints, camera, vars) — yet the two peers' confirmed-state
traces compared **identical** for all 685 ticks. A desync where both peers
agree on every hash column they logged is self-contradictory: the remote's
reported `check_hash` for tick 620 (6E52159C) did not match the hash the
client itself had recorded in its own trace.

## Root cause

The INPUT datagram layout is

```
[magic][version][remote_advantage][session][ack_tick][seq]
[player][frame_count][src][dst][frames x16][check_tick][check_hash][checksum]
```

but `syNetPeerChecksumPacket()` folded only the fixed header fields and the
frame array. Three serialized fields rode **unauthenticated**:

- `remote_advantage` (2 bytes) — telemetry, harmless when corrupted,
- `check_tick` (4 bytes),
- `check_hash` (4 bytes) — the value the desync oracle compares.

The corruption simulator flips one byte in a delivered datagram. A flip
inside the 8-byte sync-check trailer (~6.5 % of the ~124-byte packet) left
the checksum valid, so `syNetPeerHandlePacket` accepted the packet and fed a
garbage `check_hash` into `syNetRollbackCheckRemoteConfirmedHash` — which is
tick-aligned (`ConfirmedTicks[ring] == tick`) and correctly fired DESYNC on a
hash that never existed on the sender.

The detector did its job; the transport lied to it.

## Fix

- `syNetPeerBuildPacket` / `syNetPeerHandlePacket` now compute the checksum
  with `syNetPeerChecksumBytes(buffer, SYNETPEER_PACKET_BYTES - 4)` — the
  whole wire image except the trailing checksum word — matching how FORWARD
  and control packets were already authenticated. `syNetPeerChecksumPacket`
  (field-wise variant that could not see the trailer) is deleted;
  `syNetPeerChecksumAccumulate*` stay for the input-history checksum.
- `SYNETPEER_VERSION` bumped 3 -> 4. The checksum algorithm changed, so a
  mixed-version pair would drop every packet anyway; the version gate makes
  that explicit instead of a silent stall.
- The in-engine fuzz builder (`SSB64_NETPLAY_PACKETFUZZ`) signs its crafted
  packets the same way, so tier-4/5 mutant vectors are unaffected.

## Verification

- Same adversarial profile after the fix: `IN SYNC for 661 ticks`,
  `desync=0` on both peers, rollbacks still exercised
  (`rollbacks=7 resim_ticks=22 max_rollback=4`).
- Clean baseline unchanged: `IN SYNC for 1501 ticks`.
- Mutant vectors still detected (`accept_bad_session`, `no_forward_guard`
  -> DESYNC at line 2).

## Audit hooks

- Any *supplemental* field appended to a wire datagram must land inside the
  checksummed span. The field-wise checksum over a subset of fields made this
  easy to miss — prefer byte-wise checksums over the serialized image.
- "All columns differ but the pairwise trace agrees" = the compared value
  was corrupted in flight, not a diverged simulation. Check the transport's
  authentication coverage before hunting game-state nondeterminism.
