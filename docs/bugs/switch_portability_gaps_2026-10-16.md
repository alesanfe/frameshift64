# Switch port — desktop-only assumptions in shared code

**Status:** FIXED — `BattleShip.nro` links and `BundleSwitch` produces a working `switch_sd/` drop-in (devkitA64 r30, libnx 4.12, Ubuntu 24.04 container).

## Symptoms

Cross-compiling for `NintendoSwitch` (devkitPro toolchain) surfaced a chain of failures, each from a desktop assumption leaking into shared code:

- `find_package`/vendored deps absent from dkp repos (`switch-spdlog`, `switch-nlohmann-json`, `switch-libzip` don't exist — they come via FetchContent like Android).
- hidapi configured its libusb backend (nonexistent on Switch); GCAdapter code needed `libusb.h`.
- ImGui could not see `GLES3/gl3.h` — devkitPro's toolchain file only injects `libnx/include`, not `portlibs/switch/include`, and `include_directories()` only covers targets created *after* it.
- `<dlfcn.h>`/`dladdr`, `mincore`, `_exit`, the Discord SDK, the curl self-updater, and the Torch first-run wizard all have no Switch equivalent.
- libnx `switch.h` typedefs (`s64`, `u64`, ...) collide with `libultra/types.h` — including it anywhere libultra types are visible is a hard error.
- A truncated FetchContent clone (`json.hpp` = 0 bytes after a disk-full crash) surfaced as "`nlohmann` does not name a type" despite a correct `-I` — always suspect dep-tree corruption after I/O failures.

## Fixes

- `components/libultraship/cmake/dependencies/switch.cmake` — new dep file: portlibs SDL2/tinyxml2 via `find_package`, spdlog/nlohmann_json/libzip via FetchContent, hidapi stub (same approach as Android), `USE_OPENGLES`, `IMGUI_IMPL_OPENGL_ES3`, ZLIB, `libzip::zip` alias; portlibs include attached directly to the `imgui` target.
- `components/libultraship/src/fast/interpreter.cpp` — `gfxPointerHasReadableBytes()` gained a `__SWITCH__` branch using `svcQueryMemory` (the libnx equivalent of `VirtualQuery`/`mincore`) with locally-declared `MemoryInfo`/prototype — `switch.h` cannot be included (typedef collision), so the ABI is declared manually. `<dlfcn.h>` module-detection excluded; returns false (no dlopen mods on Switch).
- `components/libultraship` `os.cpp`, controller managers — `__SWITCH__` backends from the upstream fork adapted onto this tree; Joy-Con pair routing merged with our first-free-port auto-routing.
- `src/application/update/updater/UpdaterStubs.cpp` — guard extended to `__SWITCH__` (inert updater, same as UWP/Android).
- `src/application/port_switch_stubs.cpp` — NEW. No-op `ExtractAssetsIfNeeded`/`RunFirstRunWizard`/`SetWindowIcon`. Assets ship pre-extracted next to the `.nro`; if `BattleShip.o2r` is missing the stub path exits cleanly. Stubs (not `#if` at call sites) were chosen because call sites live in actively-edited menu/init code — link-time symbols can't be lost to a guard revert.
- `CMakeLists.txt` — Switch exclusion block mirroring UWP: `Updater.cpp`, `ShaderDownloader.cpp`, `ModPackDownloader.cpp`, `DiscordRichPresence.cpp`, `port_window_icon.cpp`, `first_run.cpp` all out of `SSB64_SRC_PORT`.
- `src/application/port.cpp`, `PortMenu*.cpp` — `__SWITCH__` guards where call sites gate behavior (shader-pack downloader UI hidden; local post-process shader picker kept — it works over GLES).
- `src/observability/port_log.c` — Switch exit branch uses `svcExitProcess()` (libnx), not POSIX `_exit`.

## Verification

`scripts/build/build-switch.sh --skip-extract` inside an Ubuntu 24.04 container with devkitA64 r30-1 (the pinned toolchain needs glibc ≥ 2.36; Ubuntu 22.04's 2.35 fails at `__isoc23_strtol`). Produces `BattleShip.nro` (13.8 MB) + `switch_sd/switch/BattleShip/{BattleShip.nro,BattleShip.o2r,f3d.o2r,gamecontrollerdb.txt}`.

## Audit hooks

- New dep fails its `find_package` on Switch → check `switch.cmake` before patching call sites; portlibs CMake configs live under `$DEVKITPRO/portlibs/switch/lib/cmake`.
- "`X does not name a type" with a correct `-I` → suspect a truncated FetchContent tree after disk-full or interrupted clone.
- `include_directories()` silently does nothing for targets created earlier in the same directory — use `target_include_directories`.
- Desktop-only API missing on Switch → prefer a `switch_stubs`-style no-op over guarding call sites in files under active edit.
