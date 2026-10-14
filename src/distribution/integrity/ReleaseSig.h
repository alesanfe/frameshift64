/* ReleaseSig.h — offline-signed release-manifest verification (TD-009).
 *
 * The GitHub release ships SHA256SUMS.txt plus a minisign signature
 * (SHA256SUMS.txt.minisig, produced by `minisign -l`). The updater embeds
 * the release public key at build time (BATTLESHIP_RELEASE_PUBKEY) and
 * verifies the sums file's signature before trusting any digest in it —
 * closing the "digest travels with the payload" tamper hole where a forged
 * release could ship a malicious zip alongside its own digest.
 *
 * When the build has no embedded pubkey (dev builds), the functions report
 * verification as unavailable and callers fall back to the plain size +
 * digest check. */
#pragma once

#include <string>
#include <vector>

namespace ssb64::enhancements {

/* True when this binary carries an embedded release pubkey. */
bool ReleaseSignatureSupported();

/* Verify `minisig_text` (a minisign `.minisig` file) against `message`
 * using the embedded release pubkey. Requires legacy/raw-mode signatures
 * ('Ed'): `minisign -l`. Returns true only on a valid Ed25519 signature;
 * `error` carries a short reason on failure. */
bool VerifyReleaseSignature(const std::string& message, const std::string& minisig_text,
                            std::string& error);

/* Look up `asset_name` inside a `sha256sum`-style manifest
 * ("<64-hex>  <name>" per line). Returns the digest as "sha256:<hex>" —
 * the same shape GitHub's asset.digest uses — or "" when absent. */
std::string SumsDigestForAsset(const std::string& sums_text, const std::string& asset_name);

/* Verify `signature_b64` (base64 of a minisign SigStruct — 'Ed' raw mode,
 * 74 bytes) over `message` with `pubkey_b64` (base64 of a minisign
 * PubkeyStruct — 42 bytes: alg + keynum + 32-byte key). Unlike
 * VerifyReleaseSignature the key is caller-supplied — used for mod
 * manifests, where each mod ships its own identity. Does NOT need
 * BATTLESHIP_RELEASE_PUBKEY (the ed25519 lib is always linked). */
bool VerifyEd25519MinisigFields(const std::string& message,
                                const std::string& signature_b64,
                                const std::string& pubkey_b64,
                                std::string& error);

} // namespace ssb64::enhancements
