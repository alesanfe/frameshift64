# src/distribution — update + content delivery infrastructure

Everything that moves bytes from the internet to the install —
the machinery that *enables* features, not features themselves.

- `Updater.cpp` — self-update check/download/install (desktop only;
  Android/UWP excluded in CMakeLists).
- `ShaderDownloader.cpp`, `ModPackDownloader.cpp` — curated pack
  install via `platform/io/download_service` + libzip audit + pinned
  SHA-256.
- `ReleaseSig.*` — Ed25519 manifest verification (orlp/ed25519).
- `FileHash.*` — in-process SHA-256.

## Allowed dependencies

`observability`, `platform` (io), `resources`, `integration`,
`features`. Never `application`/`presentation` — the menu calls in
through `features/enhancements.h`.
