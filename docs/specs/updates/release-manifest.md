# Release manifest contract

Consumed by `src/application/update/integrity/ReleaseSig.cpp` +
`src/application/update/updater/Updater.cpp`:

- `version`, `artifacts[]` with `name`, `size`, `sha256`.
- Whole manifest signed with raw Ed25519 (`minisign -l`); public key
  compiled in via `BATTLESHIP_RELEASE_PUBKEY` from
  `config/release_pubkey.hex`.
- Client verifies signature BEFORE parsing, then size+sha256 per
  downloaded artifact.
