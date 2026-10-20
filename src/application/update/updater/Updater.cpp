/* Smash Classic auto-updater, modeled on Jump Circuit's (autoload/updater.gd):
 *
 *   - On launch, ask GitHub for the latest release of kRepo. Only a strictly
 *     newer semantic version counts as an update.
 *   - The platform asset must download from this repo's releases (nothing
 *     else is trusted) and is verified against the size and SHA-256 digest
 *     GitHub reports for it.
 *   - Windows: a hidden PowerShell installer waits for the game to exit,
 *     unpacks the zip into a staging folder, checks it holds a complete
 *     build, backs up the files it will replace, copies the new files over
 *     the install, and relaunches the game. Any failure restores the backup
 *     and reopens the previous version. Saves, settings and the ROM-derived
 *     BattleShip.o2r are never in the zip, so they are never touched.
 *   - "Skip this version" is remembered; --no-update-check (or
 *     SSB64_NO_UPDATE_CHECK=1) disables the launch check.
 *
 * Linux (AppImage) and macOS (DMG) keep the upstream download flow. */
#include "enhancements.h"

#include "FileHash.h" // Sha256File + DownloadedFileMatches (shared with ModPackDownloader)
#include "ReleaseSig.h" // TD-009: signed SHA256SUMS.txt verification
#include "http_curl.h" // CurlGetToString / CurlDownloadToFile — libcurl in-process
#include "build_version.h" // BATTLESHIP_CURRENT_VERSION / BATTLESHIP_BUILD_SHA
#include "port_log.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iterator>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>
#endif

