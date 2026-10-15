# ADR 0002 — Submodule isolation rules

**Status:** accepted (2026-10)

Submodules stay at their current paths: `components/decomp/`, `components/libultraship/`, `components/game-runtime/`. Physically relocating them (`game/components/decomp/`, `third_party/…`) was considered and deferred — the churn in `.gitmodules`, `new-worktree` and CI outweighed the benefit.

Rules:
- No port features inside `components/decomp/` beyond what the `port-patches`/`decomp` branch needs to compile against the port (include-path shims like `port_log.h` are acceptable and load-bearing).
- No direct patching of `components/libultraship/` or `components/game-runtime/` when the change can live in a port-side adapter (`src/game/engine/decomp/`).
- Game logic must not depend on Torch internals; Torch is build-time asset extraction only.
- Any required submodule change is committed on the submodule's own branch and the pointer bumped in the outer tree (`CLAUDE.md` worktree workflow).
