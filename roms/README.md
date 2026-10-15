# roms/

Drop your legally-owned SSB64 ROM dumps here. **Never commit them** — this
directory is gitignored except this README and `.gitkeep`.

Expected files:

| File | Required | Purpose |
|------|----------|---------|
| `baserom.us.z64` | yes (US builds) | Source ROM for asset extraction + decomp matching |
| `baserom.jp.z64` | only for `REGION=jp` | JP build variant |

The extraction pipeline (`make assets` / `ExtractAssets` target) reads the
ROM and produces `BattleShip.o2r` (+ `f3d.o2r`) next to the game binary.
No ROM bytes are committed, distributed, or included in any package —
see `make release-binary-check` / `package-contents-check`.

ROM checksums and version details: `docs/architecture/overview.md`.
