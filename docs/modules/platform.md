# src/platform — OS abstraction, no product knowledge

| Subdir | Contents |
|--------|----------|
| `coroutine/` | `coroutine.h` + per-OS fibers (win32/posix/android/switch + aarch64/armv7 asm). Used by rollback resimulation. |
| `io/`        | `http_download`, `download_service` (cancellable transfers: curl child on desktop, JNI HttpURLConnection on Android), `zip_extract` (libzip). |
| `android/`   | torch_runner bridge + touch overlay. |
| `switch/`    | SwitchImpl + AArch64 context-switch asm. |

## Allowed dependencies

`observability` only (`port_watchdog.h` consumption is tracked debt in
`config/lint/module-deps-baseline.txt` — port_watchdog lives in
`application/` but is really a platform-facing header).
