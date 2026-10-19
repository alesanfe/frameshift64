#include "port_save.h"
#include "port_log.h"
#include "smash_classic/version.h"

#include <libultraship/libultraship.h>
#include <SDL2/SDL.h>

#include <span>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <io.h>      /* _commit, _fileno */
#else
#include <unistd.h>  /* fsync (POSIX + libnx) */
#endif

// Where the SRAM file lives, in priority order:
//
//   1. $SSB64_SAVE_PATH                  — explicit override (debug / CI).
//   2. Ship::Context::GetPathRelativeToAppDirectory("ssb64_save.bin")
//      — same convention as BattleShip.o2r and BattleShip.cfg.json:
//         * NON_PORTABLE=ON                                 → OS app-data dir
//             macOS    ~/Library/Application Support/BattleShip/
//             Linux    $XDG_DATA_HOME/BattleShip/  (or ~/.local/share/...)
//             Windows  %APPDATA%\BattleShip\
//         * NON_PORTABLE=OFF                                → cwd (portable)
//         * SHIP_HOME=<dir>                                 → that dir wins on macOS/Linux
//   3. SDL_GetPrefPath fallback                              — only if Ship::Context
//                                                             hasn't been constructed yet
//                                                             (shouldn't happen — SRAM
//                                                             I/O runs after PortInit —
//                                                             but defensive).
//   4. "ssb64_save.bin" in cwd                               — last-resort.

