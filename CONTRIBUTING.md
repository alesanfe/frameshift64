# Contributing

- Read `CLAUDE.md` first — it documents the worktree workflow, build commands (`-j 4` cap), and the decomp-preservation rules.
- Repo layout and module boundaries: `docs/architecture/overview.md`, decisions in `docs/decisions/`.
- New port code goes under `src/<module>/` — nothing new at repo root. New tools under `tools/<category>/`, automation under `scripts/<category>/`.
- Docs are classified by function under `docs/` (living docs vs dated investigations — see `docs/README.md`).
- Generated output goes to `out/` (gitignored). Never commit ROMs, `.o2r` archives, or copyrighted assets.
- Lint: `python tools/analysis/lint/run_project_lint.py` (writes to `out/reports/lint/`).
