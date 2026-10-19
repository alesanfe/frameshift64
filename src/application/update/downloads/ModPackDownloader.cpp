/* ModPackDownloader.cpp — content-pack installer for the Mods menu.
 *
 * Two entry points share one worker pipeline:
 *
 *   * Curated installs (StartModPackInstallAsync) — known-good release
 *     assets (currently the SSB Reloaded hi-res pack) with a
 *     compile-time-pinned URL + size + SHA-256. The digest is baked in
 *     at release-bump time, so a compromised upstream listing cannot
 *     redirect the install (the updater's metadata-digest caveat,
 *     TD-009, does not apply).
 *   * User-supplied installs (StartPackInstallFromUrlAsync /
 *     StartPackInstallFromPathAsync / a dropped file) — the same
 *     download + validate + install machinery, minus the pinned digest
 *     (the source is arbitrary, so the computed SHA-256 is reported in
 *     the status line rather than enforced).
 *
 * Payload sniffing: a zip/dir with a root manifest.json is a script mod
 * (installed as-is into mods/, activated on Hot Reload); PNGs in the
 * native "#<rgba8crc>#<fmt>#<siz>.png" grammar are a texture pack
 * (zips installed in place — HiResPack::ScanZip indexes members without
 * extraction); PNGs in the GLideN64/Rice grammar are converted member-
 * by-member through the vendored hash mapping
 * (tools/developer/modding/vendor/battleship.json -> bundled-data/gliden64-map.json,
 * staged next to the binary by the build) and materialized as a mods/
 * subdir.
 *
 * Pipeline (all on a detached worker — a 200 MB download must never
 * stall the render thread):
 *
 *   Downloading   DownloadService -> mods/.<name>.part  (cancellable)
 *   Verifying     size + pinned SHA-256                 (FileHash.cpp)
 *   Validating    libzip member audit + kind detection
 *   Converting    GLideN64 -> native rename materialization
 *   Installing    rename/copy into mods/               (same dir = atomic)
 *
 * The .part/.tmp staging names keep a half-installed payload invisible
 * to both consumers of mods/ (HiResPack indexes only .zip/.png,
 * MountModsDir only mounts .o2r/.otr/.zip/manifest dirs), so an
 * interrupted run can never be mistaken for an installed pack.
 *
 * Cancel semantics: CancelModPackInstall() bumps s_generation AND calls
 * DownloadCancel on the in-flight transfer so a 200 MB download stops
 * instead of running to completion in the background. After any later
 * step the worker deletes the staged output and returns to Idle.
 *
 * Transport: src/platform/http/download_service.* — in-process libcurl
 * worker on desktop, HttpURLConnection worker on Android, stub on UWP
 * (this file is excluded from the UWP build in CMakeLists.txt).
 */
#include "enhancements.h"

#include "FileHash.h"
#include "port_log.h"
#include "exe_dir.h"
#include "download_service.h"

#include <ship/Context.h>
#include <ship/window/FileDropMgr.h>

