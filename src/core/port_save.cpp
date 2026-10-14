#include "port_save.h"
#include "port_log.h"
#include "smash_classic/version.h"

#include <libultraship/libultraship.h>
#include <SDL2/SDL.h>

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

    // A sidecar from a NEWER format version means the image was written by
    // a build whose SRAM layout we don't know. Reading is still attempted
    // (flat image), but this must be loud in logs for support.
    if (FILE *f = std::fopen(metaPath().c_str(), "rb")) {
        char buf[256] = {0};
        std::fread(buf, 1, sizeof(buf) - 1, f);
        std::fclose(f);
        unsigned int fmt = 0;
        if (std::sscanf(buf, "format_version=%u", &fmt) == 1 &&
            fmt > SMASH_SAVE_FORMAT_VERSION) {
            port_log("SSB64 Save: WARNING — save written by newer format v%u "
                     "(this build understands v%u); fields may not round-trip\n",
                     fmt, SMASH_SAVE_FORMAT_VERSION);
        }
    }
}

void writeSaveMeta()
{
    const std::string path = metaPath();
    FILE *f = std::fopen(path.c_str(), "wb");
    if (f == NULL) {
        port_log("SSB64 Save: could not write meta sidecar %s\n", path.c_str());
        return;
    }
    std::fprintf(f,
                 "format_version=%u\ngame_version=%s\nsram_size=0x%x\n",
                 SMASH_SAVE_FORMAT_VERSION, SMASH_CLASSIC_APP_VERSION_STR,
                 (unsigned)PORT_SAVE_SIZE);
    std::fclose(f);
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
    static std::string dir;
    dir = std::filesystem::path(savePath()).parent_path().string();
    if (dir.empty()) {
        dir = ".";
    }
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

    const std::string path = savePath();
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
    std::fclose(f);

    if (wrote != img.size() || flush_rc != 0) {
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
