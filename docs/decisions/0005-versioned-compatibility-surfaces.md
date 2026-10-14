# ADR-0005: Independent version numbers per compatibility surface

## Status

Accepted (2026-10)

## Context

The project has many external contracts — saves, config, o2r assets, mod
API, netplay wire protocol, lobby protocol — that historically shared the
app version or had none. Coupling them means a netplay message change
forces a "new release" semantic onto saves, or a save-format bump appears
to break the lobby.

## Decision

`include/smash_classic/version.h` declares one constant per surface.
They move independently; each is enforced at its own boundary:

- Netplay wire: `SYNETPEER_VERSION` in the packet header (currently 3).
- Lobby: `?v=` param pinned per room by the host (currently 2).
- Saves/config/mod API/o2r: numbers reserved now, enforced as the
  schema/manifest work lands (see debt register TD-004..TD-008).

Breaking changes bump only their own surface. Message/enum IDs are
assigned once, never reused — gaps are permanent.

## Consequences

- A PR touching a wire format must bump exactly one constant and update
  `docs/compatibility/policy.md`.
- The session manifest (TD-008) carries the effective set so peers
  negotiate capabilities instead of assuming version equality implies
  identical behavior.
