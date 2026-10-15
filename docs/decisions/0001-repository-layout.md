# ADR 0001 — Repository layout by responsibility

**Status:** accepted (2026-10)
**Context:** the repo root mixed product code (`port/`), decomp submodule, engine submodules, tooling, docs, CI config and generated artifacts. `port/` had become a catch-all for everything that wasn't decomp.

**Decision:**
- `src/` — all port-layer C++ code, split by responsibility: `application/`, `core/`, `bridge/`, `netplay/`, `features/`, `presentation/`, `resources/`, `platform/`, `compatibility/`.
- `game/components/decomp/` stays physically at `components/decomp/` (submodule); same for `components/libultraship/`, `components/game-runtime/`. Moving submodule paths is deferred — see ADR 0002.
- `services/netplay-lobby/` — the Cloudflare lobby service, deployable independently.
- `mods/` — mod SDK, examples (ex `mods/examples/`), templates.
- `tools/` — reusable programs; `scripts/` — thin automation; `tests/` — automated checks; `config/` — tool configuration; `docs/` — docs by function; `assets/` — source assets + extraction config; `out/` — all generated output (gitignored).
- `platforms/` — android/ and uwp/ packaging targets.

**Include compatibility:** `src/` and every leaf module dir are on the include path of both `ssb64_game` and `BattleShip`, so bare includes (`"port_log.h"`) keep resolving and prefixed includes became `"netplay/"`, `"features/"`, `"bridge/modding/"`, etc.

**Consequences:** new code goes in `src/<module>/`; nothing new should be added to repo root. Generated artifacts never live at root — use `out/`.