#include <nlohmann/json.hpp>
#include <zip.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ssb64 {
namespace enhancements {

namespace {
namespace fs = std::filesystem;


// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

/* SSB Reloaded 2.0.1 (BattleShip build), by GhostlyDark.
 * https://github.com/GhostlyDark/SSB-Reloaded
 *
 * Pinned against the v2.0.1 release asset's official digest
 * (GitHub API `assets[].digest` field, checked 2026-10-14). The archive
 * holds ~4,700 PNGs under "SSB Reloaded (BattleShip)/" named in the
 * pack grammar "#<rgba8CRC>#<fmt>#<siz>.png"; HiResPack::ScanZip reads
 * the .zip in place, so no extraction is needed. Redistribution is not
 * performed — the user's own download lands in their mods/ folder, same
 * as dropping it there by hand per the upstream README. */
constexpr CuratedModPack kCuratedPacks[] = {
    {
        "ssb-reloaded",
        "SSB Reloaded",
        "2.0.1",
        "https://github.com/GhostlyDark/SSB-Reloaded/releases/download/v2.0.1/ssb-reloaded-v2.0.1-battleship.zip",
        "5cc70f30408931544237982ca2d83b048e50b28c23279d9f21b7c34d50ead88b",
        219954181,
        "ssb-reloaded-v2.0.1-battleship.zip",
        "Complete HD texture pack (characters, stages, items, UI). "
        "Cosmetic only — does not affect netplay compatibility.",
        "https://github.com/GhostlyDark/SSB-Reloaded",
    },
};

// ---------------------------------------------------------------------------
// Flow state
// ---------------------------------------------------------------------------

std::atomic<ModPackInstallPhase> s_phase{ModPackInstallPhase::Idle};
std::atomic<uint64_t>            s_generation{0};
std::atomic<ModPackInstalledKind> s_lastKind{ModPackInstalledKind::None};

std::mutex  s_stateMutex;
std::string s_status;
std::string s_partPath;      // download staging file (for progress polling)
std::string s_lastName;      // display name of the last installed payload

void SetStatus(std::string s) {
    std::lock_guard<std::mutex> lock(s_stateMutex);
    s_status = std::move(s);
}

void SetPartPath(std::string p) {
    std::lock_guard<std::mutex> lock(s_stateMutex);
    s_partPath = std::move(p);
}

void SetLastName(std::string n) {
    std::lock_guard<std::mutex> lock(s_stateMutex);
    s_lastName = std::move(n);
}

// ---------------------------------------------------------------------------
// Active-download handle (for cancel)
// ---------------------------------------------------------------------------

// The in-flight transfer, published by the worker under s_childMutex so
// CancelModPackInstall() (UI thread) can cancel it. Cleared by the
// worker before it disposes the handle — a cancel that misses the
// window finds nullptr and becomes a no-op.
std::mutex s_childMutex;
Download* s_download = nullptr;

void KillActiveDownload() {
    std::lock_guard<std::mutex> lock(s_childMutex);
    if (s_download != nullptr) {
        DownloadCancel(s_download);
    }
}

// ---------------------------------------------------------------------------
// Path/name helpers
// ---------------------------------------------------------------------------

bool HasTraversalOrAbsolute(const std::string& name) {
    if (name.empty() || name.front() == '/' || name.front() == '\\') return true;
    if (name.find(':') != std::string::npos) return true; // drive letter / ADS
    // Backslash is a separator on Windows: "a\\..\\b.png" would dodge the
    // '/'-component check below, so reject the character outright. The
    // packs we ship links to use '/' exclusively.
    if (name.find('\\') != std::string::npos) return true;
    // ".." as a whole path component only — "a..b.png" is legitimate.
    size_t pos = 0;
    while (pos <= name.size()) {
        size_t end = name.find('/', pos);
        std::string_view comp(name.data() + pos,
                              (end == std::string::npos ? name.size() : end) - pos);
        if (comp == "..") return true;
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return false;
}

std::string BasenameOf(const std::string& name) {
    const size_t slash = name.find_last_of('/');
    return slash == std::string::npos ? name : name.substr(slash + 1);
}

std::string LowerExt(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return {};
    std::string ext = name.substr(dot);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}

/* mods/ installable dir/file name derived from a URL basename or local
 * path stem: [A-Za-z0-9 ._-], spaces preserved, capped at 64 chars. */
std::string SanitizeInstallName(std::string in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (std::isalnum(uc) || c == ' ' || c == '-' || c == '_' || c == '.') {
            out += c;
        } else {
            out += '_';
        }
        if (out.size() >= 64) break;
    }
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
    return out.empty() ? "custom-pack" : out;
}

std::string DeriveBaseName(const std::string& source) {
    // Last path component without its extension. Handles URLs (path
    // segment) and filesystem paths alike.
    size_t end = source.find_first_of("?#"); // strip url query/fragment
    std::string clean = source.substr(0, end);
    while (!clean.empty() && (clean.back() == '/' || clean.back() == '\\')) {
        clean.pop_back();
    }
    const size_t slash = clean.find_last_of("/\\");
    std::string base = slash == std::string::npos ? clean : clean.substr(slash + 1);
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        base.resize(dot);
    }
    return SanitizeInstallName(base);
}

// ---------------------------------------------------------------------------
// Pack-content validation + grammar detection
// ---------------------------------------------------------------------------

/* Native pack grammar: basename contains "#<8 hex>#<n>#<n>" — same shape
 * HiResPack::ParseFilename requires. Only needs to prove the archive is
 * really a texture pack; the loader itself re-parses per member. */
bool LooksLikePackPng(const std::string& basename) {
    if (LowerExt(basename) != ".png") return false;
    const size_t first = basename.find('#');
    if (first == std::string::npos) return false;
    const size_t hexStart = first + 1;
    size_t i = hexStart;
    while (i < basename.size() && std::isxdigit(static_cast<unsigned char>(basename[i]))) ++i;
    return i > hexStart && i < basename.size() && basename[i] == '#';
}

/* GLideN64/Rice-PNG grammar:
 *     "<prefix>#<crc8>#<fmt>#<siz>[#<palCRC8>]_<tag>.png"
 * e.g. "SMASH BROTHERS#3A8B8290#2#0#41B9B4CE_ciByRGBA.png" or
 * "...#ABCDEF12#0#0_all.png". On match, `outKey` receives the
 * battleship.json lookup key ("CRC#F#S" or "CRC#F#S#PAL", uppercase). */
bool ParseGlidenName(const std::string& basename, std::string* outKey) {
    if (LowerExt(basename) != ".png") return false;
    // "_<tag>.png" must terminate the name.
    const size_t stemEnd = basename.size() - 4;
    const size_t us = basename.rfind('_', stemEnd);
    if (us == std::string::npos || us == 0 || us + 1 >= stemEnd) return false;
    // Split everything before the '_' on '#'; the last 3-4 fields are
    // the hash tuple, the leading part is the free-form prefix (it may
    // itself contain '#' or '_').
    std::vector<std::string> fields;
    for (size_t pos = 0; pos < us;) {
        const size_t next = basename.find('#', pos);
        fields.push_back(basename.substr(pos, (next == std::string::npos || next > us
                                                 ? us
                                                 : next) - pos));
        if (next == std::string::npos || next > us) break;
        pos = next + 1;
    }
    if (fields.size() < 4) return false; // prefix + crc + fmt + siz minimum
    auto isHex8 = [](const std::string& f) {
        if (f.size() != 8) return false;
        for (char c : f) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    };
    auto isDigit1 = [](const std::string& f) {
        return f.size() == 1 &&
               std::isxdigit(static_cast<unsigned char>(f[0]));
    };
    // Candidate trailing tuple: either [crc, fmt, siz] or
    // [crc, fmt, siz, pal].
    const size_t n = fields.size();
    std::string crc, fmt, siz, pal;
    if (fields.size() >= 5 && isHex8(fields[n - 1]) && isDigit1(fields[n - 2]) &&
        isDigit1(fields[n - 3]) && isHex8(fields[n - 4])) {
        crc = fields[n - 4]; fmt = fields[n - 3]; siz = fields[n - 2]; pal = fields[n - 1];
    } else if (isDigit1(fields[n - 1]) && isDigit1(fields[n - 2]) &&
               isHex8(fields[n - 3])) {
        crc = fields[n - 3]; fmt = fields[n - 2]; siz = fields[n - 1];
    } else {
        return false;
    }
    auto upper = [](std::string f) {
        for (char& c : f) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return f;
    };
    std::string key = upper(crc) + "#" + fmt + "#" + siz;
    if (!pal.empty()) key += "#" + upper(pal);
    if (outKey) *outKey = key;
    return true;
}

/* "#<ourHash>#<fmt>#<siz>.png" from a mapped hash + a parse key. */
std::string NativePackName(const std::string& ourHash, const std::string& key) {
    const size_t f1 = key.find('#');
    const size_t f2 = key.find('#', f1 + 1);
    return "#" + ourHash + key.substr(f1, f2 - f1 + 2) + ".png";
}

// ---------------------------------------------------------------------------
// GLideN64 -> native hash mapping (vendored data)
// ---------------------------------------------------------------------------

/* tools/developer/modding/vendor/battleship.json, staged next to the binary as
 * bundled-data/gliden64-map.json. Maps "RiceCRC#FMT#SIZ[#PAL]" (uppercase
 * hex) -> our decoded-RGBA8 hash (8 hex). Pure rename data — no image
 * decoding. Loaded lazily on the first GLideN64 pack seen. */
std::once_flag s_mapOnce;
bool s_mapOk = false;
std::unordered_map<std::string, std::string> s_glidenMap;

std::filesystem::path GlidenMapPath() {
    namespace fs = std::filesystem;
    const std::string dir = ExecutableDirPath();
    if (dir.empty()) return {};
    const fs::path exeDir(dir);
    // Packagers relocate data relative to the binary: Windows keeps it
    // next to the exe, the macOS .app moves it to Contents/Resources,
    // the Linux AppImage to usr/share/<app>.
    const fs::path candidates[] = {
        exeDir / "bundled-data" / "gliden64-map.json",
        exeDir.parent_path() / "Resources" / "bundled-data" / "gliden64-map.json",
        exeDir.parent_path() / "share" / "BattleShip" / "bundled-data" / "gliden64-map.json",
    };
    std::error_code ec;
    for (const fs::path& c : candidates) {
        if (fs::is_regular_file(c, ec)) return c;
        ec.clear();
    }
    return candidates[0];
}

void LoadGlidenMap() {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path path = GlidenMapPath();
    if (path.empty() || !fs::is_regular_file(path, ec)) {
        SPDLOG_WARN("Mod pack: GLideN64 map not found at {}", path.string());
        return;
    }
    try {
        std::ifstream in(path, std::ios::binary);
        const nlohmann::json j = nlohmann::json::parse(in);
        if (!j.is_object()) {
            SPDLOG_ERROR("Mod pack: gliden64-map.json is not an object");
            return;
        }
        s_glidenMap.reserve(j.size());
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it.value().is_string()) {
                s_glidenMap.emplace(it.key(), it.value().get<std::string>());
            }
        }
        s_mapOk = !s_glidenMap.empty();
        SPDLOG_INFO("Mod pack: GLideN64 map loaded ({} entries)", s_glidenMap.size());
    } catch (const std::exception& e) {
        SPDLOG_ERROR("Mod pack: gliden64-map.json parse failed: {}", e.what());
    }
}

