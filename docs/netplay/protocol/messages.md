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
| C→S | `?role=host|join|spec&v=1|2&room=&slot=&key=` | URL params on upgrade |
| S→C | `{type:"welcome", upkey, ...}` | upkey is the slot-attach secret |
| S→C | `{type:"error", reason}` then close 4000 | see error-codes.md |
| S→C/C→S | `{type:"peer", ...}` text | join/part/topology |
| C→S | binary endpoint announce | hole-punch offer |
| S→C | binary endpoint + relay flag | when direct path is impossible |
| both | binary game frames | v2: slot-indexed routing |

Room codes: `^[A-HJ-NP-Z2-9]{6}$` (6 chars, ambiguous glyphs excluded).
Text messages ≤ 2048 B; binary ≤ 1024 B; rate limits 20 text/s and
600 binary/s per socket, plus a bounded join/uplink rate per room.