namespace ssb64 {
namespace enhancements {

static std::atomic<bool> s_updateChecked{false};
static std::atomic<bool> s_updateAvailable{false};
static std::atomic<bool> s_isDownloading{false};
static std::atomic<bool> s_downloadComplete{false};
static std::atomic<bool> s_isCheckingForUpdates{false};
static std::atomic<bool> s_updateCheckFailed{false};
static std::atomic<bool> s_quitForUpdate{false};

static std::string s_latestVersion;
static std::string s_releaseNotes;
static std::string s_downloadUrl;
static long long s_assetSize = 0;
static std::string s_assetDigest;
static std::string s_sumsUrl;  /* SHA256SUMS.txt release asset URL */
static std::string s_sigUrl;   /* SHA256SUMS.txt.minisig release asset URL */
static std::string s_updateStatus;
static std::string s_downloadStatus;
static std::mutex s_stringMutex;

namespace {

constexpr const char* kRepo = "fatty8me2/smash-classic";
constexpr const char* kLatestReleaseApi = "https://api.github.com/repos/fatty8me2/smash-classic/releases/latest";
constexpr const char* kReleasePageUrl = "https://github.com/fatty8me2/smash-classic/releases/latest";
constexpr const char* kDownloadPrefix = "https://github.com/fatty8me2/smash-classic/releases/download/";
constexpr const char* kSkippedVersionCVar = "gUpdater.SkippedVersion";

#if defined(__linux__)
constexpr const char* kPlatformAssetName = "BattleShip-x86_64.AppImage";
#elif defined(_WIN32)
constexpr const char* kPlatformAssetName = "BattleShip-windows.zip";
#elif defined(__APPLE__)
constexpr const char* kPlatformAssetName = "BattleShip.dmg";
#else
constexpr const char* kPlatformAssetName = "";
#endif

#if defined(_WIN32)
/* Embedded Windows installer (see the file header). Runs hidden under
 * Windows PowerShell 5.1, which every supported Windows ships. */
constexpr const char* kWindowsInstallerScript = R"PS(
param(
    [int]$WaitForPid,
    [string]$ZipPath,
    [string]$InstallDir,
    [string]$GamePath,
    [string]$Version
)
$ErrorActionPreference = "Stop"
# Tell the waiting game we are running: only then does it quit and hand over.
try { Set-Content -LiteralPath ($ZipPath + ".started") -Value $PID -ErrorAction Stop } catch { }
$stage = Join-Path $env:TEMP ("SmashClassic-update-" + [guid]::NewGuid().ToString("N"))
$unpacked = Join-Path $stage "new"
$backup = Join-Path $stage "backup"
$required = @("BattleShip.exe", "torch.exe", "f3d.o2r", "config.yml")
$installed = @()
try {
    try { Wait-Process -Id $WaitForPid -Timeout 60 -ErrorAction Stop } catch { }
    New-Item -ItemType Directory -Path $unpacked -Force | Out-Null
    Expand-Archive -LiteralPath $ZipPath -DestinationPath $unpacked -Force
    foreach ($name in $required) {
        if (-not (Test-Path -LiteralPath (Join-Path $unpacked $name) -PathType Leaf)) {
            throw ("the update is missing " + $name)
        }
    }
    $files = Get-ChildItem -LiteralPath $unpacked -Recurse -File -Force |
        ForEach-Object { $_.FullName.Substring($unpacked.Length + 1) }
    New-Item -ItemType Directory -Path $backup -Force | Out-Null
    try {
        foreach ($rel in $files) {
            $destination = Join-Path $InstallDir $rel
            if (Test-Path -LiteralPath $destination -PathType Leaf) {
                $saved = Join-Path $backup $rel
                New-Item -ItemType Directory -Path (Split-Path $saved) -Force | Out-Null
                Copy-Item -LiteralPath $destination -Destination $saved -Force
            }
            New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
            Copy-Item -LiteralPath (Join-Path $unpacked $rel) -Destination $destination -Force
            $installed += $rel
        }
    } catch {
        foreach ($rel in $installed) {
            $destination = Join-Path $InstallDir $rel
            $saved = Join-Path $backup $rel
            try {
                if (Test-Path -LiteralPath $saved -PathType Leaf) {
                    Copy-Item -LiteralPath $saved -Destination $destination -Force
                } else {
                    Remove-Item -LiteralPath $destination -Force
                }
            } catch { }
        }
        throw
    }
    Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath ($ZipPath + ".started") -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $PSCommandPath -Force -ErrorAction SilentlyContinue
    Start-Process -FilePath $GamePath -WorkingDirectory $InstallDir
} catch {
    $message = "Could not install Smash Classic " + $Version + ": " + $_.Exception.Message + "`nThe previous version will be opened."
    try {
        Add-Type -AssemblyName System.Windows.Forms
        [System.Windows.Forms.MessageBox]::Show($message, "Smash Classic Update", "OK", "Error") | Out-Null
    } catch { }
    Remove-Item -LiteralPath $ZipPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath ($ZipPath + ".started") -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
    if (Test-Path -LiteralPath $GamePath -PathType Leaf) {
        Start-Process -FilePath $GamePath -WorkingDirectory $InstallDir
    }
}
)PS";
#endif

#if !defined(_WIN32)
/* fork+execvp a helper with no shell in the middle — argv entries reach
 * the child verbatim, so nothing needs quoting. stdio is redirected to
 * /dev/null and a detached reaper thread collects the exit status so the
 * helper can't leave a zombie behind. strip_ld_env drops the AppImage
 * loader paths in the child so system tools see the distro libraries. */
void SpawnDetached(const char* const argv[], bool strip_ld_env) {
    const pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        const int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        if (strip_ld_env) {
            unsetenv("LD_LIBRARY_PATH");
            unsetenv("LD_PRELOAD");
        }
        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }
    std::thread([pid] {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }).detach();
}
#endif

void SetUpdateStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(s_stringMutex);
    s_updateStatus = status;
}

void SetDownloadStatus(const std::string& status) {
    std::lock_guard<std::mutex> lock(s_stringMutex);
    s_downloadStatus = status;
}

/* "v1.2.3" / "1.2" -> {1,2,3}; anything non-numeric ends the version. */
std::vector<int> ParseVersion(std::string v) {
    std::vector<int> parts;
    size_t at = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    while (at < v.size()) {
        size_t end = at;
        while (end < v.size() && v[end] >= '0' && v[end] <= '9') end++;
        if (end == at) break;
        parts.push_back(std::atoi(v.substr(at, end - at).c_str()));
        if (end >= v.size() || v[end] != '.') break;
        at = end + 1;
    }
    return parts;
}