bool GlidenMapReady() {
    std::call_once(s_mapOnce, LoadGlidenMap);
    return s_mapOk;
}

// ---------------------------------------------------------------------------
// Payload classification
// ---------------------------------------------------------------------------

enum class PayloadKind { ScriptMod, NativePack, GlidenPack, Unknown };

/* Classify a zip: root manifest.json -> script mod; else by which PNG
 * grammar its members use. Also enforces the traversal audit — every
 * caller handles the archive's members by name, so unsafe names are
 * rejected regardless of kind. `pngCount` reports grammar-matching
 * members for status text. */
PayloadKind ClassifyZip(zip_t* z, std::string& error) {
    int native = 0, gliden = 0;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char* raw = zip_get_name(z, i, 0);
        if (raw == nullptr) continue;
        const std::string name(raw);
        if (HasTraversalOrAbsolute(name)) {
            error = "archive contains an unsafe path: " + name;
            return PayloadKind::Unknown;
        }
        if (name.back() == '/') continue;
        if (name == "manifest.json") {
            return PayloadKind::ScriptMod;
        }
        const std::string base = BasenameOf(name);
        if (LowerExt(base) != ".png") continue;
        if (LooksLikePackPng(base)) { ++native; continue; }
        std::string key;
        if (ParseGlidenName(base, &key)) {
            ++gliden;
            continue;
        }
    }
    if (native > 0 && gliden == 0) return PayloadKind::NativePack;
    if (gliden > 0) return PayloadKind::GlidenPack; // mixed packs convert too
    if (native > 0) return PayloadKind::NativePack;
    return PayloadKind::Unknown;
}

/* The strict audit the curated (pinned) path keeps: PNG pack members or
 * text metadata only — a pinned digest already proves content, this
 * catches a structurally-malformed mirror of it. */
