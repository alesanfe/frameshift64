# Generated files — what's committed vs regenerated

Rule of thumb: **generated files are committed only when they are a build
input on a fresh clone** (CI builds without the ROM) — everything else is
rebuilt into `out/` and ignored.

## Committed generated files

| Path | Generator | Regenerate when |
|------|-----------|-----------------|
| `assets/recipes/<ver>/reloc_*.yml` | `tools/codegen/generate_yamls.py` (from `tools/codegen/reloc_data_symbols.<ver>.txt` + ROM) | reloc symbol lists change (rare; frozen v1.0). Golden-hashed: `tests/support/golden/extraction_recipes.sha256` — update via `python tests/integration/assets/extraction_recipes_test.py --update` |
| `assets/recipes/<ver>/*.yml` (audio, particles, transitions…) | same | recipe tweaks |
| `generated/src/<ver>/RelocFileTable.<ver>.cpp` + `generated/include/` | `tools/codegen/generate_reloc_table.py` | reloc yamls change. Enforced by `check_generated_headers` (banner) + `make codegen-check` (CI: regen must be a no-op) |
| `include/reloc_data.<ver>.h` | splat/reloc pipeline | `#define` symbol→id map; no ROM bytes |

Policy: every committed generated file must carry the generated banner
(`check_generated_headers` enforces) and `make codegen-check` must stay
clean — a regenerate that changes the tree means someone edited output
instead of the generator inputs.

## Regenerated locally (never committed)

| Output | Producer |
|--------|----------|
| `out/build/<preset>/**` | cmake/ninja build trees |
| `out/reports/`, `out/test-results/` | lint/security/test runners |
| `BattleShip.o2r`, `f3d.o2r` | Torch extraction (needs ROM in `roms/`) |
| `reloc_data.h` variants under `out/` | codegen |
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
python tools/lint/check_generated_headers.py
python tests/integration/assets/extraction_recipes_test.py
```
