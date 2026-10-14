# Supported platforms

| Platform | ABI | Build path | Status |
|----------|-----|-----------|--------|
| Windows | x64 MSVC | `platforms/` n/a — CMake root, VS 2022 BuildTools | Primary |
| Linux | x86_64 | CMake + gcc/clang, AppImage packaging | Supported |
| macOS | arm64 | CMake + clang, DMG packaging | Supported |
| Android | arm64-v8a, armeabi-v7a (minSdk 24) | `platforms/android/` Gradle | Supported |
| UWP | x64 | `platforms/uwp/` | Maintained |

## Graphics backends

Fast3D exposes OpenGL, DirectX 11, and Metal (via libultraship). Backend
selection is a runtime capability probe, not a build constant — a backend
may be absent on a given platform without breaking the build. Widescreen,
hires textures, and frame interpolation are per-backend capabilities that
must degrade independently.

## Netplay parity

Netplay is platform-agnostic at the protocol level (all wire fields via
explicit `syNetPeerWriteU*`/`ReadU*`, endian-stable). Cross-platform
matches additionally depend on identical simulation behavior across
libm/libc — resolved by compiling the decomp's own Cody-Waite trig
(`docs/bugs/README.md` → `trig_host_libm_divergence`).

## Deprecation rule

Dropping a platform/backend requires: a note in CHANGELOG, one release
with a deprecation warning where feasible, and removal of the CI job only
after the deprecation release ships.
