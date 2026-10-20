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
#include "ModPackDownloader_internal.h"

#include <ship/Context.h>
#include <ship/window/FileDropMgr.h>

#include <spdlog/spdlog.h>

#include <atomic>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

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
// ---------------------------------------------------------------------------
// Active-download handle (for cancel)
// ---------------------------------------------------------------------------

// The in-flight transfer, published by the worker under s_childMutex so
// CancelModPackInstall() (UI thread) can cancel it. Cleared by the
// worker before it disposes the handle — a cancel that misses the
// window finds nullptr and becomes a no-op.
std::mutex s_childMutex;
Download* s_download = nullptr;
} // namespace


namespace mpd {

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

void SetPhase(ModPackInstallPhase p) { s_phase.store(p); }
void SetLastKind(ModPackInstalledKind k) { s_lastKind.store(k); }
uint64_t Generation() { return s_generation.load(); }

void SetActiveDownload(Download* d) {
    std::lock_guard<std::mutex> lock(s_childMutex);
    s_download = d;
}

void KillActiveDownload() {
    std::lock_guard<std::mutex> lock(s_childMutex);
    if (s_download != nullptr) {
        DownloadCancel(s_download);
    }
}

// ---------------------------------------------------------------------------
// Path/name hygiene
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
static void TrimNameEdges(std::string& out) {
    while (!out.empty() && (out.front() == ' ' || out.front() == '.')) {
        out.erase(out.begin());
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        out.pop_back();
    }
}

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
    TrimNameEdges(out);
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

/* mods/ root — the dir HiResPack scans and "Open Mods Folder" opens.
 * Also scanned by MountModsDir for script mods (port.cpp walks both
 * this and the exe-adjacent mods/). */
std::filesystem::path ModsRoot() {
    return Ship::Context::GetPathRelativeToAppDirectory("mods");
}

} // namespace mpd

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
        return (fs::path(mpd::ModsRoot()) / pack.installFileName).string();
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

void LaunchWorker(mpd::InstallJob job) {
    s_generation.fetch_add(1);
    mpd::SetStatus({});
    s_lastKind.store(ModPackInstalledKind::None);
    const uint64_t gen = s_generation.load();
    std::thread([job = std::move(job), gen]() mutable {
        mpd::InstallWorker(std::move(job), gen);
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
    mpd::InstallJob job;
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
        mpd::SetStatus("Error: not an http(s) URL");
        s_phase.store(ModPackInstallPhase::Error);
        return;
    }
    mpd::InstallJob job;
    job.label = u;
    job.url = u;
    LaunchWorker(std::move(job));
}

void StartPackInstallFromPathAsync(const std::string& path) {
    if (!CanStartNewInstall()) return;
    std::error_code ec;
    const std::filesystem::path p(path);
    if (!std::filesystem::exists(p, ec)) {
        mpd::SetStatus("Error: path does not exist");
        s_phase.store(ModPackInstallPhase::Error);
        return;
    }
    mpd::InstallJob job;
    job.label = p.filename().string();
    job.localPath = p.string();
    LaunchWorker(std::move(job));
}

void CancelModPackInstall() {
    s_generation.fetch_add(1);
    mpd::KillActiveDownload(); // unblock the worker's transfer wait promptly
    mpd::SetPartPath({});
    s_phase.store(ModPackInstallPhase::Idle);
    mpd::SetStatus({});
}

ModPackInstallPhase GetModPackInstallPhase() { return s_phase.load(); }

std::string GetModPackInstallStatus() {
    std::lock_guard<std::mutex> lock(s_stateMutex);
    return s_status;
}

ModPackInstalledKind GetLastInstalledPackKind() { return s_lastKind.load(); }

bool IsGliden64ConverterReady() { return mpd::GlidenMapReady(); }

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
    const std::string ext = mpd::LowerExt(p.filename().string());
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
