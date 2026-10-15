# Current build — working commands (pre-refactor baseline)

Snapshot taken at tag `pre-structure-refactor` (branch
`refactor/professional-project-structure`). These commands are verified on
this machine (Windows, VS 2022 BuildTools).

## Toolchain on this host

| Tool | Location |
|------|----------|
| MSVC | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.x` |
| WinSDK | 10.0.26100 |
| CMake/CTest | `…\BuildTools\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\` |
| Ninja | `python -m ninja` (pip user install) |
| vcpkg | reused clone at `out\build\verify\libultraship\vcpkg` |

`out\msvc-env.bat` wraps `vcvars64` + ninja + cmake + `VCPKG_ROOT` — pass any
cmake invocation as arguments (it is not on PATH; the BuildTools install has
no `cmake.exe` in PATH by default).

## Build

```bat
:: configure (once, or when presets change)
out\msvc-env.bat --preset ninja-base -B out\build\ninja-verify

:: incremental build
out\msvc-env.bat --build out\build\ninja-verify

:: target only
out\msvc-env.bat --build out\build\ninja-verify --target netpacket_test
```

Presets (`CMakePresets.json`): `ninja-base`, `windows-debug`,
`windows-release`, `linux-debug`, `linux-release`, `macos-debug`,
`macos-release`, `jp-release`, `lint`.

Gotchas:

- `MSVC_RUNTIME_LIBRARY` must be `MultiThreaded` on every `smash_*` OBJECT
  lib — the exe links the static CRT; `/MD` objects leave `__imp_*` externs
  unresolved (LNK2019).
- vcpkg runtime DLLs are staged post-build via `cmake/CopyDirIfExists.cmake`
  (generator-expression safe). Without them the exe exits `0xC0000135`
  (STATUS_DLL_NOT_FOUND) — fuzz runs report this as "crashes".
- `baserom.us.z64` lives in `roms/` (gitignored); `BattleShip.o2r` +
  `f3d.o2r` must sit next to the binary to launch the game (build only needs
  them for `ExtractAssets`-adjacent targets).

## Tests

```bat
:: native unit tests (ctest labels: unit/netplay/protocol/deterministic)
"…\CMake\bin\ctest.exe" --test-dir out\build\ninja-verify --output-on-failure

:: Makefile quick suite (unit+smoke, no rom/net/gpu)
make test
make test-suite TEST_SUITE=<name>   :: tests/config/test-suites.yml

:: parser fuzz vs the built binary (needs staged DLLs)
python tools/analysis/fuzz/fuzz_parsers.py --binary out\build\ninja-verify\BattleShip.exe

:: netplay e2e (needs node lobby + .o2r next to the exe)
python tests\e2e\netplay\spectate_test.py --build-dir out\build\ninja-verify
python tests\e2e\netplay\spectate_test.py --build-dir out\build\ninja-verify --spec-delay 8   :: mid-match join
```

## Lint / quality gates

```
make module-deps env-check docs-check retired-paths-check cvar-check gen-headers-check
python tools/analysis/lint/check_*.py            :: individual scripts also runnable directly
make lobby-test                          :: 21 security checks + 400-iter lobby fuzz
python tests/integration/assets/extraction_recipes_test.py   :: TD-006 golden
```

## Codegen

```
make generate-yamls      :: regenerate assets/game/<v>/ + reloc yaml tables
make codegen             :: CMake-managed generators (reloc tables, headers)
make codegen-check       :: fails if regenerating changes the tree (CI gate)
```

## Makefile targets (facade, current)

`setup` `doctor` `info` `configure` `build` `rebuild` `run` `test`
`test-suite` `lint` `format` `format-check` `security` `semgrep`
`dependency-audit` `sbom` `release-binary-check` `package`
`package-windows` `package-linux` `package-macos` `package-android`
`package-uwp` `release-check` `release-dry-run` `android` `uwp`
`lobby-install` `lobby-test` `lobby-dev` `mods-test` `clean` `distclean`
`reset`

Not yet implemented (planned): `layout-check`, `verify-fast` / `check` /
`verify` tiers, `install-python` / `install-node`, `generated-check`,
`prune`, `artifacts`, `switch-*`.
