# Migration status

Tracks the 2026-10 repository reorganization.

## Done

- `port/` → `src/` split by responsibility (application, bridge, core,
  netplay, features, presentation, resources, platform, compatibility)
- `server/` → `services/netplay-lobby/` (cloudflare + node reference +
  security tests)
- `assets/configuration/extraction/` → `assets/configuration/extraction/<region>`; controllers →
  `assets/configuration/controllers/`; css icons → `assets/source/`
- `tools/` classified (asset-pipeline, auditing, codegen, debugging, lint,
  modding, save-editor); `tools/debugging/` merged in
- `scripts/` classified (build, ci, development, packaging, testing)
- `mods/examples/` → `mods/examples` + `mods/templates`
- `android/`, `uwp/` → `platforms/`
- `config/` for lint/semgrep/cppcheck config; `out/` for all generated
  output; `CMakePresets.json`; `cmake/` modules + `platforms/<os>/cmake/`
  per-platform fragments (Windows, Linux, Switch, macOS)
- `docs/` categorized; ADRs seeded under `docs/decisions/`
- `docs/compatibility/`, `docs/netplay/protocol/`, `docs/security/`,
  `docs/engineering/`, `docs/testing/` policy layer

## In progress / next

- `src/netplay/` internal split: protocol / transport / rollback / lobby
  (TD-001) — pure code motion once active netplay debugging lands
- CMake component targets (TD-002)
- Session manifest handshake (TD-008)

## Deferred

- Physical submodule moves to `game/decomp`, `third_party/` — blocked on
  worktree-tooling verification (ADR-0002). Boundaries are enforced by
  convention + docs until then.
