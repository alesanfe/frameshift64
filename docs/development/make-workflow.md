# Make workflow — lifecycle stages

The Makefile separates lifecycle stages; each stage assumes the previous
one ran. `make dev-build` is the comfortable all-in-one during development.

```
install → generate-sources → configure → codegen → build → test → package → verify/release
```

## Stage targets

| Target | Does | Needs |
|--------|------|-------|
| `layout-check` | required files/dirs exist | clone |
| `doctor` | toolchain + ROM + submodules sane | — |
| `install` | `submodules` + `install-python` + `install-node` | layout-check |
| `install-python` | `.venv` + `pip install -e ".[test]"` | python |
| `install-node` | `npm ci` in `services/netplay-lobby` | node |
| `generate-sources` | regenerate committed inputs (`assets/recipes`, reloc yamls) | python+PyYAML |
| `configure` | `cmake --preset <PRESET>` only | generate-sources |
| `codegen` | `PrepareBuildInputs` + `ExtractAssetHeaders` | configure |
| `build` | compile `ssb64` only (no reconfigure) | configure |
| `dev-build` | `configure codegen build` — one shot | install |
| `test` / `test-suite` | pytest quick suite / named suite | build* |
| `package` / `package-<os>` | installer/portable → `dist/` | build |
| `verify` / `release-check` | full gate chain | everything |

*most test targets only need the python suite; e2e needs the built exe.

## Convenience / derived-state targets

| Target | Effect |
|--------|--------|
| `setup` | `layout-check doctor install generate-sources configure` — fresh checkout |
| `rebuild` | `clean-build configure codegen build` |
| `regenerate` | `clean-generated install generate-sources configure codegen` |
| `generated-check` | alias of `codegen-check`: regen must not change the tree (CI gate) |
| `clean-generated` | safe-rm `out/` `dist/` |
| `clean-dependencies` | safe-rm `.venv` `.venv-lint` `lobby/node_modules` |
| `prune` | `clean-generated` + `clean-dependencies` |

## Safe-delete policy

`clean-generated`, `clean-dependencies`, `prune` resolve each path and
**refuse to delete anything outside the repo**. They never touch:
`src` `include` `assets` `config` `docs` `specs` `tests` `roms` `decomp`
`libultraship` `torch` `generated/` (committed codegen contract — see
`generated-files.md`).

## Key variables

`PRESET` (default `<host>-debug`) · `REGION` (`us`|`jp`) · `JOBS`
`PROJECT_PYTHON` — `.venv`'s python when present, else system `python`
`ROM_PATH` · `BUILD_DIR` = `out/build/<PRESET>` · `OUT_DIR` = `out`
