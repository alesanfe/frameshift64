# Architecture

The architecture reference lives under `docs/architecture/`:

- **`docs/architecture/overview.md`** — project status, module layout,
  the decomp ↔ port `port_*` ABI boundary, dependencies.
- `docs/architecture/build_and_tooling.md` — CMake module scopes, quality
  tiers, generated-code flow.
- `docs/architecture/repository-inventory.md` + `current-tree.txt` —
  file-level census of the tree.
- `docs/decisions/` — ADRs for structural choices.

Rules that matter most:

1. **decomp boundary** — `components/decomp` never includes port headers;
   it reaches the port only through the `port_*` extern "C" ABI in
   `src/game/engine/`.
2. **Module scopes** — each `src/` module is a `smash_<mod>` OBJECT target
   whose include dirs are limited to `SSB64_MODULE_SCOPE_<mod>` in
   `CMakeLists.txt`. `tools/analysis/lint/check_module_deps.py` enforces
   the dependency rules in CI — violations go on the tracked-debt list.
3. **Generated vs source** — build output lives in `out/` (gitignored);
   only `generated/` is versioned, per `docs/development/generated-files.md`.
4. **Remotes** — `frameshift` is canonical; see `docs/development/remotes.md`.
