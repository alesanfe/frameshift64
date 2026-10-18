# ADR 0003 — All generated output under `out/`

**Status:** accepted (2026-10)
**Links:** `generated/README.md`, `docs/architecture/build_and_tooling.md`, ADR 0001

Build trees, lint reports, coverage, packages and logs live under `out/` (gitignored): `out/build/<preset>`, `out/reports/{lint,cppcheck,semgrep,tests,coverage}`, `out/packages`, `out/logs`.

- `f3d.o2r` is generated into `${CMAKE_BINARY_DIR}` (was: source root).
- ROM-free codegen (`reloc_data.<ver>.h`, `RelocFileTable.<ver>.*`) is emitted to `<build>/generated/`; only `generated/src/<ver>/ssb64_reloc_rebuild.<ver>.cpp` stays committed because its generator needs a baserom (see `generated/README.md`). Everything else generated goes to `out/` or the binary dir.
- `CMakePresets.json` presets (`windows-debug`, `linux-release`, `lint`, …) write to `out/build/<preset>` so local and CI layouts match.
- User-provided ROMs may live at `roms/` (gitignored) or root `baserom.<ver>.z64` for backward compat.
