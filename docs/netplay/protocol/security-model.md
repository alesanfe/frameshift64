# Netplay security model

Trust boundary: **the lobby is honest-but-curious infrastructure; peers
are untrusted**. The room code is the only room secret — everything else
is authenticated per-connection.

## Enforced today

- **upkey**: on `welcome`, the Worker issues a per-slot secret; attaching
  an uplink requires `&key=`. A room-code guesser cannot take a slot
  (commit 2026-10-13, `docs/bugs/README.md` → `netplay_input_injection`).
- **Server-enforced star**: the Worker routes binary frames by
  authenticated slot, ignoring any client-supplied `to_slot`.
- **FORWARD gating**: clients accept FORWARD only on the host link for
  forward-only remotes; `src_player` is host-stamped, not self-declared.
- **Session IDs**: lobby-generated per match; P2P packets carrying a
  foreign session are dropped at parse.
- **Rate limits**: per-socket (20 text/s, 600 binary/s), per-room bounded
  join rate; message size caps (2048 text / 1024 binary).
- **Spectator confinement**: spec slots are receive-only at the server;
  binary game traffic from a spec socket is not forwarded.

## Deliberately out of scope

- Content encryption (inputs are game data, not secrets; the room code +
  upkey are the confidentiality boundary for *access*).
- Cheat prevention beyond desync detection — a modified client can lie
  about its inputs; confirmed-hash divergence is the detection, not
  prevention.
- Cloudflare credential management (covered by `docs/security/`).

## Residual risks

See `docs/security/network-threat-model.md` for the full table — top
items: lobby compromise is invisible to clients (mitigated by end-to-end
session_id + confirmed-hash checks, not by encryption), replayed packets
are dropped by sequence tracking but replay *across sessions* is bounded
by lobby-generated session IDs.
