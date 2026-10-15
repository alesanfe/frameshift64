# Deprecated components

Nothing is formally deprecated at this time. When deprecating:

1. Mark the component + add a `DEPRECATED:` note at its entry point.
2. Record here: component, replacement, removal release target.
3. Keep it working (or loudly failing) for at least one release.

## Historical removals (for reference)

| Removed | Replacement | Notes |
|---------|-------------|-------|
| `port/` tree | `src/` responsibility split | 2026-10 reorganization |
| `server/` | `services/netplay-lobby/` | 2026-10 |
| `mods/examples/` | `mods/examples` + `mods/templates` | 2026-10 |
| `assets/game/` at root | `assets/game/` | 2026-10; runtime still stages `assets/game/<region>` next to the binary |
| `tools/debugging/` | `tools/debugging/` | 2026-10 |
| `ssb64.o2r` | `BattleShip.o2r` + `f3d.o2r` | early port-asset archive, long gone |