bool ValidateCuratedPackZip(zip_t* z, std::string& error) {
    int packPngs = 0;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char* raw = zip_get_name(z, i, 0);
        if (raw == nullptr) continue;
        const std::string name(raw);
        /* Defense in depth: the pinned digest already proves content, but
         * the member audit should catch unsafe names even if a caller
         * ever bypasses verification. */
        if (HasTraversalOrAbsolute(name)) {
            error = "archive contains an unsafe path: " + name;
            return false;
        }
        if (name.back() == '/') continue;
        const std::string base = BasenameOf(name);
        const std::string ext = LowerExt(base);
        if (ext == ".png") {
            if (LooksLikePackPng(base)) ++packPngs;
            continue;
        }
        if (ext == ".txt" || ext == ".md") continue;
        error = "archive contains an unexpected file: " + name;
        return false;
    }
    if (packPngs == 0) {
        error = "archive has no pack textures (#<hash>#<fmt>#<siz>.png)";
        return false;
    }
    return true;
}

zip_t* OpenZipChecked(const std::filesystem::path& zipPath, std::string& error) {
    int zerr = 0;
    zip_t* z = zip_open(zipPath.generic_string().c_str(), ZIP_RDONLY, &zerr);
    if (z == nullptr) {
        zip_error_t ze;
        zip_error_init_with_code(&ze, zerr);
        error = std::string("file is not a valid zip (") + zip_error_strerror(&ze) + ")";
        zip_error_fini(&ze);
    }
    return z;
}

/* Extract one zip member into destPath (creating parents). Caller has
 * already run the name audit. */
bool ExtractZipMember(zip_t* z, zip_int64_t index, const std::filesystem::path& destPath) {
    std::error_code ec;
    std::filesystem::create_directories(destPath.parent_path(), ec);
    zip_file_t* f = zip_fopen_index(z, index, 0);
    if (f == nullptr) return false;
    FILE* out = std::fopen(destPath.string().c_str(), "wb");
    if (out == nullptr) {
        zip_fclose(f);
        return false;
    }
    char buf[64 * 1024];
    zip_int64_t got;
    bool ok = true;
    while ((got = zip_fread(f, buf, sizeof(buf))) > 0) {
        if (std::fwrite(buf, 1, static_cast<size_t>(got), out) != static_cast<size_t>(got)) {
            ok = false;
            break;
        }
    }
    ok = ok && got == 0;
    std::fclose(out);
    zip_fclose(f);
    return ok;
}

/* Convert a GLideN64 zip/dir into a native-grammar directory at outDir.
 * Matched PNGs land as "<reldir>/#<hash>#<fmt>#<siz>.png"; unmatched or
 * unparseable PNGs are skipped (the mapping only covers vanilla-ROM
 * textures — Smash Remix additions have no target); .txt/.md metadata is
 * copied through for browsability. Returns counts for the status line. */
struct ConvertStats { int matched = 0, unmatched = 0, other = 0; };

bool ConvertGlidenZip(zip_t* z, const std::filesystem::path& outDir,
                      ConvertStats& st, std::string& error) {
    namespace fs = std::filesystem;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n; i++) {
        const char* raw = zip_get_name(z, i, 0);
        if (raw == nullptr) continue;
        const std::string name(raw);
        if (name.back() == '/') continue;
        const std::string base = BasenameOf(name);
        const std::string ext = LowerExt(base);
        const std::string relDir = name.size() > base.size()
                                       ? name.substr(0, name.size() - base.size() - 1)
                                       : std::string();
        const fs::path dstDir = outDir / fs::path(relDir);

        if (ext == ".png") {
            if (LooksLikePackPng(base)) {
                // Already native — keep verbatim.
                if (!ExtractZipMember(z, i, dstDir / base)) {
                    error = "could not extract " + name;
                    return false;
                }
                ++st.matched;
                continue;
            }
            std::string key;
            if (!ParseGlidenName(base, &key)) {
                ++st.other;
                continue;
            }
            const auto it = s_glidenMap.find(key);
            if (it == s_glidenMap.end()) {
                ++st.unmatched;
                continue;
            }
            if (!ExtractZipMember(z, i, dstDir / NativePackName(it->second, key))) {
                error = "could not extract " + name;
                return false;
            }
            ++st.matched;
        } else if (ext == ".txt" || ext == ".md") {
            ExtractZipMember(z, i, dstDir / base); // metadata: best-effort
        } else {
            ++st.other;
        }
    }
    if (st.matched == 0) {
        error = "no pack textures matched the ROM's texture set";
        return false;
    }
    return true;
}

