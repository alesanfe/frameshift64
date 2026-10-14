# ADR 0003 — All generated output under `out/`

**Status:** accepted (2026-10)

Build trees, lint reports, coverage, packages and logs live under `out/` (gitignored): `out/build/<preset>`, `out/reports/{lint,cppcheck,semgrep,tests,coverage}`, `out/packages`, `out/logs`.

- `f3d.o2r` is generated into `${CMAKE_BINARY_DIR}` (was: source root).
- `generated/include/*/reloc_data.*.h` remain committed source-adjacent codegen (CI builds without a ROM); everything else generated goes to `out/` or the binary dir.
- `CMakePresets.json` presets (`windows-debug`, `linux-release`, `lint`, …) write to `out/build/<preset>` so local and CI layouts match.
- User-provided ROMs may live at `roms/` (gitignored) or root `baserom.<ver>.z64` for backward compat.
