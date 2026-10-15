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
 * Transport: src/platform/http/download_service.* — curl child on desktop,
 * HttpURLConnection worker on Android, stub on UWP (this file is
 * excluded from the UWP build in CMakeLists.txt).
 */
#include "enhancements.h"

#include "FileHash.h"
#include "port_log.h"
#include "platform/filesystem/exe_dir.h"
#include "platform/http/download_service.h"

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
        base = base.substr(0, dot);
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

void InstallWorker(InstallJob job, uint64_t gen) {
    namespace fs = std::filesystem;
    auto cancelled = [gen]() { return s_generation.load() != gen; };
    auto fail = [&](const std::string& why) {
        SPDLOG_ERROR("Mod install of {} failed: {}", job.label, why);
        SetStatus("Error: " + why);
        SetPartPath({});
        s_phase.store(ModPackInstallPhase::Error);
    };
    auto done = [&](ModPackInstalledKind kind, const std::string& name) {
        SetPartPath({});
        SetLastName(name);
        s_lastKind.store(kind);
        SetStatus("Installed " + name);
        SPDLOG_INFO("Mod install: {} -> {}", job.label, name);
        s_phase.store(ModPackInstallPhase::Done);
    };

    fs::path modsDir;
    try {
        modsDir = ModsRoot();
    } catch (...) {
        fail("app directory unavailable");
        return;
    }
    std::error_code ec;
    fs::create_directories(modsDir, ec);
    if (ec) {
        fail("could not create mods/ folder: " + ec.message());
        return;
    }

    const std::string baseName = job.installName.empty()
                                     ? DeriveBaseName(job.url.empty() ? job.localPath : job.url)
                                     : SanitizeInstallName(job.installName);

    // ---- Acquire the payload ------------------------------------------------
    // For URL jobs: download into mods/.<base>.part. For local jobs the
    // source path is used in place.
    fs::path srcFile; // populated when the payload is a file
    fs::path partPath;
    if (!job.url.empty()) {
        partPath = modsDir / ("." + baseName + ".part");
        SetPartPath(partPath.string());
        fs::remove(partPath, ec); ec.clear();

        s_phase.store(ModPackInstallPhase::Downloading);
        SetStatus("Downloading " + job.label + "...");
        SPDLOG_INFO("Mod pack: downloading {} -> {}", job.url, partPath.string());

        Download* dl = DownloadStart(job.url, partPath);
        if (dl == nullptr) {
            fail("could not start the download");
            return;
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
        if (cancelled() || dlState == DownloadState::Cancelled) {
            fs::remove(partPath, ec);
            return;
        }
        if (dlState != DownloadState::Done) {
            fs::remove(partPath, ec);
            fail("download failed");
            return;
        }
        srcFile = partPath;
    } else {
        srcFile = job.localPath;
    }

    // ---- Verifying ----------------------------------------------------------
    if (!job.sha256.empty() || job.expectedSize > 0) {
        s_phase.store(ModPackInstallPhase::Verifying);
        SetStatus("Verifying download...");
        std::string err;
        if (!DownloadedFileMatches(srcFile.string(), job.expectedSize,
                                   job.sha256.empty() ? "" : std::string("sha256:") + job.sha256,
                                   err)) {
            if (!partPath.empty()) fs::remove(partPath, ec);
            fail(err);
            return;
        }
    } else if (!srcFile.empty()) {
        // Unverified source — compute the digest anyway so the user can
        // check it against wherever they got the link.
        s_phase.store(ModPackInstallPhase::Verifying);
        SetStatus("Hashing download...");
        const std::string hex = Sha256File(srcFile.string());
        if (!hex.empty()) {
            SetStatus("sha256: " + hex);
        }
    }
    if (cancelled()) {
        if (!partPath.empty()) fs::remove(partPath, ec);
        return;
    }

    // ---- Validating + classifying -------------------------------------------
    s_phase.store(ModPackInstallPhase::Validating);
    SetStatus("Checking contents...");
    std::string err;

    const fs::path localPath = job.localPath.empty() ? fs::path() : fs::path(job.localPath);
    const bool isDir = !localPath.empty() && fs::is_directory(localPath, ec);
    ec.clear();
    // The payload's own extension comes from the source name (the URL's
    // last segment or the local path), not the .part staging name.
    const std::string srcBase = !localPath.empty()
                                    ? localPath.filename().string()
                                    : BasenameOf(job.url.substr(0, job.url.find_first_of("?#")));
    const std::string srcExt = LowerExt(srcBase);
    const bool isPng = srcExt == ".png";
    const bool isArchiveFile = !srcFile.empty() && !isDir && !isPng;

    zip_t* z = nullptr;
    PayloadKind kind = PayloadKind::Unknown;
    if (isDir) {
        // Directory payload: manifest.json at root => script mod folder.
        if (fs::exists(localPath / "manifest.json", ec)) {
            kind = PayloadKind::ScriptMod;
        } else {
            // Scan PNG basenames for either grammar.
            int native = 0, gliden = 0;
            for (auto it = fs::recursive_directory_iterator(localPath, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) { ec.clear(); continue; }
                if (!it->is_regular_file(ec)) continue;
                const std::string base = it->path().filename().string();
                if (LowerExt(base) != ".png") continue;
                if (LooksLikePackPng(base)) ++native;
                else { std::string k; if (ParseGlidenName(base, &k)) ++gliden; }
            }
            if (gliden > 0) kind = PayloadKind::GlidenPack;
            else if (native > 0) kind = PayloadKind::NativePack;
        }
    } else if (isPng) {
        kind = PayloadKind::NativePack; // single texture: grammar checked at convert time
    } else if (isArchiveFile) {
        z = OpenZipChecked(srcFile, err);
        if (z == nullptr) {
            if (!partPath.empty()) fs::remove(partPath, ec);
            fail(err);
            return;
        }
        if (job.curated) {
            // Pinned content still gets the strict member audit.
            if (!ValidateCuratedPackZip(z, err)) {
                zip_close(z);
                fs::remove(partPath, ec);
                fail(err);
                return;
            }
            kind = PayloadKind::NativePack;
        } else if (srcExt == ".o2r" || srcExt == ".otr") {
            // An .o2r/.otr is mounted by MountModsDir whether or not it
            // carries a manifest (pure asset-swap archives exist).
            kind = PayloadKind::ScriptMod;
        } else {
            kind = ClassifyZip(z, err);
        }
    } else {
        fail("unsupported input — expected a .zip, an image pack folder, or a PNG");
        return;
    }
    if (cancelled()) {
        if (z) zip_close(z);
        if (!partPath.empty()) fs::remove(partPath, ec);
        return;
    }

    // ---- Converting / installing ---------------------------------------------
    switch (kind) {
    case PayloadKind::ScriptMod: {
        s_phase.store(ModPackInstallPhase::Installing);
        SetStatus("Installing mod into mods/...");
        if (isDir) {
            const fs::path dst = modsDir / baseName;
            const fs::path stage = modsDir / ("." + baseName + ".tmp");
            fs::remove_all(stage, ec); ec.clear();
            fs::copy(localPath, stage,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                fs::remove_all(stage, ec);
                fail("could not copy mod folder: " + ec.message());
                return;
            }
            if (!CommitDir(stage, dst, err)) { fail(err); return; }
            done(ModPackInstalledKind::ScriptMod, baseName);
        } else {
            const fs::path dst = modsDir / (baseName + ".zip");
            if (!CommitFile(srcFile, dst, err)) {
                if (!partPath.empty()) fs::remove(partPath, ec);
                fail(err);
                return;
            }
            done(ModPackInstalledKind::ScriptMod, baseName + ".zip");
        }
        break;
    }
    case PayloadKind::NativePack: {
        s_phase.store(ModPackInstallPhase::Installing);
        SetStatus("Installing pack into mods/...");
        if (isDir) {
            const fs::path dst = modsDir / baseName;
            const fs::path stage = modsDir / ("." + baseName + ".tmp");
            fs::remove_all(stage, ec); ec.clear();
            fs::copy(localPath, stage,
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
            if (ec) {
                fs::remove_all(stage, ec);
                fail("could not copy pack folder: " + ec.message());
                return;
            }
            if (!CommitDir(stage, dst, err)) { fail(err); return; }
            done(ModPackInstalledKind::TexturePack, baseName);
        } else if (isPng) {
            // Single loose texture: install under its own name (must
            // already be in the native grammar to bind; GLideN64-named
            // files get renamed through the map).
            std::string key;
            if (!LooksLikePackPng(srcBase)) {
                if (ParseGlidenName(srcBase, &key) && GlidenMapReady()) {
                    const auto it = s_glidenMap.find(key);
                    if (it != s_glidenMap.end()) {
                        const std::string outName = NativePackName(it->second, key);
                        const fs::path dst = modsDir / outName;
                        if (!CommitFile(srcFile, dst, err)) { fail(err); return; }
                        done(ModPackInstalledKind::TexturePack, outName);
                        return;
                    }
                }
                fail("PNG is not in the pack grammar (\"#<hash>#<fmt>#<siz>.png\") "
                     "or the GLideN64/Rice grammar");
                return;
            }
            const fs::path dst = modsDir / srcBase;
            if (!CommitFile(srcFile, dst, err)) { fail(err); return; }
            done(ModPackInstalledKind::TexturePack, srcBase);
        } else {
            const fs::path dst = modsDir / (baseName + ".zip");
            if (!CommitFile(srcFile, dst, err)) {
                if (!partPath.empty()) fs::remove(partPath, ec);
                fail(err);
                return;
            }
            done(ModPackInstalledKind::TexturePack, baseName + ".zip");
        }
        break;
    }
    case PayloadKind::GlidenPack: {
        if (!GlidenMapReady()) {
            if (z) zip_close(z);
            if (!partPath.empty()) fs::remove(partPath, ec);
            fail("this build has no GLideN64 conversion table "
                 "(bundled-data/gliden64-map.json missing)");
            return;
        }
        s_phase.store(ModPackInstallPhase::Converting);
        SetStatus("Converting texture names...");
        const fs::path stage = modsDir / ("." + baseName + ".tmp");
        fs::remove_all(stage, ec); ec.clear();
        fs::create_directories(stage, ec);
        ConvertStats st;
        const bool ok = z ? ConvertGlidenZip(z, stage, st, err)
                          : ConvertGlidenDir(localPath, stage, st, err);
        if (z) zip_close(z);
        z = nullptr;
        if (!ok) {
            fs::remove_all(stage, ec);
            if (!partPath.empty()) fs::remove(partPath, ec);
            fail(err);
            return;
        }
        if (cancelled()) {
            fs::remove_all(stage, ec);
            if (!partPath.empty()) fs::remove(partPath, ec);
            return;
        }
        s_phase.store(ModPackInstallPhase::Installing);
        SetStatus("Installing converted pack...");
        const fs::path dst = modsDir / baseName;
        if (!CommitDir(stage, dst, err)) {
            if (!partPath.empty()) fs::remove(partPath, ec);
            fail(err);
            return;
        }
        if (!partPath.empty()) fs::remove(partPath, ec);
        done(ModPackInstalledKind::TexturePack,
             baseName + fmt::format(" ({} textures converted, {} skipped)",
                                    st.matched, st.unmatched));
        break;
    }
    case PayloadKind::Unknown:
    default: {
        if (z) zip_close(z);
        if (!partPath.empty()) fs::remove(partPath, ec);
        fail("unrecognized content — expected a mod (manifest.json), a "
             "native texture pack, or a GLideN64/Rice pack");
        return;
    }
    }

    if (z) zip_close(z);
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