/* -1 / 0 / 1. An unparseable current version ("dev") is older than anything. */
int CompareVersions(const std::string& a, const std::string& b) {
    std::vector<int> pa = ParseVersion(a);
    std::vector<int> pb = ParseVersion(b);
    if (pa.empty() || pb.empty()) {
        return pa.empty() == pb.empty() ? 0 : (pa.empty() ? -1 : 1);
    }
    for (size_t i = 0; i < pa.size() || i < pb.size(); i++) {
        int va = (i < pa.size()) ? pa[i] : 0;
        int vb = (i < pb.size()) ? pb[i] : 0;
        if (va != vb) return (va < vb) ? -1 : 1;
    }
    return 0;
}

#if defined(_WIN32)
std::string ExecutablePath() {
    char path[MAX_PATH];
    DWORD n = GetModuleFileNameA(nullptr, path, MAX_PATH);
    return (n > 0 && n < MAX_PATH) ? std::string(path, n) : std::string();
}
#endif

/* In-process HTTP via libcurl (src/platform/http/http_curl.cpp). Replaces
 * the curl.exe subprocess + pipe dance: no shell quoting to audit, no
 * AppImage LD_LIBRARY_PATH strip, and progress comes from a real callback
 * instead of parsing `-#` output. */
bool FetchToString(const std::string& url, std::string* out, long timeoutSeconds = 600) {
    std::string err;
    if (!CurlGetToString(url, out, &err,
                       { "Accept: application/vnd.github+json" }, timeoutSeconds)) {
        port_log("Updater: fetch %s failed: %s\n", url.c_str(), err.c_str());
        return false;
    }
    return true;
}

bool FetchToFile(const std::string& url, const std::filesystem::path& destPath,
                 const CurlProgressFn& progress = nullptr) {
    std::string err;
    if (!CurlDownloadToFile(url, destPath, &err, progress)) {
        port_log("Updater: download %s failed: %s\n", url.c_str(), err.c_str());
        return false;
    }
    return true;
}

/* Only this repo's release downloads are trusted — keep rejecting URLs
 * outside our releases even though there's no longer a shell line for
 * them to escape into. */
bool IsTrustedDownloadUrl(const std::string& url) {
    if (url.rfind(kDownloadPrefix, 0) != 0) return false;
    for (char c : url) {
        if (c == '"' || c == '\'' || c == '\\' || c <= 0x20 || c == 0x7F) return false;
    }
    return true;
}

void ResetDownloadStateForNewCheck() {
    s_downloadComplete.store(false);
    SetDownloadStatus("");
}

#ifndef __SWITCH__

/* Snapshot of the shared update state the download thread works from —
 * taken once under s_stringMutex so the thread never touches the
 * globals again. */
struct UpdateJob {
    std::string url;
    long long expectedSize;
    std::string expectedDigest;
    std::string version;
    std::string sumsUrl;
    std::string sigUrl;
};

bool UpdateDownloadProgress(unsigned long long now, unsigned long long total) {
    if (total == 0) return true;
    std::ostringstream s;
    s << "Downloading... " << std::fixed << std::setprecision(1)
      << (100.0 * static_cast<double>(now) / static_cast<double>(total)) << "%";
    SetDownloadStatus(s.str());
    return true;
}

void FailUpdate(const std::string& message) {
    SetDownloadStatus("Error: " + message);
    s_isDownloading.store(false);
}

bool ReadTextFile(const std::string& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return !out.empty();
}

/* TD-009 — the digest GitHub's API reports for the asset travels the
 * same channel as the payload, so it proves integrity, not authenticity.
 * When a release pubkey is embedded, the authoritative digest instead
 * comes from SHA256SUMS.txt verified against its minisign signature — a
 * forged release would need the offline signing key, not just GitHub API
 * access. `tmpPath` is the platform's already-downloaded artifact path
 * (the manifest files park beside it as *.sums / *.minisig). On success
 * `digest` holds the authoritative "sha256:<hex>" for this platform's
 * asset. */