namespace {

std::once_flag gPathOnce;
std::string gSavePath;

void resolveSavePath()
{
    if (const char *override = std::getenv("SSB64_SAVE_PATH")) {
        gSavePath = override;
        return;
    }
    try {
        // Mirrors how BattleShip.o2r / BattleShip.cfg.json / logs/*.log
        // are located. Honors SHIP_HOME and the NON_PORTABLE build flag.
        gSavePath = Ship::Context::GetPathRelativeToAppDirectory("ssb64_save.bin");
        if (!gSavePath.empty()) {
            return;
        }
    } catch (...) {
        // Ship::Context not yet alive — fall through.
    }
    if (char *p = SDL_GetPrefPath(NULL, "BattleShip")) {
        gSavePath = std::string(p) + "ssb64_save.bin";
        SDL_free(p);
        return;
    }
    gSavePath = "ssb64_save.bin"; // last-resort cwd
}

const std::string &savePath()
{
    std::call_once(gPathOnce, resolveSavePath);
    return gSavePath;
}

// The SRAM image itself is a flat 32 KiB chip mirror — no header, because
// every game offset is absolute. Format/version metadata lives in a
// `ssb64_save.meta` sidecar (SMASH_SAVE_FORMAT_VERSION bumps when the
// *layout* of that image changes).
std::once_flag gMetaCheckOnce;
std::once_flag gMetaWriteOnce;

void writeSaveMeta();   /* defined below; migrations re-stamp via this */

std::string metaPath()
{
    // replace_extension operates on the filename only — the old
    // find_last_of('.') scanned the whole path, so a save path without
    // an extension inside a dotted directory ("C:\foo.bar\ssb64_save")
    // wrote the sidecar into the parent directory.
    std::filesystem::path p = savePath();
    p.replace_extension(".meta");
    return p.string();
}

/* Migration chain: one-step upgraders indexed by the version they upgrade
 * FROM. A v1→v2 migrator lives at kMigrations[0], etc. Each step receives
 * the image path and may rewrite it in place (through the same temp+rename
 * discipline as port_save_write); the sidecar is re-stamped after the
 * chain completes. Empty today — the first format change lands its
 * migrator here rather than growing ad-hoc branches in the read path. */
struct SaveMigrator {
    unsigned int from_version;
    int (*apply)(const char *image_path);   /* 0 on success */
};
/* MSVC rejects zero-size arrays (C2466) — a default-constructed span is
 * the portable empty table. When the first migrator lands, declare the
 * backing array and point the span at it:
 *   constexpr SaveMigrator kMigrationList[] = { {1, migrate_v1_to_v2} };
 *   constexpr std::span<const SaveMigrator> kMigrations{kMigrationList}; */
constexpr std::span<const SaveMigrator> kMigrations{};

int runSaveMigrations(unsigned int stored_version)
{
    unsigned int v = stored_version;
    while (v < SMASH_SAVE_FORMAT_VERSION) {
        const SaveMigrator *step = nullptr;
        for (const SaveMigrator &m : kMigrations) {
            if (m.from_version == v) { step = &m; break; }
        }
        if (step == nullptr) {
            port_log("SSB64 Save: no migrator for format v%u (target v%u) — "
                     "reading flat image unconverted; new-format fields "
                     "will read as zero\n",
                     v, SMASH_SAVE_FORMAT_VERSION);
            return -1;
        }
        port_log("SSB64 Save: migrating format v%u -> v%u\n", v, v + 1);
        if (step->apply(savePath().c_str()) != 0) {
            port_log("SSB64 Save: migration v%u -> v%u FAILED — leaving "
                     "image at v%u\n", v, v + 1, v);
            return -1;
        }
        v++;
    }
    return 0;
}

void checkSaveFormat()
{
    // Wrong size = corrupt or truncated file. We still read (offsets are
    // zero-filled past EOF) but the user-facing log makes it diagnosable.
    std::error_code ec;
    uintmax_t sz = std::filesystem::file_size(savePath(), ec);
    if (!ec && sz != PORT_SAVE_SIZE) {
        port_log("SSB64 Save: %s is %llu bytes, expected %u — treating as "
                 "fresh SRAM (offsets past EOF read as zero)\n",
                 savePath().c_str(), (unsigned long long)sz,
                 (unsigned)PORT_SAVE_SIZE);
    }

    if (FILE *f = std::fopen(metaPath().c_str(), "rb")) {
        char buf[256] = {0};
        std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        unsigned int fmt = 0;
        if (std::sscanf(buf, "format_version=%u", &fmt) != 1) {
            return;
        }
        if (fmt > SMASH_SAVE_FORMAT_VERSION) {
            // A sidecar from a NEWER format version means the image was
            // written by a build whose SRAM layout we don't know. Reading
            // is still attempted (flat image), but this must be loud.
            port_log("SSB64 Save: WARNING — save written by newer format v%u "
                     "(this build understands v%u); fields may not round-trip\n",
                     fmt, SMASH_SAVE_FORMAT_VERSION);
        } else if (fmt < SMASH_SAVE_FORMAT_VERSION) {
            if (runSaveMigrations(fmt) == 0) {
                /* Re-stamp at the migrated version — via the write-once
                 * flag so port_save_write doesn't emit a second copy. */
                std::call_once(gMetaWriteOnce, writeSaveMeta);
            }
        }
    }
}

void writeSaveMeta()
{
    const std::string path = metaPath();
    /* Same temp+rename discipline as the image: a crash mid-sidecar used to
     * be able to leave a truncated (and misleading) format_version. */
    const std::string tmp = path + ".tmp";
    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == NULL) {
        port_log("SSB64 Save: could not write meta sidecar %s\n", path.c_str());
        return;
    }
    std::fprintf(f,
                 "format_version=%u\ngame_version=%s\nsram_size=0x%x\n",
                 SMASH_SAVE_FORMAT_VERSION, SMASH_CLASSIC_APP_VERSION_STR,
                 (unsigned)PORT_SAVE_SIZE);
    std::fflush(f);
    std::fclose(f);
#ifdef _WIN32
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        port_log("SSB64 Save: meta replace failed (%lu)\n", (unsigned long)GetLastError());
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
    }
#else
    {
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            port_log("SSB64 Save: meta rename failed: %s\n", ec.message().c_str());
            std::filesystem::remove(tmp, ec);
        }
    }
#endif
}

// Sticky "last write failed" flag — the decomp's syDmaWriteSram wrapper
// discards port_save_write's return value, so the game layer can't see
// failures directly. PortPushFrame polls this once per frame and surfaces
// it as a GameOverlay notification (AUD-16).
std::atomic<bool> gWriteFailed{false};

} // namespace

extern "C" int port_save_write_failed(void)
{
    return gWriteFailed.load() ? 1 : 0;
}

extern "C" void port_save_clear_write_failed(void)
{
    gWriteFailed.store(false);
}

extern "C" const char *port_save_get_path(void)
{
    return savePath().c_str();
}

