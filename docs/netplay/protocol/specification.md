# Netplay wire protocol — normative specification

Status: **normative**. `components/decomp/src/sys/netpeer.c` is the reference
implementation; where this document and the code disagree, the code is
correct and this document must be fixed in the same PR.

Transport: UDP datagrams, one packet per datagram. Lobby signaling is a
separate protocol over WebSocket (`lifecycle.md`, `security-model.md`).

## Common framing

All multi-byte integers are written via `syNetPeerWriteU16`/`WriteU32`
(big-endian on the wire) and read via the matching `ReadU*`. Implementations
must never memcpy a struct onto the wire.

Every packet family shares a fixed-size wire layout — peers are identified
by *size*, not by an always-present type field:

| Family | Size (bytes) | Discriminator |
|--------|--------------|---------------|
| CONTROL (READY/START/BATTLE_READY/BATTLE_START) | `SYNETPEER_CONTROL_PACKET_BYTES` = 20 | `type` field |
| BOOTSTRAP (MATCH_CONFIG) | `SYNETPEER_BOOTSTRAP_PACKET_BYTES` = 96 + `SYNETPEER_METADATA_BYTES` | `type` field |
| INPUT | `SYNETPEER_PACKET_BYTES` = 172 | implied by size |
| FORWARD | `SYNETPEER_FORWARD_PACKET_BYTES` = 189 | `type` field |

A datagram whose size matches none of these is dropped silently and counted
(`sSYNetPeerPacketsDropped`).

## Common header

    offset  size  field
    0       4     magic          = 0x53534E50 ("SSNP")
    4       2     version        = 3 (SMASH_NETPLAY_PROTOCOL_VERSION)
    6       2     type-or-advantage — packet type (control/bootstrap/forward)
                                   or s16 remote_advantage (INPUT)
    8       4     session_id     — lobby-generated for lobby matches
    ...     ...   family-specific payload
    last-4  4     checksum       — FNV-1a over semantic fields

`magic`/`version` mismatch → drop. `session_id` mismatch → drop (unless
the `skip_session_check` test mutant is armed). `checksum` mismatch →
drop + count.

## INPUT packet (type-implied, size 172)

    +24-byte header (magic,version,advantage,session,ack,seq,
                     local_player,frame_count,src_player,dst_player)
    +16 × SYNetPeerPacketFrame (8 bytes each, zero-padded):
        tick u32 | buttons u16 | stick_x s8 | stick_y s8
    +8-byte sync check: check_tick u32 | check_hash u32
        (0xFFFFFFFF/0 when no confirmed tick exists yet)
    +4-byte FNV-1a checksum

`frame_count` ≤ 16 is enforced; frames beyond `frame_count` are ignored.
`remote_advantage` (s16, clamped ±32767) drives timesync pacing. The sync
check lets the receiver detect divergence at the sender's latest
*confirmed* tick — see `lifecycle.md` §Rollback.

## CONTROL packets (size 20)

    magic u32 | version u16 | type u16 | session_id u32 | value u32 | checksum u32

Used for the pre-match barrier exchange. `type` carries the meaning;
`value` is type-specific (e.g. peer-ready flag).

## BOOTSTRAP packet (MATCH_CONFIG)

    magic u32 | version u16 | type u16 | session_id u32 |
    SYNetInputReplayMetadata (80 bytes: stage, stocks, items, seed,
    player_count, per-slot fighter kinds + costumes) | checksum u32

Sent by the host until `READY` is received (bounded retries).

## FORWARD packet (star-topology relay, size 189)

    magic u32 | version u16 | type u16 | session_id u32 |
    src_player u8 | inner INPUT packet | checksum u32

Accepted **only** on the host link, and only for remotes flagged
forward-only. `src_player` is stamped by the relaying host — clients never
trust a self-declared source slot. See `security-model.md`.

## Session manifest (target — TD-008)

Before `BATTLE_START`, both sides exchange and compare:

    { protocolVersion, gameVersion, rulesetHash, simulationConfigHash,
      modApiVersion, simulationMods[], assetRevision,
      characterRegistryHash, stageRegistryHash, buildCompatibilityId }

Result: Compatible / Compatible-with-warnings / Incompatible / Unverified.
Incompatible → structured refusal listing each differing field.
