# Netplay & lobby protocol compatibility

Two distinct protocols, two distinct versions:

| Layer | Version field | Where enforced |
|-------|--------------|----------------|
| P2P game traffic (UDP, SSNP) | `SYNETPEER_VERSION` = 3, u16 after magic | `syNetPeerHandlePacket` drops mismatches |
| Lobby signaling (WebSocket) | `?v=1|2` URL param, room-pinned | Worker rejects with `version-mismatch` |

## Rules

- Message IDs are assigned explicitly and never reused — see
  `docs/netplay/protocol/messages.md`. Packet types are fixed constants
  (`SYNETPEER_PACKET_*`), not positional enum values.
- A peer on a different SSNP version is invisible (packets dropped), and
  a joiner on a different lobby version gets an explicit
  `version-mismatch` error. There is no silent downgrade.
- All multi-byte fields are written/read through `syNetPeerWriteU*` /
  `syNetPeerReadU*` helpers — never struct-cast — so the wire format is
  endianness-stable.
- Every packet carries a FNV-1a checksum over its semantic fields;
  corruption is detected and counted, not parsed.
- Backward compatibility is achieved by new packet *types* and new
  `SYNETPEER_VERSION`, never by changing the meaning of existing fields.

## The old-build problem

A user on build A playing a user on build B where only gameplay code
differed (same protocol version) desyncs silently. The mitigation is the
session manifest exchange (spec §Session manifest, TD-008): ruleset hash,
simulation-config hash, mod set, asset revision, build compatibility ID
are compared before `BATTLE_START`.
