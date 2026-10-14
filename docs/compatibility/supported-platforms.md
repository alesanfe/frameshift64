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
simulation, all `src/features/` enhancements, netplay/rollback, hires
packs, UI. The gated entries below diverge for *dependency* reasons, not
preference; each row names the real blocker and what unifying would cost.

| Feature | Win | Linux | macOS | Android | UWP | Switch | Blocker |
|---------|-----|-------|-------|---------|-----|--------|---------|
| Mods (TinyCC + funchook) | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | funchook/TinyCC have no Android/UWP/Switch target (`DISABLE_SCRIPTING`); UWP forbids exec-memory outright |
| In-app updater | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | shells out to `curl`; Android APKs update via package install, UWP via MSIX |
| Shader-pack downloader | ✔ | ✔ | ✔ | ✔ | ✘ | ✘ | unified: HTTP via `HttpDownloadToFile` (curl on desktop, HttpURLConnection/JNI on Android) + libzip extraction on all; UWP ships packaged shaders |
| Discord Rich Presence | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | no discord-rpc target for Android/UWP/Switch |
| Windowed-fullscreen toggle | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | Android is always fullscreen; meaningless on UWP/Switch |
| GameCube adapter (libusb) | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | libusb host access needs desktop USB stack (WinUSB driver on Windows) |
| First-run ROM wizard | ✔ | ✔ | ✔ | ✔* | ✘ | ✘ | *Android extracts on-device via `libtorch_runner.so`; UWP/Switch ship pre-extracted assets |
| RenderDoc integration | ✔ | ✔ | ✔ | ✘ | ✘ | ✘ | desktop dev tooling only |

Gating mechanism: `src/features/{Updater,DiscordRichPresence,ModPackDownloader}.cpp`
are filtered out of `SSB64_SRC_PORT` for Android/UWP in CMakeLists, and the
corresponding menu entries use `#if` guards — never both (they'd drift).
ShaderDownloader compiles on Android: its transport layer is
`src/platform/http_download.cpp` (JNI→HttpURLConnection) and extraction is
`src/platform/zip_extract.cpp` (libzip, already a LUS dep everywhere). Mods
gate on `DISABLE_SCRIPTING` everywhere.

Note: `android.permission.INTERNET` + `ACCESS_NETWORK_STATE` are declared in
the manifest — required for both netplay sockets and the downloader.

### Unification paths, if ever wanted

- **Mod-pack downloader on Android**: same shape as the shader downloader —
  it still shells out to a killable `curl` child for progress/cancel on
  ~200 MB transfers, so it stays desktop-only until the JNI path grows a
  cancel/progress callback.
- **Mods on Android**: needs an arm64 detour backend (funchook has partial
  arm64 support upstream) and a JIT-able mod compiler path (TCC has an
  aarch64 backend; Android also allows `PROT_EXEC` on app-private pages).
  Real work, not a flag flip — treat as its own milestone.
- **Updater on Android**: do not unify — self-updaters violate the APK
  distribution model regardless of store policy.
