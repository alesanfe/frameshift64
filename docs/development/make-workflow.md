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
| `generate-sources` | regenerate committed inputs (`assets/game`, reloc yamls) | python+PyYAML |
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
`ARTIFACT_DIR` = `out/artifacts` · `PACKAGE_DIR` = `out/packages` ·
`REPORT_DIR` = `out/reports` · `LOG_DIR` = `out/logs`

## Module map

| File | Owns |
|------|------|
| `make/common.mk` | shell, variables, host detection, guards, `doctor`, `layout-check` |
| `make/dependencies.mk` | `setup`/`install*`/submodules/venv/lobby deps |
| `make/generation.mk` | `codegen`, `assets`, `generate-yamls`, `synth-verify` |
| `make/build.mk` | `configure`, `build`, `run`, `clean*`, sanitizers presets |
| `make/test.mk` | pytest/ctest/e2e/fuzz/coverage |
| `make/netplay.mk` | `test-netplay`, `test-lobby*`, `test-websocket*`, `netplay-matrix` |
| `make/quality.mk` | lint/format/arch + ratchets (`complexity-check`, `file-size-check`, `encoding-check`, `baseline-check`) |
| `make/security.mk` | semgrep/dependency-audit/sbom/release-binary-check |
| `make/abi.mk` | `abi-check`/`abi-exports`/`abi-compat`/`abi-baseline` (`port_*` surface vs `config/abi/`) |
| `make/decomp.mk` | facade over `components/decomp` (`decomp-init`/`extract`/`validate`/…) |
| `make/reproducibility.mk` | `worktree-check`, `source-repro-check`, `asset-repro-check`, `package-repro-check`, `clean-room-check`, `reproducibility-check`/`reproduce` |
| `make/platforms.mk` | host/config builds (`debug`, `build-release`, `relwithdebinfo`, `minsizerel`), `emscripten[-st|-test|-serve]`, `switch*` (SW-08 gated), sanitizers (`test-asan`, `test-ubsan`, `tsan`/`test-tsan`, `sanitizer-matrix`/`test-sanitizers`) |
| `make/packaging.mk` | `package*`/`artifacts`/`artifact-manifest`/`package-verify`/`package-smoke-test` |
| `make/release.mk` | `release`/`release-prepare`/`release-build`/`release-verify`/`release-notes`, `release-check`/`release-dry-run` |
| `make/workflows.mk` | composites: `dev`, `pre-commit`, `pre-push`, `ci-local`, `verify-all` |
| `make/diagnostics.mk` | `status`, `versions`, `environment`, `graph-deps`, `graph-targets`, `logs-tail`, `report` |
| `make/makefile.mk` | facade self-validation (`makefile-check`) |
| `make/help.mk` | categorized `help`/`help-all`/`help-json`/`help-<category>` |
| `make/platforms/*.mk` | per-platform targets (`android*`, `uwp`) |

## Workflows

```
make pre-commit   # format-check + quick-check + test-unit     (seconds)
make pre-push     # check + test + static-analysis             (minutes)
make verify       # layout + doctor + check + security + generated + build
make verify-all   # exhaustive: verify + security + abi-check + reproducibility
                  # (needs a CLEAN tree — worktree-check is part of the gate)
make ci-local     # = verify — what CI runs
```

## Platform facades

| Target | Reality |
|--------|---------|
| `emscripten` / `emscripten-st` | `web-wasm` / `web-wasm-st` presets; gated on `out/emsdk` |
| `emscripten-serve` | `platforms/web/serve.py` (COOP/COEP + CSP headers; `PORT=8931`, `BUILD=web-wasm-st`) |
| `switch` / `switch-check` / `package-switch` | gated SW-08 — fails informatively until the SDK exists |
| `test-asan` / `test-ubsan` | `<host>-asan` / `<host>-ubsan` presets (ubsan: not on MSVC) |
| `tsan` / `test-tsan` | gated — no `linux-tsan` preset yet; delegates to WSL on Windows once registered |

## Reproducibility gates

`worktree-check` (clean tree + submodules at recorded SHAs) →
`source-repro-check` (committed generated files match their generators) →
`asset-repro-check` (same ROM + recipes extract a byte-identical `.o2r`
twice — the build pins `SOURCE_DATE_EPOCH=315532800`) →
`package-repro-check` (two package runs hash identically) →
`clean-room-check` (detached pristine worktree: checkout → configure →
build → ctest, then asserts the build dirtied no tracked files).
`reproduce` = `reproducibility-check` = the whole gate.

## Local release

`make release VERSION=X.Y.Z` runs `release-prepare` (semver, clean tree,
branch=main, no duplicate `vX.Y.Z` tag) → `release-build` (release-preset
packages + checksums + SBOM + notes into `out/artifacts/`) →
`release-verify` (`verify-fast` + `abi-check` + `package-verify`).

Publication is a two-step, double-guarded operation:
`CONFIRM_RELEASE=1 make release-tag VERSION=X.Y.Z` creates the annotated
tag, then `CONFIRM_RELEASE=1 make release-publish VERSION=X.Y.Z
REMOTE=<name>` pushes it — pushing `v*` fires `.github/workflows/release.yml`,
which builds every platform and creates the GitHub Release. No remote is
ever guessed: `REMOTE` must be passed explicitly.

## Help

`make help` shows the daily-driver subset. `make help-all` prints every
documented target grouped by category (`[Build]`, `[Test]`, `[Netplay]`,
`[Quality]`, `[Security]`, `[ABI]`, `[Decomp]`, `[Package]`, `[Release]`,
`[Reproducibility]`, `[Platform]`, `[Workflow]`, `[Clean]`, `[Diagnostics]`,
`[Makefile]`, `[Help]`). `make help-<category>` filters; `make help-json`
exports the catalog to `out/reports/make-targets.json`.

Every public target must carry `## [Category] description` — enforced by
`makefile-docs-check` (part of `makefile-check` and `check`). Internal
helpers stay undocumented via a `guard-*` name or a `# internal` comment.

## ABI gates

`make abi-check` runs: `abi-version` (PORT_ABI_* constants valid),
`abi-exports` (exported `port_*` symbols match `config/abi/exports.json`),
`abi-compat` (semver vs `config/abi/version.json`'s last release).

When the surface intentionally changes: `ALLOW_UPDATE=1 make abi-baseline`
after a build regenerates the baseline — the guard makes accidental
regeneration impossible.
