# Generated files — what's committed vs regenerated

Rule of thumb: **generated files are committed only when they cannot be
rebuilt without a baserom** (CI builds without the ROM) — everything else
is emitted into `<build>/generated/` or `out/` and ignored.

## Committed generated files

| Path | Generator | Regenerate when |
|------|-----------|-----------------|
| `assets/game/<ver>/reloc_*.yml` | `tools/code-generation/generate_yamls.py` (from `tools/code-generation/reloc_data_symbols.<ver>.txt` + ROM) | reloc symbol lists change (rare; frozen v1.0). Golden-hashed: `tests/support/golden/extraction_recipes.sha256` — update via `python tests/integration/assets/extraction_recipes_test.py --update` |
| `assets/game/<ver>/*.yml` (audio, particles, transitions…) | same | recipe tweaks |
| `assets/game/<ver>/manifests/*.json` | `tools/code-generation/generate_fighter_slices.py` | deblob slice map changes |
| `generated/src/<ver>/ssb64_reloc_rebuild.<ver>.cpp` | `tools/code-generation/generate_fighter_slices.py` (needs `roms/baserom.<ver>.z64`) | deblob regen. **Only committed generated source** — see `generated/README.md` |

Policy: every committed generated file must carry the generated banner
(`check_generated_headers` enforces) and `make codegen-check` must stay
clean — a regenerate that changes the tree means someone edited output
instead of the generator inputs.

## Regenerated at build time (never committed)

Emitted under `<build>/generated/` by `GenerateRelocArtifacts` from
committed inputs only — safe on ROM-free CI:

| Output | Producer | Emitted to |
|--------|----------|------------|
| `reloc_data.<ver>.h` | `tools/code-generation/generate_reloc_stubs.py` (`reloc_data_symbols.<ver>.txt`) | `<build>/generated/include/<ver>/` |
| `RelocFileTable.<ver>.{cpp,h}` | `tools/code-generation/generate_reloc_table.py` (`assets/game/<ver>/reloc_*.yml`) | `<build>/generated/src/<ver>/` |

Regenerate manually:

    cmake --build <build> --target GenerateRelocArtifacts

## Regenerated locally (never committed)

| Output | Producer |
|--------|----------|
| `out/build/<preset>/**` | cmake/ninja build trees (incl. `<build>/generated/`) |
| `out/reports/`, `out/test-results/` | lint/security/test runners |
| `BattleShip.o2r`, `f3d.o2r` | Torch extraction (needs ROM in `roms/`) |
| `components/decomp/src/credits/*.credits.{encoded,metadata}` | `tools/assets/pipeline/creditsTextConverter.py` |
| `*.o2r.recipe`, `torch.hash.yml` | Torch run artifacts |
| `dist/` | `package-*` targets |

## User-local (never committed)

`roms/*.z64` (copyrighted), `*.sav`, `ssb64_save.bin`, `BattleShip.cfg.json`,
`.venv/`, `node_modules/`, `logs/`, `debug_traces/`, `tex_dump/`, `extract/`,
`test.zip`, `x64/`, IDE files (`.vs/`, `.idea/`, `*.user`).

## Checking

```
git status --porcelain   # must stay clean after build+test
make codegen-check       # generated files match generators
python tools/analysis/lint/check_generated_headers.py
python tests/integration/assets/extraction_recipes_test.py
```