bool FetchAuthoritativeDigest(const UpdateJob& job, const std::string& tmpPath,
                              std::string& digest, std::string& err) {
    if (!ReleaseSignatureSupported()) {
        port_log_ex(PORT_LOG_WARN, "updater",
                    "Updater: WARNING — no release pubkey embedded; "
                    "falling back to unauthenticated API digest\n");
        digest = job.expectedDigest;
        return true;
    }
    if (job.sumsUrl.empty() || job.sigUrl.empty()) {
        err = "release lacks a signed manifest (SHA256SUMS.txt/.minisig)";
        return false;
    }
    const std::string sumsPath = tmpPath + ".sums";
    const std::string sigPath  = tmpPath + ".minisig";
    std::remove(sumsPath.c_str());
    std::remove(sigPath.c_str());
    if (!FetchToFile(job.sumsUrl, sumsPath) || !FetchToFile(job.sigUrl, sigPath)) {
        err = "could not download the signed release manifest";
        return false;
    }
    std::string sums, sig;
    const bool read_ok = ReadTextFile(sumsPath, sums) && ReadTextFile(sigPath, sig);
    std::remove(sumsPath.c_str());
    std::remove(sigPath.c_str());
    if (!read_ok) {
        err = "signed release manifest is empty";
        return false;
    }
    if (!VerifyReleaseSignature(sums, sig, err)) {
        return false;
    }
    digest = SumsDigestForAsset(sums, kPlatformAssetName);
    if (digest.empty()) {
        err = "signed manifest has no entry for this platform";
        return false;
    }
    return true;
}

/* Shared verify tail: authoritative digest + size/hash match. */
bool VerifyUpdateDownload(const UpdateJob& job, const std::string& tmpPath, std::string& err) {
    std::string authDigest;
    return FetchAuthoritativeDigest(job, tmpPath, authDigest, err) &&
           DownloadedFileMatches(tmpPath, job.expectedSize, authDigest, err);
}

/* The signed manifest assets may sit anywhere in the release's asset
 * list (the platform pass breaks early), so grab their URLs on a
 * separate pass. Only URLs passing the same trust gate as the payload
 * are kept — they go straight to libcurl. */
void GrabManifestUrls(const nlohmann::json& release) {
    if (!release.contains("assets") || !release["assets"].is_array()) {
        return;
    }
    std::string sums, sig;
    for (const auto& asset : release["assets"]) {
        if (!asset.contains("name") || !asset["name"].is_string() ||
            !asset.contains("browser_download_url") || !asset["browser_download_url"].is_string()) {
            continue;
        }
        const std::string an = asset["name"].get<std::string>();
        const std::string au = asset["browser_download_url"].get<std::string>();
        if (an == "SHA256SUMS.txt") sums = au;
        else if (an == "SHA256SUMS.txt.minisig") sig = au;
    }
    std::lock_guard<std::mutex> lock(s_stringMutex);
    s_sumsUrl = (!sums.empty() && IsTrustedDownloadUrl(sums)) ? sums : "";
    s_sigUrl  = (!sig.empty()  && IsTrustedDownloadUrl(sig))  ? sig  : "";
}

/* Locate this platform's asset in the release and publish its download
 * fields under the lock. */
/* True when the release asset is the platform package with a usable URL. */
static bool IsMatchingAsset(const nlohmann::json& asset) {
    return asset.contains("name") && asset["name"].is_string() &&
           asset["name"].get<std::string>() == kPlatformAssetName &&
           asset.contains("browser_download_url") && asset["browser_download_url"].is_string();
}

bool FindPlatformAsset(const nlohmann::json& release) {
    if (!release.contains("assets") || !release["assets"].is_array()) {
        return false;
    }
    for (const auto& asset : release["assets"]) {
        if (!IsMatchingAsset(asset)) {
            continue;
        }
        std::string url = asset["browser_download_url"].get<std::string>();
        long long size = (asset.contains("size") && asset["size"].is_number()) ? asset["size"].get<long long>() : 0;
        if (!IsTrustedDownloadUrl(url) || size <= 0) {
            port_log("Updater: rejecting release asset %s\n", url.c_str());
            return false;
        }
        std::lock_guard<std::mutex> lock(s_stringMutex);
        s_downloadUrl = url;
        s_assetSize = size;
        s_assetDigest = (asset.contains("digest") && asset["digest"].is_string())
                            ? asset["digest"].get<std::string>() : std::string();
        return true;
    }
    return false;
}

