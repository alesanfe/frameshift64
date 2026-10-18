# Contributing to BattleShip

BattleShip is a PC port of Super Smash Bros. 64 built on the complete
decompilation (`components/decomp`). Contributions of all sizes are welcome —
this document describes how to get a change accepted.

## Ground rules

1. **No copyrighted material.** Never commit ROM dumps, extracted assets, or
   disassembly output beyond what the decomp project already ships. Asset
   extraction happens on the user's machine via `ExtractAssets`.
2. **Decomp preservation.** Code under `components/decomp` describes the
   *game's behavior*. Keep IDO idioms (goto, unusual casts, odd temporaries)
   that encode N64 semantics — they are load-bearing. Port-only fixes belong
   in `src/` or behind `#ifdef PORT` in decomp. Behavior > byte-matching;
   document intentional deviations in `docs/bugs/`.
3. **Architecture is enforced, not suggested.** Each `src/<module>` compiles
   with a private include scope (`SSB64_MODULE_SCOPE_*` in `CMakeLists.txt`)
   and `tools/analysis/lint/check_module_deps.py` gates the per-file matrix.
   A cross-module include that isn't in scope fails the build — don't
   rebaseline it away.

## Workflow

```bash
# configure + build (Windows example; see docs/architecture/build_and_tooling.md)
cmake -B out/build/windows-debug
cmake --build out/build/windows-debug --config Debug --target ssb64 -j 4
ctest --test-dir out/build/windows-debug -C Debug

# project lint suite (uses .venv-lint tools; creates it on first run)
python tools/analysis/lint/run_project_lint.py
```

Parallel agents/contributors: use `./scripts/development/new-worktree.sh <slug>`
so each session gets its own checkout + build tree. Build paths must be
absolute; cap parallelism at `-j 4`.

## Pull request guidelines

- **Scope**: one logical change per PR. Structural refactors go in phases
  (see `AGENTS.md` — max ~5 files per phase, cleanup commits separate).
- **Tests**: add or update tests when behavior changes. Relevant suites:
  `tests/unit` (protocol, parsers), `tests/e2e/netplay` (rollback harness),
  `tests/fuzz` (parser corpora), `ctest` smoke tests.
- **Style**: match the file you touch. C++ uses the surrounding module's
  conventions; `port_*` is the decomp↔port C ABI prefix; user-visible
  strings go through `TR()`/PortI18n.
- **Docs**: update the doc that covers the area you changed
  (`docs/architecture/`, `docs/testing/`, etc.). Significant bugfixes get an
  entry under `docs/bugs/<topic>_<YYYY-MM-DD>.md` linked from the index.
  Architectural changes need an ADR under `docs/decisions/`.
- **Commits**: imperative subject lines; explain *why* in the body.

## CI expectations

Every PR runs `build.yml` (Windows/Linux/macOS) plus `repo-checks.yml`
(lints, codegen verification, module-deps, public API, test policy).
The rollback determinism suite (`determinism.yml`) runs nightly when the
`BASEROM_US_Z64` secret is configured — it needs a legally-owned ROM and can
never run from a fork's PR. If your change touches netplay or the sim,
run `scripts/testing/netplay_localtest.ps1` locally and say so in the PR.

## Where to start

- `AGENTS.md` — canonical repo context (layout, worktree flow, gotchas)
- `docs/architecture/overview.md` — module map and the port_* ABI boundary
- `docs/decisions/` — why the tree looks the way it does
- `tests/` — mirrors the `src/` modules; `docs/testing/test-catalog.md` lists them

## Reporting bugs

Open an issue with repro steps and, if the game crashed, the crash bundle
(`crash_bundle/` next to the executable — see
`src/observability/crash_bundle.cpp`). For netplay desyncs include the
`out/logs/netplay-test/` traces if you ran the test harness.
