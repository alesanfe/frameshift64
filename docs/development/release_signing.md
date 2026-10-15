# Release manifest signing (TD-009)

The updater downloads `SHA256SUMS.txt` **and** `SHA256SUMS.txt.minisig`
from each release and verifies the manifest's Ed25519 signature *before*
trusting any digest in it. This is what turns the sha256 gate from a
corruption check into publisher authentication: a forged release would
need the offline signing key, not just GitHub API access.

- **Signer:** `minisign -l` (raw/legacy mode — plain RFC 8032 Ed25519 over
  the file bytes, no Blake2b prehash, so the client needs no extra hash).
- **Verifier:** `src/application/update/integrity/ReleaseSig.cpp` + vendored orlp/ed25519
  (FetchContent, pinned to `b1f19fab…`, verify-only — 5 C files).
- **Public key:** embedded at compile time via `BATTLESHIP_RELEASE_PUBKEY`,
  sourced from `config/release_pubkey.hex` (committed — it is a *public*
  key). Builds without the file log a warning and fall back to the
  unauthenticated API digest.
- **Private key:** lives only in the `MINISIGN_SECRET_KEY` GitHub secret,
  used by the release workflow. Never committed.

## One-time setup

```bash
# On an offline/trusted machine:
minisign -G -W -p release.pub -s release.key   # -W: no password (CI signs unattended)

python tools/release/make_pubkey_hex.py release.pub > config/release_pubkey.hex
git add config/release_pubkey.hex && git commit -m "Embed release signing pubkey"
```

Then in GitHub → repo **Settings → Secrets and variables → Actions**,
create `MINISIGN_SECRET_KEY` containing the **full contents of
`release.key`** (the whole file, comment lines included).

Keep `release.key` somewhere safe offline (password manager / hardware
token). If it is lost, generate a new pair, update the secret, and ship a
release whose binaries embed the new pubkey.

## What happens on release

`.github/workflows/release.yml` (publish job):

1. `SHA256SUMS.txt` is generated over every artifact.
2. If `MINISIGN_SECRET_KEY` is set, `minisign -Sl` produces
   `SHA256SUMS.txt.minisig` (otherwise the release goes out unsigned —
   a `::warning::` is emitted in the log).
3. Both files land as release assets; attestations and the SBOM ride
   alongside as before.

## What the updater does

1. The release-asset scan grabs `SHA256SUMS.txt` / `.minisig` URLs
   (same trust gate as the payload URL).
2. After the platform artifact downloads, both manifest files download,
   `VerifyReleaseSignature()` checks `sig_alg == 'Ed'` and the Ed25519
   signature over the manifest bytes with the embedded pubkey.
3. `SumsDigestForAsset()` extracts the authoritative digest for this
   platform's asset; the size+sha256 check runs against *that* digest —
   the API-reported `asset.digest` is ignored once signing is enabled.
4. Any failure (missing manifest, bad signature, missing entry) fails
   closed: the update is refused.

## Key rotation

1. Generate a new pair (`minisign -G -W …`).
2. Update `MINISIGN_SECRET_KEY` **and** `config/release_pubkey.hex` in the
   same release cycle — an updater with the old pubkey refuses manifests
   signed by the new key, and vice versa.
3. Users on old binaries stay safe: they keep verifying with the old
   embedded key until they update manually.
