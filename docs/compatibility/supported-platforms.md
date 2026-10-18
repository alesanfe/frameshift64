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

## Feature parity matrix

Everything not listed here is identical across platforms — same decomp
simulation, all `src/game/gameplay/` enhancements, netplay/rollback, hires
packs, UI. The gated entries below diverge for *dependency* reasons, not
preference; each row names the real blocker and what unifying would cost.

| Feature | Win | Linux | macOS | Android | UWP | Switch | Blocker |
|---------|-----|-------|-------|---------|-----|--------|---------|
| Mods (TinyCC + funchook) | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | funchook/TinyCC have no Android/UWP/Switch target (`DISABLE_SCRIPTING`); UWP forbids exec-memory outright. Long-term path is a portable mod backend (declarative/WASM/Lua) — not a port of the native detour model |
| In-app updater | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | desktop replaces the binary in-place; Android can only do assisted installs (Play In-App Updates via Play Core when Play-distributed, or `PackageInstaller` session for sideloaded APKs — user confirmation required). Unifiable behind an `IUpdateService` interface, not yet implemented |
| Shader-pack downloader | ✔ | ✔ | ✔ | ✔ | ✘ | ✘ | unified: HTTP via `HttpDownloadToFile` (curl on desktop, HttpURLConnection/JNI on Android) + libzip extraction on all; UWP ships packaged shaders |
| Curated pack downloads | ✔ | ✔ | ✔ | ✔ | ✘ | ✘ | unified: `DownloadService` (killable curl child on desktop, HttpURLConnection task on Android) + in-process SHA-256 + libzip audit on all |
| Discord Rich Presence | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | in-tree client for Discord's local IPC (`discord-ipc-N` pipe/unix socket — no SDK dependency). The Discord Social SDK officially supports Android ≥7 — an `IDiscordIntegration` backend could unify this; not yet integrated |
| Windowed-fullscreen toggle | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | Android is always fullscreen; meaningless on UWP/Switch |
| GameCube adapter (libusb) | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | libusb host access needs desktop USB stack (WinUSB driver on Windows). Android USB-host API could offer experimental support on OTG-capable devices; not implemented |
| First-run ROM wizard | ✔ | ✔ | ✔ | ✔* | ✘ | ✘ | *Android extracts on-device via `libtorch_runner.so`; UWP/Switch ship pre-extracted assets |
| RenderDoc integration | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | desktop dev tooling only |

Gating mechanism: `src/application/update/updater/Updater.cpp` and `src/networking/social/DiscordRichPresence.cpp` are
filtered out of `SSB64_SRC_PORT` for Android in CMakeLists (and both
downloaders plus `first_run.cpp` for UWP), and the corresponding menu
entries use `#if` guards — never both (they'd drift). Both downloaders
compile on Android: transport is `src/platform/http/http_download.cpp` +
`src/platform/http/download_service.cpp` (JNI→HttpURLConnection), extraction is
`src/platform/filesystem/zip_extract.cpp` (libzip, already a LUS dep everywhere), and
integrity is the in-process SHA-256 in `src/application/update/integrity/FileHash.cpp`. Mods
gate on `DISABLE_SCRIPTING` everywhere.

Note: `android.permission.INTERNET` + `ACCESS_NETWORK_STATE` are declared in
the manifest — required for both netplay sockets and downloads.

### Unification paths, if ever wanted

- **Updater on Android**: an `IUpdateService` interface with a desktop impl
  (current binary replacement) and two Android impls — Play In-App Updates
  (Play Core native SDK, requires Play distribution) or a `PackageInstaller`
  assisted-install session for GitHub-distributed APKs. The latter needs
  `REQUEST_INSTALL_PACKAGES` + a FileProvider for the downloaded APK.
  Silent installs are impossible outside managed devices either way.
- **Discord on Android**: Discord Social SDK has official Android support
  (C++/Unity/Unreal). Add an `IDiscordIntegration` interface with the
  in-tree local-IPC backend on desktop and a Social SDK backend on
  Android; the presence model (`state/details/party/timestamps`) is shared.
- **Mods on Android**: the interesting parity target is not porting
  funchook/TCC — it is a portable mod backend (declarative packs +
  WASM/Lua scripting behind a semantic hook API). The manifest already
  separates `simulation` mods (netplay-hashed) from cosmetic ones, which
  is the compatibility contract a portable backend would reuse.
- **GC adapter on Android**: Android USB-host API can reach the adapter on
  OTG-capable devices (Dolphin does this) — experimental tier, behind the
  same controller-backend interface as SDL pads.
