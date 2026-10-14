# Error codes

## Lobby reject reasons (`{type:"error", reason}` + close 4000)

| reason | Meaning | Client action |
|--------|---------|---------------|
| `rate-limited` | Join/uplink rate exceeded for the room | back off, retry later |
| `no-such-room` | Bad code, missing host, or bad upkey | re-check code / rejoin |
| `version-mismatch` | `?v=` differs from the room-pinned version | upgrade/downgrade client |
| `room-full` | All player slots (0..3) or spec slots (4..7) taken | retry or host new room |
| `room-in-use` | Host re-joined a room it already holds | reuse existing connection |

Adding a reason is additive (non-breaking). Removing/renaming one breaks
older clients' error UI — treat like a protocol change.

## P2P layer

The UDP layer has no error channel by design: invalid packets are dropped
and counted. Diagnosable conditions surface through counters and logs:

| Counter | Meaning |
|---------|---------|
| `sSYNetPeerPacketsDropped` | magic/version/session/checksum/dst/count failures |
| `sSYNetPeerSeqGaps` | missing sequence numbers (loss upstream) |
| `sSYNetPeerSeqDuplicates` | re-delivered datagrams |
| `sSYNetPeerSeqOutOfOrder` | reordering |
| `sSYNetPeerLateFrames` | input arrived after its tick was needed |

Desync is reported via confirmed-hash mismatch (`check_tick`/`check_hash`
in every INPUT packet) feeding `syNetReplayWriteDesyncPackage` — not via
an error code.
