# src/distribution — update + content delivery infrastructure

Everything that moves bytes from the internet to the install —
the machinery that *enables* features, not features themselves.

- `updater/Updater.cpp` — self-update check/download/install (desktop
  only; Android/UWP excluded in CMakeLists).
- `downloads/ShaderDownloader.cpp`, `downloads/ModPackDownloader.cpp` —
  curated pack install via `platform/http/download_service` + libzip
  audit + pinned SHA-256.
- `integrity/ReleaseSig.*` — Ed25519 manifest verification
  (orlp/ed25519).
- `integrity/FileHash.*` — in-process SHA-256.

## Allowed dependencies

`observability`, `platform` (http/filesystem), `resources`,
`integration`, `gameplay`. Never `application`/`presentation` — the
menu calls in through `gameplay/enhancements.h`.
