# Module membership criteria — the first-level rule for `src/`

`src/` mixes four legitimate axes: lifecycle layer, game-domain features,
user-facing I/O, and platform detail. This file is the tie-breaker — when
code could plausibly live in two places, the rule below decides.

## The rule

**A module belongs to the first row of this table whose question it
answers "yes" to.** Rows are ordered by who *consumes* the code, not by
how important it is.

| # | Top-level | Membership test | Depends on |
|---|---|---|---|
| 1 | `application/` | Does it wire composition, boot, shutdown, or app-global state? | everything below |
| 2 | `game/` | Does it implement or bridge *game* behaviour (decomp ABI, gameplay rules, mods)? | engine, shared |
| 3 | `networking/` | Does it move bytes or presence to other machines? | game contracts, infra |
| 4 | `interfaces/` | Does a human perceive it (eyes, ears, hands)? | feature contracts, infra |
| 5 | `platform/` | Does it exist only because this OS/toolchain lacks or differs? | infra contracts |
| 6 | `config/` `i18n/` `observability/` `persistence/` | Is it a leaf service with *no* dependency on the rows above? | nothing (observability excepted) |
| 7 | `shared/` | Is it a dependency-free contract (types, results) usable by all? | nothing |

`resources/` was dissolved (2026-10): its members live next to their real
owner — relocation + CSS asset providers under `game/engine/`, N64 audio
backend under `interfaces/audio/n64/`, frame interpolation + shader
manifest + HiRes packs under `interfaces/rendering/`. Resource-loading
code joins the module that consumes it instead of a catch-all bucket.

## Tie-breakers (the ambiguous cases that prompted this doc)

- **Netplay is a feature, not transport plumbing.** `src/networking/` owns
  protocol+rollback+session; `src/platform/http` owns the generic HTTP GET.
  Downloads for updates/mods use platform/http and live in the feature dir.
- **Save/profile state lives in `persistence/`**, not `game/` — the
  profile snapshot is a service other features consume, not gameplay logic.
  (The `core/` catch-all was flattened 2026-10-18 — leaf services are
  first-level dirs: `config/`, `i18n/`, `observability/`, `persistence/`.)
- **Narration is presentation** — it's user-facing feedback, so
  `interfaces/presentation/`, not `platform/` (moved 2026-10-18).
- **Watchdog/liveness is observability**, so `observability/` (moved
  2026-10-18); only `application/` may *start/stop* it.
- **A CVar schema entry does not create a module.** Config keys live in
  `config/schema.cpp` until that file outgrows ~500 LOC, then split
  by domain under `config/schema/`.

## Hard rules

1. `decomp` never sees `src/` headers; `game/engine/decomp` is the only
   bridge home. A `#include` past the `port_*` ABI is an instant reject.
2. Leaf services (`config/`, `i18n/`, `observability/`, `persistence/`) and
   `shared/` may not depend upward — no game/menu/netplay includes.
   `observability` (port_log) is the sanctioned exception every scope
   already carries.
3. No `utils/`, `common/`, `misc/` — a file with no membership answer is
   usually `shared/` (pure contract) or mis-scoped feature code.
4. Adding a top-level `src/` dir requires updating this file, the
   `SSB64_MODULE_SCOPE_*` list, and `check_module_deps.py` in one commit.