/* Parse the latest-release response and publish the outcome. Throws on
 * malformed JSON. */
void ProcessLatestRelease(const std::string& response) {
    auto release = nlohmann::json::parse(response);
    if (!release.contains("tag_name") || !release["tag_name"].is_string()) {
        throw std::runtime_error("latest release missing tag_name");
    }
    std::string latestTag = release["tag_name"].get<std::string>();
    std::string notes = (release.contains("body") && release["body"].is_string())
                            ? release["body"].get<std::string>() : std::string();
    {
        std::lock_guard<std::mutex> lock(s_stringMutex);
        s_latestVersion = latestTag;
        s_releaseNotes = notes;
        s_downloadUrl.clear();
        s_assetSize = 0;
        s_assetDigest.clear();
        s_sumsUrl.clear();
        s_sigUrl.clear();
    }
    if (CompareVersions(latestTag, BATTLESHIP_CURRENT_VERSION) <= 0) {
        SetUpdateStatus("Up to date (" + std::string(BATTLESHIP_CURRENT_VERSION) + ").");
        return;
    }
    GrabManifestUrls(release);
    if (FindPlatformAsset(release)) {
        s_updateAvailable.store(true);
        SetUpdateStatus("");
        port_log("Updater: %s available (have %s)\n", latestTag.c_str(), BATTLESHIP_CURRENT_VERSION);
    } else {
        s_updateCheckFailed.store(true);
        SetUpdateStatus("Latest release has no update for this platform.");
    }
}

#if defined(_WIN32)
/* Write the embedded installer script, launch it hidden, and wait for
 * its .started marker — only then is it safe for the game to quit. */