extern "C" const char *port_save_get_dir(void)
{
    /* Memoized like savePath(): re-resolving per call would race the
     * static string and could hand callers a stale c_str() pointer. */
    static std::once_flag sDirOnce;
    static std::string dir;
    std::call_once(sDirOnce, [] {
        dir = std::filesystem::path(savePath()).parent_path().string();
        if (dir.empty()) {
            dir = ".";
        }
    });
    return dir.c_str();
}

extern "C" int port_mkdir_p(const char *path)
{
    try {
        std::filesystem::create_directories(path);
        return 0;
    } catch (...) {
        port_log("SSB64 Save: mkdir(%s) failed\n", path);
        return -1;
    }
}

extern "C" int port_save_read(uintptr_t offset, void *dst, size_t size)
{
    if (size == 0) return 0;
    std::memset(dst, 0, size);

    std::call_once(gMetaCheckOnce, checkSaveFormat);

    FILE *f = std::fopen(savePath().c_str(), "rb");
    if (f == NULL) {
        return 0; // missing file -> zero-filled, treated as fresh SRAM
    }
    if (std::fseek(f, (long)offset, SEEK_SET) != 0) {
        std::fclose(f);
        return 0; // short file -> zero-filled
    }
    (void)std::fread(dst, 1, size, f);
    std::fclose(f);
    return 0;
}

extern "C" int port_save_write(uintptr_t offset, const void *src, size_t size)
{
    if (size == 0) return 0;

    std::call_once(gMetaCheckOnce, checkSaveFormat);

    const std::string& path = savePath();
    const std::string tmp  = path + ".tmp";

    /* Read-modify-write the whole image through a temp file + rename. The
     * old in-place seek+fwrite could leave a truncated ssb64_save.bin if
     * the process died mid-write — and the DMA wrappers ignore the return
     * value, so the corruption was also silent. With rename, the previous
     * file stays intact until the new one is fully on disk. */
    std::vector<char> img;
    {
        std::error_code ec;
        uintmax_t cur = std::filesystem::file_size(path, ec);
        if (!ec && cur > 0) {
            img.resize((size_t)cur);
        }
        if (!img.empty()) {
            if (FILE *f = std::fopen(path.c_str(), "rb")) {
                (void)std::fread(img.data(), 1, img.size(), f);
                std::fclose(f);
            } else {
                img.clear();
            }
        }
    }
    const size_t end = (size_t)offset + size;
    if (img.size() < end) {
        img.resize(end, 0);   /* zero-pad gap like the old path did */
    }
    std::memcpy(img.data() + offset, src, size);

    FILE *f = std::fopen(tmp.c_str(), "wb");
    if (f == NULL) {
        port_log("SSB64 Save: open(%s) for write failed\n", tmp.c_str());
        gWriteFailed = true;
        return -1;
    }
    size_t wrote = std::fwrite(img.data(), 1, img.size(), f);
    int flush_rc = std::fflush(f);
    /* Durability, not just ordering: fflush only pushes to the OS page
     * cache — a power loss between fflush and rename could still drop the
     * new image. Force it to stable storage before the atomic replace. */
    int sync_rc = 0;
#ifdef _WIN32
    sync_rc = _commit(_fileno(f));
#else
    sync_rc = fsync(fileno(f));
#endif
    std::fclose(f);

    if (wrote != img.size() || flush_rc != 0 || sync_rc != 0) {
        port_log("SSB64 Save: short write (%zu/%zu) to %s\n",
                 wrote, img.size(), tmp.c_str());
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        gWriteFailed = true;
        return -1;
    }

    /* Atomic replace: rename() is atomic on POSIX. On Windows plain rename
     * refuses to overwrite, so go through MoveFileEx. */
#ifdef _WIN32
    if (!MoveFileExA(tmp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        port_log("SSB64 Save: replace %s -> %s failed (%lu)\n",
                 tmp.c_str(), path.c_str(), (unsigned long)GetLastError());
        gWriteFailed = true;
        return -1;
    }
#else
    {
        std::error_code ec;
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            port_log("SSB64 Save: rename %s -> %s failed: %s\n",
                     tmp.c_str(), path.c_str(), ec.message().c_str());
            gWriteFailed = true;
            return -1;
        }
    }
#endif
    std::call_once(gMetaWriteOnce, writeSaveMeta);
    return 0;
}
