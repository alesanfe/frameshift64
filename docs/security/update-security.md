# Update security

The updater (`src/distribution/updater/Updater.cpp`) downloads release artifacts. Until
signature verification lands (TD-009), the trust chain is GitHub Releases
over TLS — sufficient for integrity against random corruption, not against
a compromised release.

## Requirements (target)

- Signed manifest or per-artifact SHA-256 published by the release
  workflow (hashes generated in CI, not on a dev machine).
- The updater verifies the downloaded package hash before touching the
  install; a mismatch aborts and keeps the current version.
- Rollback: the updater keeps the previous binary until the new one is
  verified installed and launches once.
- `in-toto`/SLSA attestations where the platform supports them (release
  workflow already uses `actions/attest`).
- No auto-update for the lobby service (it is infrastructure, deployed
  via wrangler under maintainer credentials).

## User-visible contract

- Updates are opt-in (prompt), never silent.
- The prompt shows version + source (releases page URL).
- A failed verification leaves the current install untouched and shows
  the checksum mismatch.