bool LaunchWindowsInstaller(const std::string& exeDir, const std::string& updatesDir,
                            const std::string& exePath, const std::string& zipPath,
                            const std::string& version, std::string& err) {
    std::string scriptPath = updatesDir + "\\install_update.ps1";
    {
        std::ofstream script(scriptPath, std::ios::binary | std::ios::trunc);
        script << kWindowsInstallerScript;
        if (!script) {
            err = "could not prepare the installer.";
            return false;
        }
    }
    char systemRoot[MAX_PATH] = "C:\\Windows";
    GetEnvironmentVariableA("SystemRoot", systemRoot, MAX_PATH);
    std::string cmd = std::string("\"") + systemRoot + "\\System32\\WindowsPowerShell\\v1.0\\powershell.exe\"" +
                      " -NoProfile -NonInteractive -WindowStyle Hidden -ExecutionPolicy Bypass" +
                      " -File \"" + scriptPath + "\"" +
                      " -WaitForPid " + std::to_string(GetCurrentProcessId()) +
                      " -ZipPath \"" + zipPath + "\"" +
                      " -InstallDir \"" + exeDir + "\"" +
                      " -GamePath \"" + exePath + "\"" +
                      " -Version \"" + version + "\"";
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    /* A hidden console of its own (PowerShell won't run detached from
     * any console). Break away from the game's job object if there is
     * one, so the installer outlives the game; not every job allows
     * that, so fall back to a plain launch. */
    std::string cmdBreakaway = cmd;
    if (!CreateProcessA(nullptr, cmdBreakaway.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB, nullptr, exeDir.c_str(), &si, &pi) &&
        !CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                        exeDir.c_str(), &si, &pi)) {
        err = "could not start the installer.";
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* Quit only once the installer is confirmed running; it waits for
     * this process to exit before touching any files. */
    std::string marker = zipPath + ".started";
    bool started = false;
    for (int i = 0; i < 300 && !started; i++) {     // PowerShell can take a while to cold-start
        Sleep(100);
        started = GetFileAttributesA(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (!started) {
        err = "the installer did not start.";
        return false;
    }
    return true;
}
#endif

void RunPlatformUpdate(const UpdateJob& job);

#if defined(_WIN32)
void RunPlatformUpdate(const UpdateJob& job) {
    std::string exePath = ExecutablePath();
    size_t slash = exePath.find_last_of("\\/");
    if (slash == std::string::npos) {
        FailUpdate("could not locate the install folder.");
        return;
    }
    std::string exeDir = exePath.substr(0, slash);
    std::string updatesDir = exeDir + "\\updates";
    CreateDirectoryA(updatesDir.c_str(), nullptr);
    std::string zipPath = updatesDir + "\\update.zip";
    std::remove(zipPath.c_str());
    std::remove((zipPath + ".started").c_str());

    if (!FetchToFile(job.url, zipPath, UpdateDownloadProgress)) {
        std::remove(zipPath.c_str());
        FailUpdate("download failed.");
        return;
    }
    SetDownloadStatus("Verifying...");
    {
        std::string err;
        if (!VerifyUpdateDownload(job, zipPath, err)) {
            std::remove(zipPath.c_str());
            FailUpdate(err);
            return;
        }
    }
    std::string err;
    if (!LaunchWindowsInstaller(exeDir, updatesDir, exePath, zipPath, job.version, err)) {
        FailUpdate(err);
        return;
    }
    SetDownloadStatus("Installing " + job.version + " - the game will restart...");
    s_downloadComplete.store(true);
    port_log("Updater: installer started for %s, closing the game\n", job.version.c_str());
    s_quitForUpdate.store(true);    // TickUpdater closes the window on the main thread
}
#elif defined(__linux__)
void RunPlatformUpdate(const UpdateJob& job) {
    const char* appImagePath = getenv("APPIMAGE");
    if (!appImagePath) {
        FailUpdate("not running as an AppImage.");
        return;
    }
    std::string tempPath = std::string(appImagePath) + ".part";
    if (!FetchToFile(job.url, tempPath, UpdateDownloadProgress)) {
        std::remove(tempPath.c_str());
        FailUpdate("download failed.");
        return;
    }
    SetDownloadStatus("Verifying...");
    {
        std::string err;
        if (!VerifyUpdateDownload(job, tempPath, err)) {
            std::remove(tempPath.c_str());
            FailUpdate(err);
            return;
        }
    }
    chmod(tempPath.c_str(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
    if (std::rename(tempPath.c_str(), appImagePath) != 0) {
        FailUpdate("could not replace the AppImage.");
        return;
    }
    SetDownloadStatus("Update complete! Please restart the game.");
    s_downloadComplete.store(true);
}
#elif defined(__APPLE__)
void RunPlatformUpdate(const UpdateJob& job) {
    std::string tempDmg = "/tmp/SmashClassic_Update.dmg";
    if (!FetchToFile(job.url, tempDmg, UpdateDownloadProgress)) {
        std::remove(tempDmg.c_str());
        FailUpdate("download failed.");
        return;
    }
    SetDownloadStatus("Verifying...");
    {
        std::string err;
        if (!VerifyUpdateDownload(job, tempDmg, err)) {
            std::remove(tempDmg.c_str());
            FailUpdate(err);
            return;
        }
    }
    SetDownloadStatus("Download complete! Opening DMG...");
    s_downloadComplete.store(true);
    const char* argv[] = { "open", tempDmg.c_str(), nullptr };
    SpawnDetached(argv, /*strip_ld_env=*/false);
}
#else
void RunPlatformUpdate(const UpdateJob&) {
}
#endif

#endif // __SWITCH__


bool LaunchCheckDisabled() {
    const char* env = std::getenv("SSB64_NO_UPDATE_CHECK");
    if (env != nullptr && env[0] != '\0' && env[0] != '0') return true;
#if defined(_WIN32)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool found = false;
    for (int i = 1; argv != nullptr && i < argc; i++) {
        if (wcscmp(argv[i], L"--no-update-check") == 0) found = true;
    }
    LocalFree(argv);
    return found;
#else
    return false;
#endif
}

} // namespace

std::string GetCurrentVersion() {
    return BATTLESHIP_CURRENT_VERSION;
}

void CheckForUpdatesAsync(bool force) {
#ifndef __SWITCH__
    if (s_isCheckingForUpdates.load() || s_isDownloading.load()) return;
    if (!force && s_updateChecked.load()) return;
    if (!force && LaunchCheckDisabled()) {
        s_updateChecked.store(true);
        SetUpdateStatus("Update check disabled.");
        return;
    }
    s_updateChecked.store(true);
    s_isCheckingForUpdates.store(true);
    s_updateCheckFailed.store(false);
    s_updateAvailable.store(false);
    ResetDownloadStateForNewCheck();
    SetUpdateStatus("");

    std::thread([]() {
        std::string response;
        if (!FetchToString(kLatestReleaseApi, &response, 10) || response.empty()) {
            s_updateCheckFailed.store(true);
            SetUpdateStatus("Unable to check for updates.");
            s_isCheckingForUpdates.store(false);
            return;
        }
        try {
            ProcessLatestRelease(response);
        } catch (...) {
            s_updateCheckFailed.store(true);
            SetUpdateStatus("Unable to read update information.");
        }
        s_isCheckingForUpdates.store(false);
    }).detach();
#endif
}

void StartGameUpdate() {
#ifndef __SWITCH__
    if (s_isDownloading.load()) return;
    s_isDownloading.store(true);

    UpdateJob job;
    {
        std::lock_guard<std::mutex> lock(s_stringMutex);
        if (s_downloadUrl.empty()) {
            s_downloadStatus = "Error: No update download available.";
            s_isDownloading.store(false);
            return;
        }
        s_downloadStatus = "Starting download...";
        job.url = s_downloadUrl;
        job.expectedSize = s_assetSize;
        job.expectedDigest = s_assetDigest;
        job.version = s_latestVersion;
        job.sumsUrl = s_sumsUrl;
        job.sigUrl = s_sigUrl;
    }

    std::thread([job]() {
        RunPlatformUpdate(job);
        s_isDownloading.store(false);
    }).detach();
#endif
}

/* Called once per frame on the main thread (PortPushFrame). */
void TickUpdater() {
    if (s_quitForUpdate.exchange(false)) {
        if (auto context = Ship::Context::GetInstance()) {
            if (auto window = context->GetWindow()) {
                window->Close();
            }
        }
    }
}

void SkipLatestVersion() {
    std::string version = GetLatestVersion();
    CVarSetString(kSkippedVersionCVar, version.c_str());
    if (auto context = Ship::Context::GetInstance()) {
        if (auto window = context->GetWindow()) {
            if (auto gui = window->GetGui()) gui->SaveConsoleVariablesNextFrame();
        }
    }
}

bool IsLatestVersionSkipped() {
    return GetLatestVersion() == CVarGetString(kSkippedVersionCVar, "");
}

void OpenReleasePage() {
#ifndef __SWITCH__
    const char* url = kReleasePageUrl;
#if defined(_WIN32)
    ShellExecuteA(nullptr, "open", url, nullptr, nullptr, SW_SHOWNORMAL);
#elif defined(__APPLE__)
    const char* argv[] = { "open", url, nullptr };
    SpawnDetached(argv, /*strip_ld_env=*/false);
#else
    const char* argv[] = { "xdg-open", url, nullptr };
    SpawnDetached(argv, /*strip_ld_env=*/true);
#endif
#endif
}

bool IsCheckingForUpdates() { return s_isCheckingForUpdates.load(); }
bool IsUpdateAvailable() { return s_updateAvailable.load(); }
bool DidUpdateCheckFail() { return s_updateCheckFailed.load(); }
bool IsDownloading() { return s_isDownloading.load(); }
bool IsDownloadComplete() { return s_downloadComplete.load(); }

std::string GetUpdateStatus() { std::lock_guard<std::mutex> lock(s_stringMutex); return s_updateStatus; }
std::string GetDownloadStatus() { std::lock_guard<std::mutex> lock(s_stringMutex); return s_downloadStatus; }
std::string GetLatestVersion() { std::lock_guard<std::mutex> lock(s_stringMutex); return s_latestVersion; }
std::string GetReleaseNotes() { std::lock_guard<std::mutex> lock(s_stringMutex); return s_releaseNotes; }

} // namespace enhancements
} // namespace ssb64