bool ConvertGlidenDir(const std::filesystem::path& srcDir,
                      const std::filesystem::path& outDir,
                      ConvertStats& st, std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(srcDir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const fs::path src = it->path();
        const std::string base = src.filename().string();
        const std::string ext = LowerExt(base);
        const fs::path relDir = src.parent_path().lexically_relative(srcDir);
        const fs::path dstDir = outDir / relDir;

        if (ext == ".png") {
            std::string outName;
            if (LooksLikePackPng(base)) {
                outName = base;
            } else {
                std::string key;
                if (!ParseGlidenName(base, &key)) { ++st.other; continue; }
                const auto mit = s_glidenMap.find(key);
                if (mit == s_glidenMap.end()) { ++st.unmatched; continue; }
                outName = NativePackName(mit->second, key);
            }
            std::error_code cec;
            fs::create_directories(dstDir, cec);
            fs::copy_file(src, dstDir / outName, fs::copy_options::overwrite_existing, cec);
            if (cec) {
                error = "could not copy " + base + ": " + cec.message();
                return false;
            }
            ++st.matched;
        } else if (ext == ".txt" || ext == ".md") {
            std::error_code cec;
            fs::create_directories(dstDir, cec);
            fs::copy_file(src, dstDir / base, fs::copy_options::overwrite_existing, cec);
        } else {
            ++st.other;
        }
    }
    if (st.matched == 0) {
        error = "no pack textures matched the ROM's texture set";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------

struct InstallJob {
    std::string label;        // status text ("SSB Reloaded 2.0.1", URL, file name)
    std::string version;
    std::string url;          // "" => local source
    std::string localPath;    // file or directory source
    std::string sha256;       // "" => unverified (reported, not enforced)
    uint64_t    expectedSize = 0; // 0 => unknown
    std::string installName;  // fixed output base name; "" => derive from source
    bool        curated = false;
};

/* mods/ root — the dir HiResPack scans and "Open Mods Folder" opens.
 * Also scanned by MountModsDir for script mods (port.cpp walks both
 * this and the exe-adjacent mods/). */
std::filesystem::path ModsRoot() {
    return Ship::Context::GetPathRelativeToAppDirectory("mods");
}

/* Move/copy a finished payload into mods/ atomically: stage under a
 * dot-name in the same directory, then rename. */
bool CommitFile(const std::filesystem::path& src, const std::filesystem::path& dst,
                std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path tmp = dst.parent_path() / ("." + dst.filename().string() + ".tmp");
    fs::remove(tmp, ec); ec.clear();
    if (src != tmp) {
        fs::rename(src, tmp, ec);
        if (ec) { // cross-fs (local file elsewhere) — copy instead
            ec.clear();
            fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error = "could not stage file into mods/: " + ec.message();
                return false;
            }
        }
    }
    fs::remove(dst, ec); ec.clear();
    fs::rename(tmp, dst, ec);
    if (ec) {
        error = "could not install into mods/: " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

/* Materialize a directory payload the same way: build under a hidden
 * staging dir inside mods/, then rename to the final name. Existing
 * installs are replaced wholesale (delete + rename is not atomic, but
 * reinstalling a dir over itself while mounted is worse). */
bool CommitDir(const std::filesystem::path& staged, const std::filesystem::path& dst,
               std::string& error) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove_all(dst, ec); ec.clear();
    fs::rename(staged, dst, ec);
    if (ec) {
        error = "could not install into mods/: " + ec.message();
        fs::remove_all(staged, ec);
        return false;
    }
    return true;
}

/* Shared state for one install job - the worker is a linear pipeline
 * (prepare -> acquire -> verify -> classify -> install), each phase a
 * function taking this context. Cleanup helpers centralize the recurring
 * "close the zip / drop the .part staging file" tails. */
struct InstallCtx {
    InstallJob job;
    uint64_t gen;
    std::error_code ec;
    std::string err;
    fs::path modsDir;
    std::string baseName;
    fs::path srcFile;   // file payload (downloaded .part or local file)
    fs::path partPath;  // mods/.<base>.part staging for URL payloads
    fs::path localPath;
    zip_t* z = nullptr;
    bool isDir = false;
    bool isPng = false;
    bool isArchiveFile = false;
    std::string srcBase;
    std::string srcExt;

    bool cancelled() const { return s_generation.load() != gen; }
    void fail(const std::string& why) {
        SPDLOG_ERROR("Mod install of {} failed: {}", job.label, why);
        SetStatus("Error: " + why);
        SetPartPath({});
        s_phase.store(ModPackInstallPhase::Error);
    }
    void done(ModPackInstalledKind kind, const std::string& name) {
        SetPartPath({});
        SetLastName(name);
        s_lastKind.store(kind);
        SetStatus("Installed " + name);
        SPDLOG_INFO("Mod install: {} -> {}", job.label, name);
        s_phase.store(ModPackInstallPhase::Done);
    }
    void dropPart() {
        if (!partPath.empty()) {
            fs::remove(partPath, ec);
            ec.clear();
        }
    }
    void closeZip() {
        if (z != nullptr) {
            zip_close(z);
            z = nullptr;
        }
    }
    /* Frequent tail: release transient artifacts then report the failure. */
    void abort(const std::string& why) {
        closeZip();
        dropPart();
        fail(why);
    }
    void abortSilent() { // cancelled: transient cleanup, no error
        closeZip();
        dropPart();
    }
};

bool PrepareInstallDirs(InstallCtx& c) {
    try {
        c.modsDir = ModsRoot();
    } catch (...) {
        c.fail("app directory unavailable");
        return false;
    }
    fs::create_directories(c.modsDir, c.ec);
    if (c.ec) {
        c.fail("could not create mods/ folder: " + c.ec.message());
        return false;
    }
    c.baseName = c.job.installName.empty()
                     ? DeriveBaseName(c.job.url.empty() ? c.job.localPath : c.job.url)
                     : SanitizeInstallName(c.job.installName);
    return true;
}

/* URL jobs download into mods/.<base>.part; local jobs read the source in
 * place. Returns false on failure or cancellation (silent). */
bool AcquirePayload(InstallCtx& c) {
    if (c.job.url.empty()) {
        c.srcFile = c.job.localPath;
        return true;
    }
    c.partPath = c.modsDir / ("." + c.baseName + ".part");
    SetPartPath(c.partPath.string());
    fs::remove(c.partPath, c.ec);
    c.ec.clear();

    s_phase.store(ModPackInstallPhase::Downloading);
    SetStatus("Downloading " + c.job.label + "...");
    SPDLOG_INFO("Mod pack: downloading {} -> {}", c.job.url, c.partPath.string());

    Download* dl = DownloadStart(c.job.url, c.partPath);
    if (dl == nullptr) {
        c.fail("could not start the download");
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(s_childMutex);
        s_download = dl;
    }
    DownloadState dlState;
    while ((dlState = DownloadPoll(dl)) == DownloadState::Running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    {
        std::lock_guard<std::mutex> lock(s_childMutex);
        s_download = nullptr;
    }
    dlState = DownloadFinish(dl);
    if (c.cancelled() || dlState == DownloadState::Cancelled) {
        c.dropPart();
        return false;
    }
    if (dlState != DownloadState::Done) {
        c.dropPart();
        c.fail("download failed");
        return false;
    }
    c.srcFile = c.partPath;
    return true;
}

/* Pinned size/sha256 verification when the manifest carries them; otherwise
 * hash anyway so the user can check the digest against the link source. */
bool VerifyPayload(InstallCtx& c) {
    if (!c.job.sha256.empty() || c.job.expectedSize > 0) {
        s_phase.store(ModPackInstallPhase::Verifying);
        SetStatus("Verifying download...");
        if (!DownloadedFileMatches(c.srcFile.string(), c.job.expectedSize,
                                   c.job.sha256.empty() ? "" : std::string("sha256:") + c.job.sha256,
                                   c.err)) {
            c.dropPart();
            c.fail(c.err);
            return false;
        }
    } else if (!c.srcFile.empty()) {
        // Unverified source - compute the digest anyway so the user can
        // check it against wherever they got the link.
        s_phase.store(ModPackInstallPhase::Verifying);
        SetStatus("Hashing download...");
        const std::string hex = Sha256File(c.srcFile.string());
        if (!hex.empty()) {
            SetStatus("sha256: " + hex);
        }
    }
    if (c.cancelled()) {
        c.dropPart();
        return false;
    }
    return true;
}

/* Directory scan used for folder payloads: counts PNG basenames matching
 * the native grammar vs the GLideN64/Rice grammar. */
PayloadKind ClassifyDir(const fs::path& dir, std::error_code& ec) {
    if (fs::exists(dir / "manifest.json", ec)) {
        return PayloadKind::ScriptMod;
    }
    int native = 0, gliden = 0;
    for (auto it = fs::recursive_directory_iterator(dir, ec);
         it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file(ec)) continue;
        const std::string base = it->path().filename().string();
        if (LowerExt(base) != ".png") continue;
        if (LooksLikePackPng(base)) ++native;
        else { std::string k; if (ParseGlidenName(base, &k)) ++gliden; }
    }
    if (gliden > 0) return PayloadKind::GlidenPack;
    if (native > 0) return PayloadKind::NativePack;
    return PayloadKind::Unknown;
}

/* Determines payload shape (dir / png / archive) and pack kind. On a fatal
 * validation error it reports via c.fail() and returns false. */
bool ClassifyPayload(InstallCtx& c, PayloadKind* kind) {
    s_phase.store(ModPackInstallPhase::Validating);
    SetStatus("Checking contents...");

    *kind = PayloadKind::Unknown;
    if (c.isDir) {
        *kind = ClassifyDir(c.localPath, c.ec);
    } else if (c.isPng) {
        *kind = PayloadKind::NativePack; // single texture: grammar checked at convert time
    } else if (c.isArchiveFile) {
        c.z = OpenZipChecked(c.srcFile, c.err);
        if (c.z == nullptr) {
            c.dropPart();
            c.fail(c.err);
            return false;
        }
        if (c.job.curated) {
            // Pinned content still gets the strict member audit.
            if (!ValidateCuratedPackZip(c.z, c.err)) {
                c.abort(c.err);
                return false;
            }
            *kind = PayloadKind::NativePack;
        } else if (c.srcExt == ".o2r" || c.srcExt == ".otr") {
            // An .o2r/.otr is mounted by MountModsDir whether or not it
            // carries a manifest (pure asset-swap archives exist).
            *kind = PayloadKind::ScriptMod;
        } else {
            *kind = ClassifyZip(c.z, c.err);
        }
    } else {
        c.fail("unsupported input - expected a .zip, an image pack folder, or a PNG");
        return false;
    }
    if (c.cancelled()) {
        c.abortSilent();
        return false;
    }
    return true;
}

/* Stage a directory payload through mods/.<base>.tmp then atomically commit
 * it into mods/<base>. */
void InstallDirPayload(InstallCtx& c, ModPackInstalledKind result, const char* copyErr) {
    const fs::path dst = c.modsDir / c.baseName;
    const fs::path stage = c.modsDir / ("." + c.baseName + ".tmp");
    fs::remove_all(stage, c.ec); c.ec.clear();
    fs::copy(c.localPath, stage,
             fs::copy_options::recursive | fs::copy_options::overwrite_existing, c.ec);
    if (c.ec) {
        fs::remove_all(stage, c.ec);
        c.fail(std::string(copyErr) + c.ec.message());
        return;
    }
    if (!CommitDir(stage, dst, c.err)) {
        c.fail(c.err);
        return;
    }
    c.done(result, c.baseName);
}

/* Zip payload committed as mods/<base>.zip. */
void InstallZipPayload(InstallCtx& c, ModPackInstalledKind result) {
    const fs::path dst = c.modsDir / (c.baseName + ".zip");
    if (!CommitFile(c.srcFile, dst, c.err)) {
        c.abort(c.err);
        return;
    }
    c.done(result, c.baseName + ".zip");
}

/* Single loose PNG: must already carry the native grammar name, unless the
 * GLideN64 map can rename it. */
void InstallPngPayload(InstallCtx& c) {
    std::string key;
    if (!LooksLikePackPng(c.srcBase)) {
        if (ParseGlidenName(c.srcBase, &key) && GlidenMapReady()) {
            const auto it = s_glidenMap.find(key);
            if (it != s_glidenMap.end()) {
                const std::string outName = NativePackName(it->second, key);
                const fs::path dst = c.modsDir / outName;
                if (!CommitFile(c.srcFile, dst, c.err)) {
                    c.fail(c.err);
                    return;
                }
                c.done(ModPackInstalledKind::TexturePack, outName);
                return;
            }
        }
        c.fail("PNG is not in the pack grammar (\"#<hash>#<fmt>#<siz>.png\") "
               "or the GLideN64/Rice grammar");
        return;
    }
    const fs::path dst = c.modsDir / c.srcBase;
    if (!CommitFile(c.srcFile, dst, c.err)) {
        c.fail(c.err);
        return;
    }
    c.done(ModPackInstalledKind::TexturePack, c.srcBase);
}

/* GLideN64/Rice pack: convert member names through the bundled map into a
 * staging dir, then commit the converted tree. */
void InstallGlidenPayload(InstallCtx& c) {
    if (!GlidenMapReady()) {
        c.abort("this build has no GLideN64 conversion table "
                "(bundled-data/gliden64-map.json missing)");
        return;
    }
    s_phase.store(ModPackInstallPhase::Converting);
    SetStatus("Converting texture names...");
    const fs::path stage = c.modsDir / ("." + c.baseName + ".tmp");
    fs::remove_all(stage, c.ec); c.ec.clear();
    fs::create_directories(stage, c.ec);
    ConvertStats st;
    const bool ok = c.z ? ConvertGlidenZip(c.z, stage, st, c.err)
                        : ConvertGlidenDir(c.localPath, stage, st, c.err);
    c.closeZip();
    if (!ok) {
        fs::remove_all(stage, c.ec);
        c.dropPart();
        c.fail(c.err);
        return;
    }
    if (c.cancelled()) {
        fs::remove_all(stage, c.ec);
        c.dropPart();
        return;
    }
    s_phase.store(ModPackInstallPhase::Installing);
    SetStatus("Installing converted pack...");
    const fs::path dst = c.modsDir / c.baseName;
    if (!CommitDir(stage, dst, c.err)) {
        c.dropPart();
        c.fail(c.err);
        return;
    }
    c.dropPart();
    c.done(ModPackInstalledKind::TexturePack,
           c.baseName + fmt::format(" ({} textures converted, {} skipped)",
                                    st.matched, st.unmatched));
}

void InstallPayload(InstallCtx& c, PayloadKind kind) {
    switch (kind) {
    case PayloadKind::ScriptMod:
        s_phase.store(ModPackInstallPhase::Installing);
        SetStatus("Installing mod into mods/...");
        if (c.isDir) {
            InstallDirPayload(c, ModPackInstalledKind::ScriptMod,
                              "could not copy mod folder: ");
        } else {
            InstallZipPayload(c, ModPackInstalledKind::ScriptMod);
        }
        break;
    case PayloadKind::NativePack:
        s_phase.store(ModPackInstallPhase::Installing);
        SetStatus("Installing pack into mods/...");
        if (c.isDir) {
            InstallDirPayload(c, ModPackInstalledKind::TexturePack,
                              "could not copy pack folder: ");
        } else if (c.isPng) {
            InstallPngPayload(c);
        } else {
            InstallZipPayload(c, ModPackInstalledKind::TexturePack);
        }
        break;
    case PayloadKind::GlidenPack:
        InstallGlidenPayload(c);
        break;
    case PayloadKind::Unknown:
    default:
        c.abort("unrecognized content - expected a mod (manifest.json), a "
                "native texture pack, or a GLideN64/Rice pack");
        return;
    }
}

void InstallWorker(InstallJob job, uint64_t gen) {
    namespace fs = std::filesystem;
    InstallCtx c{ std::move(job), gen };

    if (!PrepareInstallDirs(c)) return;
    if (!AcquirePayload(c)) return;
    if (!VerifyPayload(c)) return;

    // Payload-shape facts shared by classification and install.
    c.localPath = c.job.localPath.empty() ? fs::path() : fs::path(c.job.localPath);
    c.isDir = !c.localPath.empty() && fs::is_directory(c.localPath, c.ec);
    c.ec.clear();
    // The payload's own extension comes from the source name (the URL's
    // last segment or the local path), not the .part staging name.
    c.srcBase = !c.localPath.empty()
                    ? c.localPath.filename().string()
                    : BasenameOf(c.job.url.substr(0, c.job.url.find_first_of("?#")));
    c.srcExt = LowerExt(c.srcBase);
    c.isPng = c.srcExt == ".png";
    c.isArchiveFile = !c.srcFile.empty() && !c.isDir && !c.isPng;

    PayloadKind kind;
    if (!ClassifyPayload(c, &kind)) return;
    InstallPayload(c, kind);
    c.closeZip();
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const CuratedModPack* GetCuratedModPacks(size_t* count) {
    *count = sizeof(kCuratedPacks) / sizeof(kCuratedPacks[0]);
    return kCuratedPacks;
}

const CuratedModPack* FindCuratedModPack(const std::string& id) {
    for (const CuratedModPack& p : kCuratedPacks) {
        if (id == p.id) return &p;
    }
    return nullptr;
}

std::string GetModPackInstallPath(const CuratedModPack& pack) {
    try {
        namespace fs = std::filesystem;
        return (fs::path(ModsRoot()) / pack.installFileName).string();
    } catch (...) {
        return {};
    }
}

bool IsModPackInstalled(const CuratedModPack& pack) {
    namespace fs = std::filesystem;
    const std::string path = GetModPackInstallPath(pack);
    if (path.empty()) return false;
    std::error_code ec;
    const auto size = fs::file_size(path, ec);
    return !ec && size == static_cast<uint64_t>(pack.expectedSize);
}

namespace {

bool CanStartNewInstall() {
    const ModPackInstallPhase cur = s_phase.load();
    return cur == ModPackInstallPhase::Idle ||
           cur == ModPackInstallPhase::Done ||
           cur == ModPackInstallPhase::Error;
}

void LaunchWorker(InstallJob job) {
    s_generation.fetch_add(1);
    SetStatus({});
    s_lastKind.store(ModPackInstalledKind::None);
    const uint64_t gen = s_generation.load();
    std::thread([job = std::move(job), gen]() mutable {
        InstallWorker(std::move(job), gen);
    }).detach();
}

} // namespace

void StartModPackInstallAsync(const std::string& packId) {
    if (!CanStartNewInstall()) return; // a flow is already running — ignore
    const CuratedModPack* pack = FindCuratedModPack(packId);
    if (pack == nullptr) {
        SPDLOG_ERROR("Mod pack: unknown id '{}'", packId);
        return;
    }
    InstallJob job;
    job.label = std::string(pack->displayName) + " " + pack->version;
    job.version = pack->version;
    job.url = pack->url;
    job.sha256 = pack->sha256;
    job.expectedSize = static_cast<uint64_t>(pack->expectedSize);
    job.installName = std::string(pack->installFileName);
    job.installName = job.installName.substr(0, job.installName.find_last_of('.'));
    job.curated = true;
    LaunchWorker(std::move(job));
}

void StartPackInstallFromUrlAsync(const std::string& url) {
    if (!CanStartNewInstall()) return;
    std::string u = url;
    // trim whitespace — pasted links often carry it
    const size_t b = u.find_first_not_of(" \t\r\n");
    const size_t e = u.find_last_not_of(" \t\r\n");
    u = (b == std::string::npos) ? "" : u.substr(b, e - b + 1);
    if (u.empty() || (u.rfind("http://", 0) != 0 && u.rfind("https://", 0) != 0)) {
        SPDLOG_ERROR("Mod pack: bad URL '{}'", u);
        SetStatus("Error: not an http(s) URL");
        s_phase.store(ModPackInstallPhase::Error);
        return;
    }
    InstallJob job;
    job.label = u;
    job.url = u;
    LaunchWorker(std::move(job));
}

void StartPackInstallFromPathAsync(const std::string& path) {
    if (!CanStartNewInstall()) return;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (!std::filesystem::exists(p, ec)) {
        SetStatus("Error: path does not exist");
        s_phase.store(ModPackInstallPhase::Error);
        return;
    }
    InstallJob job;
    job.label = p.filename().string();
    job.localPath = p.string();
    LaunchWorker(std::move(job));
}

void CancelModPackInstall() {
    s_generation.fetch_add(1);
    KillActiveDownload(); // unblock the worker's transfer wait promptly
    SetPartPath({});
    s_phase.store(ModPackInstallPhase::Idle);
    SetStatus({});
}

ModPackInstallPhase GetModPackInstallPhase() { return s_phase.load(); }

std::string GetModPackInstallStatus() {
    std::lock_guard<std::mutex> lock(s_stateMutex);
    return s_status;
}

ModPackInstalledKind GetLastInstalledPackKind() { return s_lastKind.load(); }

bool IsGliden64ConverterReady() { return GlidenMapReady(); }

uint64_t GetModPackDownloadedBytes() {
    std::string path;
    {
        std::lock_guard<std::mutex> lock(s_stateMutex);
        path = s_partPath;
    }
    if (path.empty()) return 0;
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    return ec ? 0 : size;
}

namespace {

/* SDL_DROPFILE entry point: claim drops that plausibly carry a mod or
 * texture pack (a .zip/.o2r/.otr archive, a .png, or a directory) and
 * hand them to the installer. Anything else falls through to the next
 * registered handler / the "unsupported file" toast. */
bool ModPackDropHandler(char* path) {
    if (path == nullptr || !CanStartNewInstall()) return false;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (std::filesystem::is_directory(p, ec)) {
        StartPackInstallFromPathAsync(p.string());
        return true;
    }
    const std::string ext = LowerExt(p.filename().string());
    if (ext == ".zip" || ext == ".o2r" || ext == ".otr" || ext == ".png") {
        StartPackInstallFromPathAsync(p.string());
        return true;
    }
    return false;
}

} // namespace

void RegisterModPackDropHandler() {
    auto fdm = Ship::Context::GetInstance() ? Ship::Context::GetInstance()->GetFileDropMgr()
                                          : nullptr;
    if (fdm == nullptr) return;
    static std::once_flag once;
    std::call_once(once, [&] { fdm->RegisterDropHandler(&ModPackDropHandler); });
}

} // namespace enhancements
} // namespace ssb64
