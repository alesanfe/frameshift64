/* FileHash.h — shared download-integrity helpers.
 *
 * Single home for the size + SHA-256 verification gate used by both the
 * auto-updater (Updater.cpp) and the curated content-pack downloader
 * (ModPackDownloader.cpp). AUD-32 introduced the gate in the updater;
 * extracting it here keeps every network payload on the same check
 * instead of letting each downloader grow its own variant.
 *
 * Digest-source caveat (TD-009): the updater feeds this gate a digest from
 * the minisign-verified SHA256SUMS.txt when a release pubkey is embedded
 * (see ReleaseSig.cpp); without one it falls back to the API-reported
 * digest — integrity, not publisher authenticity. ModPackDownloader pins
 * its digest at compile time instead. */
#pragma once

#include <string>

namespace ssb64::enhancements {

/* Lowercase hex SHA-256 of a file, or "" on error. Windows uses CNG
 * (bcrypt); POSIX shells out to sha256sum (Linux) / shasum (macOS). */
std::string Sha256File(const std::string& path);

/* Gate a just-downloaded file on its expected byte size and — when the
 * digest is in GitHub's "sha256:<hex>" form — its SHA-256. `error` gets
 * a user-facing reason on failure. Returns true when everything checks
 * out (or when there is no digest to compare against). */
bool DownloadedFileMatches(const std::string& path, long long expectedSize,
                           const std::string& expectedDigest, std::string& error);

} // namespace ssb64::enhancements
