# Nintendo Switch

| Piece | Location |
|-------|----------|
| Runtime init + services | `src/platform/switch/` |
| CMake logic + libultraship backend gate | `cmake/platforms/Switch.cmake` |
| Build script | `scripts/build/build-switch.sh` → `out/build/switch/` |

Requires devkitPro (`DEVKITPRO`, `nacptool`, `elf2nro`). Configure
currently fails fast: the pinned libultraship fork has no `__SWITCH__`
backend — see SW-08 in `docs/investigations/deep_technical_audit_2026-10-14.md`.
