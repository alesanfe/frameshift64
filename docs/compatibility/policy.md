# Compatibility policy

Every external contract of this project is versioned independently and
enforced at its own boundary. The canonical version numbers live in
`include/smash_classic/version.h` — do not scatter literals.

## Rules

1. **Independent numbering.** A breaking change to one surface bumps only
   that surface. A netplay message change must not force a save bump.
2. **Negotiate or refuse, never silently misinterpret.** A peer, save,
   config, mod, or archive on an incompatible version must produce a
   visible, actionable error — not a silent fallback.
3. **Old readers must reject new formats cleanly.** Unknown version →
   structured error, no partial parse.
4. **New readers must migrate or explicitly reject old formats.** A
   migration is code + a test fixture, not a comment.
5. **Non-simulation data may be lenient; simulation-affecting data may
   not.** Anything that can change game state between peers (ruleset,
   gameplay mods, asset revision) is negotiated *before* the match starts —
   see `docs/netplay/protocol/specification.md`.
6. **Widening, not renumbering.** Enum/message IDs are assigned once and
   never reused. Gaps are permanent.

## Surfaces

| Surface | Version | Enforcement point | Doc |
|---------|---------|-------------------|-----|
| Application | `VERSION` (semver) | release tags | — |
| Netplay wire protocol | `SMASH_NETPLAY_PROTOCOL_VERSION` (3) | magic+version check in `syNetPeerHandlePacket` | [netplay-protocol.md](netplay-protocol.md) |
| Lobby signaling | `SMASH_LOBBY_PROTOCOL_VERSION` (2) | `?v=` param, `version-mismatch` reject | [netplay-protocol.md](netplay-protocol.md) |
| Saves | `SMASH_SAVE_FORMAT_VERSION` (1) | `src/core/port_save.cpp` | [saves.md](saves.md) |
| Configuration | `SMASH_CONFIG_FORMAT_VERSION` (1) | CVar store; schema pending | [configuration.md](configuration.md) |
| Mod API | `SMASH_MOD_API_VERSION` (1) | `manifest.json` + `ModRegistry` | [mods.md](mods.md) |
| Asset archive | recipe hash today; `SMASH_ASSET_FORMAT_VERSION` reserved | `.recipe` sidecar auto re-extract | [assets.md](assets.md) |
| Graphics backend | runtime capability probe | Fast3D feature detection | [supported-platforms.md](supported-platforms.md) |
| Submodules | pinned SHAs in `.gitmodules` | worktree flow in CLAUDE.md | `docs/architecture/overview.md` |
| Older builds | recipe hash + protocol version | auto re-extract, version-mismatch | this file |

## Current matrix

    Application:        1.4.1
    Netplay protocol:   3   (wire, decomp/src/sys/netpeer.c)
    Lobby protocol:     2   (services/netplay-lobby)
    Save format:        1
    Config format:      1   (schema/migrations pending — see debt register)
    Mod API:            1
    Asset archive:      recipe-hash guarded
