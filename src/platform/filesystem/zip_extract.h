/* zip_extract.h — in-process .zip extraction via libzip.
 *
 * libzip is already a libultraship dependency on every shipped target
 * (vcpkg/system on desktop, FetchContent on Android), so extracting
 * through it replaces the per-OS `unzip` / `bsdtar` / `tar` shell-outs
 * that made the shader-pack downloader desktop-only. */
#pragma once

#include <filesystem>

namespace ssb64 {

/* Extract every regular file in `zipPath` under `destDir`, preserving
 * the archive's relative layout. Entries with absolute paths or `..`
 * segments are skipped (zip-slip guard). Returns false on a malformed
 * archive or any file that fails to write — callers treat the whole
 * extraction as failed in that case. */
bool ZipExtractToDir(const std::filesystem::path& zipPath,
                     const std::filesystem::path& destDir);

} // namespace ssb64
