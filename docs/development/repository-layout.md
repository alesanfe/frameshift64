# Repository layout — Makefile contract

`make layout-check` fails fast when a required path is missing. Keep this
list in sync with `REQUIRED_FILES` / `REQUIRED_DIRS` in the Makefile.

## Required files

| Path | Why |
|------|-----|
| `CMakeLists.txt`, `CMakePresets.json` | build system entry |
| `Makefile` | task facade |
| `pyproject.toml` | python tooling config |
| `tools/developer/testing/run_tests.py` | test runner (`TEST_RUNNER`) |
| `tools/analysis/lint/run_project_lint.py` | lint orchestrator (`LINT_SCRIPT`) |
| `tools/analysis/lint/check_module_deps.py` | architecture gate |
| `services/netplay-lobby/package.json` + `package-lock.json` | reproducible `npm ci` |
| `roms/README.md` | documents the ROM drop dir |

## Required dirs

`src` `include` `decomp` `libultraship` `torch` `cmake` `tools` `assets`
`assets/game` `config/controllers` `services` `platforms` `tests` `docs`
`config` `generated` `specs` `roms`

## Path variables (verified against the tree)

| Var | Value |
|-----|-------|
| `LINT_SCRIPT` | `tools/analysis/lint/run_project_lint.py` |
| `MODULE_DEPS_SCRIPT` | `tools/analysis/lint/check_module_deps.py` |
| `ENV_VARS_SCRIPT` | `tools/analysis/lint/check_env_vars.py` |
| `DOCS_CHECK_SCRIPT` | `tools/analysis/lint/check_docs.py` |
| `RETIRED_PATHS_SCRIPT` | `tools/analysis/lint/check_retired_paths.py` |
| `RELEASE_BIN_SCRIPT` | `tools/analysis/lint/check_release_binary.py` |
| `CVAR_SCHEMA_SCRIPT` | `tools/analysis/lint/check_cvar_schema.py` |
| `GEN_HEADERS_SCRIPT` | `tools/analysis/lint/check_generated_headers.py` |
| `SBOM_SCRIPT` | `tools/analysis/lint/gen_sbom.py` |
| `TEST_RUNNER` | `tools/developer/testing/run_tests.py` |
| `COVERAGE_TOOL` | `tools/developer/testing/coverage.py` |
| `PARSER_FUZZ` | `tools/analysis/fuzz/run_parser_fuzz.py` |
| `LOBBY_DIR` | `services/netplay-lobby` |
| `E2E_DIR` | `tests/e2e/netplay` |
| `ANDROID_DIR` | `platforms/android` |
| `UWP_DIR` | `platforms/uwp` |

## CMake target variables

| Var | Target |
|-----|--------|
| `APP_TARGET` | `ssb64` (exe: `BattleShip.exe`) |
| `ASSETS_TARGET` | `ExtractAssets` |
| `ASSET_HEADERS_TARGET` | `ExtractAssetHeaders` |
| `RUNTIME_ASSETS_TARGET` | `StageRuntimeAssets` |
| `O2R_TARGET` | `GenerateBattleShipO2R` |
| `STAGE_ASSETS_TARGET` | `DeriveStageAssets` |
| `SYNTH_VERIFY_TARGET` | `synth_verify` |

## Gates that run layout-check

`make setup` · `make release-check` · CI `repo-checks.yml` (first step).

## Moving a path — checklist

1. `git mv` the tree.
2. Update the variable/table above **and** in `Makefile`.
3. Update consumers: `CMakeLists.txt`, `cmake/*.cmake`, `tools/**`,
   `tests/**`, `.github/workflows/*`, `.pre-commit-config.yaml`,
   `.yamllint.yml`, `.gitignore`, golden manifests (`tests/support/golden/`).
4. Add the old path to `tools/analysis/lint/check_retired_paths.py`.
5. `make layout-check && make verify-fast`.
