# ADR 0001 — Repository layout by responsibility

**Status:** accepted (2026-10); **amended** (2026-10)
**Links:** `docs/architecture/overview.md`, ADR 0002 (submodule paths), ADR 0003 (`out/`)

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

**Amendment (2026-10):** the flat `src/` module list in the Decision above is
obsolete — `core/`, `bridge/`, `features/` and `resources/` were later
dissolved/redistributed. The authoritative module list is the
`SSB64_MODULE_DIRS` block in `CMakeLists.txt` (currently `application`,
`config`, `i18n`, `observability`, `persistence`, `game/`, `interfaces/`,
`networking/`, `platform/`), documented in `docs/architecture/overview.md`.
Module include boundaries are now compiler-enforced (`SSB64_MODULE_SCOPE_*`
private include dirs) and lint-enforced (`check_module_deps.py`), replacing
the "everything on a shared include path" arrangement described in Include
compatibility.
