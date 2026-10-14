# Network threat model (netplay + lobby)

Extends `threat-model.md`; protocol details in `docs/netplay/protocol/`.

## Amplification

The relay host forwards one datagram per client — bounded by
`MAX_PLAYERS_V2`/spectator count and by the lobby's binary rate cap. The
Worker never replicates text frames to spectators as binary and never
reflects binary to non-members: no amplification primitive exists.

## Flooding / exhaustion

- Per-socket: 20 text msgs/s, 600 binary msgs/s, 2048/1024-byte caps.
- Per-room: bounded join/uplink attempt rate (`recentJoins` timestamps).
- Per-match: remote silence > 10 s moves the session to Recovering instead
  of stalling all players.

## Session fixation / impersonation

- Slots attach only with the slot's `upkey` (issued server-side in
  `welcome`); the room code alone is insufficient.
- FORWARD's `src_player` is stamped by the relaying host — a client
  cannot choose which player its forwarded traffic appears to come from.
- Relayed links are distinguishable (`relay` flag); a relayed remote can
  never also claim a direct link for the same slot.

## Deserialization

The UDP parser is a fixed-stride field reader — no length-prefixed blobs,
no recursion, no allocation driven by packet content. `frame_count` is
capped at 16 and only bounds how many of the 16 fixed slots are live.

## Downgrade

Room protocol version is pinned by the host at creation; joiners on the
other version are rejected (`version-mismatch`), never translated.
`SYNETPEER_VERSION` mismatch makes P2P packets undecodable → the pair
never synchronizes. There is no negotiation path to weaken.

## Spectator abuse

Spectators are receive-only at the server and cannot punch, hold a player
slot, or emit binary frames. A spec socket is capped to the same rate
limits; no path upgrades spec → player mid-session.

## Intentional desync / rules manipulation

A peer cannot alter another client's match config post-`MATCH_CONFIG`
(control packets are fixed-size, pre-match only). In-match, divergent
simulation is detected via per-packet confirmed hashes; the mutant suite
(`netplay_mutation_suite.ps1`) verifies each check actually catches its
attack.
