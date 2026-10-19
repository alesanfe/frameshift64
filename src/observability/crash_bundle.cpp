/**
 * crash_bundle.cpp — see crash_bundle.h for the two-phase design.
 */

#include "crash_bundle.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>
#include "port_log.h"

#ifdef SSB64_HAVE_CURL
#include "http_curl.h"
#include "netplay_server.h"
#include <nlohmann/json.hpp>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#ifndef BATTLESHIP_CURRENT_VERSION
#define BATTLESHIP_CURRENT_VERSION "dev"
#endif
#ifndef BATTLESHIP_BUILD_SHA
#define BATTLESHIP_BUILD_SHA "dev"
#endif

extern "C" {
unsigned char port_diag_get_scene_curr(void);
unsigned char port_diag_get_scene_prev(void);
const char *port_diag_get_scene_name(unsigned char id);
}

namespace {

constexpr size_t kMaxBundlesKept = 8;

char sCrashDir[1024];
bool sCrashDirSet = false;
bool sSafeModeActive = false;

/* ── crash-marker writer (signal-handler side) ─────────────────────────── */

/* Manual hex/decimal formatting — snprintf is not on the POSIX
 * async-signal-safe list, so the marker writer can't use it.
 * All appends take `cap` (sizeof the body buffer) and saturate at
 * cap-1 so a long reason can't smash the stack inside the handler. */
size_t AppendStr(char *buf, size_t pos, const char *s, size_t cap) {
    while (*s && pos + 1 < cap) buf[pos++] = *s++;
    return pos;
}

size_t AppendHex(char *buf, size_t pos, uintptr_t v, size_t cap) {
    static const char kHex[] = "0123456789abcdef";
    char tmp[32];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v != 0) {
        tmp[n++] = kHex[v & 0xF];
        v >>= 4;
    }
    if (pos + 2 < cap) { buf[pos++] = '0'; buf[pos++] = 'x'; }
    while (n > 0 && pos + 1 < cap) buf[pos++] = tmp[--n];
    return pos;
}

size_t AppendDec(char *buf, size_t pos, unsigned long v, size_t cap) {
    char tmp[24];
    int n = 0;
    if (v == 0) {
        tmp[n++] = '0';
    }
    while (v != 0) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0 && pos + 1 < cap) buf[pos++] = tmp[--n];
    return pos;
}

size_t AppendChar(char *buf, size_t pos, char c, size_t cap) {
    if (pos + 1 < cap) buf[pos++] = c;
    return pos;
}

/* ── boot-time collector ───────────────────────────────────────────────── */

std::string DirOf(const std::string &path) {
    size_t sep = path.find_last_of("/\\");
    return (sep == std::string::npos) ? std::string(".") : path.substr(0, sep);
}

std::string UtcStamp() {
    using namespace std::chrono;
    std::time_t t = system_clock::to_time_t(system_clock::now());
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

/* Pull "pid=<n>" out of the marker text so bundle names stay unique when
 * two crashes land within the same second. */
std::string MarkerField(const std::string &marker, const char *key) {
    std::string prefix = std::string(key) + "=";
    size_t at = marker.find(prefix);
    if (at == std::string::npos) return "";
    at += prefix.size();
    size_t end = marker.find('\n', at);
    return marker.substr(at, end == std::string::npos ? end : end - at);
}

void RotateBundles(const std::filesystem::path &root) {
    std::error_code ec;
    std::vector<std::filesystem::path> dirs;
    for (const auto &e : std::filesystem::directory_iterator(root, ec)) {
        if (e.is_directory()) dirs.push_back(e.path());
    }
    /* Names are UTC-stamped so lexicographic order == chronological. */
    std::sort(dirs.begin(), dirs.end());
    while (dirs.size() > kMaxBundlesKept) {
        std::filesystem::remove_all(dirs.front(), ec);
        dirs.erase(dirs.begin());
    }
}

} // namespace

extern "C" void port_diag_set_crash_dir(const char *dir) {
    if (dir == nullptr || dir[0] == '\0') {
        sCrashDirSet = false;
        return;
    }
    std::strncpy(sCrashDir, dir, sizeof(sCrashDir) - 1);
    sCrashDir[sizeof(sCrashDir) - 1] = '\0';
    sCrashDirSet = true;
}

extern "C" void port_diag_set_safe_mode(int active) {
    sSafeModeActive = (active != 0);
}

extern "C" int port_diag_safe_mode_active(void) {
    return sSafeModeActive ? 1 : 0;
}

extern "C" void port_diag_request_safe_mode(void) {
    if (!sCrashDirSet || sCrashDir[0] == '\0') {
        return;
    }
    char path[1200];
    size_t n = std::strlen(sCrashDir);
    if (n + 18 >= sizeof(path)) return;
    std::memcpy(path, sCrashDir, n);
    std::memcpy(path + n, "/safemode.request", 18); /* includes NUL */
    FILE* f = fopen(path, "w");
    if (f != nullptr) {
        fputs("reason=user-requested\n", f);
        fclose(f);
    }
}

extern "C" void port_diag_write_crash_marker(const char *reason, uintptr_t fault_addr) {
    if (!sCrashDirSet || sCrashDir[0] == '\0') {
        return;
    }

    char path[1200];
    size_t n = std::strlen(sCrashDir);
    if (n + 14 >= sizeof(path)) return;
    std::memcpy(path, sCrashDir, n);
    std::memcpy(path + n, "/crash.marker", 14); /* includes NUL */

    char body[768];
    const size_t cap = sizeof(body);
    size_t pos = 0;
    pos = AppendStr(body, pos, "reason=", cap);
    pos = AppendStr(body, pos, reason != nullptr ? reason : "unknown", cap);
    pos = AppendChar(body, pos, '\n', cap);
    pos = AppendStr(body, pos, "fault_addr=", cap);
    pos = AppendHex(body, pos, fault_addr, cap);
    pos = AppendChar(body, pos, '\n', cap);
    pos = AppendStr(body, pos, "scene=", cap);
    pos = AppendDec(body, pos, port_diag_get_scene_curr(), cap);
    pos = AppendChar(body, pos, '\n', cap);
    pos = AppendStr(body, pos, "scene_prev=", cap);
    pos = AppendDec(body, pos, port_diag_get_scene_prev(), cap);
    pos = AppendChar(body, pos, '\n', cap);
    pos = AppendStr(body, pos, "pid=", cap);
#if defined(_WIN32)
    pos = AppendDec(body, pos, (unsigned long)_getpid(), cap);
#else
    pos = AppendDec(body, pos, (unsigned long)getpid(), cap);
#endif
    pos = AppendChar(body, pos, '\n', cap);

#if defined(_WIN32)
    HANDLE hf = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hf == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hf, body, (DWORD)pos, &written, nullptr);
    CloseHandle(hf);
#else
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    ssize_t off = 0;
    while (off < (ssize_t)pos) {
        ssize_t w = write(fd, body + off, pos - (size_t)off);
        if (w <= 0) break;
        off += w;
    }
    close(fd);
#endif
}

namespace ssb64::diag {

std::string CollectPendingCrashBundle(const std::string &log_path) {
    namespace fs = std::filesystem;
    try {
        const fs::path log_dir = DirOf(log_path);
        const fs::path marker = log_dir / "crash.marker";
        std::error_code ec;
        if (!fs::exists(marker, ec)) {
            return "";
        }

        std::string marker_text;
        {
            std::ifstream in(marker, std::ios::binary);
            marker_text.assign(std::istreambuf_iterator<char>(in),
                               std::istreambuf_iterator<char>());
        }

        const fs::path root = log_dir / "crashes";
        fs::create_directories(root, ec);

        std::string name = "crash-" + UtcStamp();
        const std::string pid = MarkerField(marker_text, "pid");
        if (!pid.empty()) name += "-p" + pid;
        const fs::path bundle = root / name;
        fs::create_directories(bundle, ec);
        if (ec) return "";

        /* crash_info.txt: marker fields + build identity + scene names so a
         * report is actionable without asking the user for versions. */
        {
            std::ofstream info(bundle / "crash_info.txt", std::ios::binary);
            info << marker_text
                 << "version=" << BATTLESHIP_CURRENT_VERSION << '\n'
                 << "build_sha=" << BATTLESHIP_BUILD_SHA << '\n'
                 << "platform="
#if defined(_WIN32)
                 << "windows"
#elif defined(__APPLE__)
                 << "macos"
#elif defined(__ANDROID__)
                 << "android"
#elif defined(__SWITCH__)
                 << "switch"
#else
                 << "linux"
#endif
                 << '\n'
                 << "collected_at=" << UtcStamp() << '\n';
            const std::string scene = MarkerField(marker_text, "scene");
            if (!scene.empty()) {
                info << "scene_name="
                     << port_diag_get_scene_name(
                            (unsigned char)std::strtoul(scene.c_str(), nullptr, 10))
                     << '\n';
            }
        }

        /* previous.log: the dead run's ssb64.log — holds the crash
         * handler's backtrace. Copied BEFORE port_log_init truncates it. */
        std::error_code ec2;
        if (fs::exists(log_path, ec2) && fs::file_size(log_path, ec2) > 0) {
            fs::copy_file(log_path, bundle / "previous.log",
                          fs::copy_options::overwrite_existing, ec2);
        }

#if defined(_WIN32)
        /* Minidumps land in the process CWD (portWriteMinidump) — sweep any
         * strays into the bundle so they travel with the report. Each dir
         * gets a fresh error_code so one unreadable dir doesn't skip the
         * other. */
        std::error_code cwd_ec;
        const fs::path cwd = fs::current_path(cwd_ec);
        for (const fs::path &dir : {cwd, log_dir.parent_path()}) {
            if (dir.empty()) continue;
            std::error_code dec;
            for (const auto &e : fs::directory_iterator(dir, dec)) {
                const std::string fn = e.path().filename().string();
                if (e.is_regular_file() && fn.rfind("crash_", 0) == 0 &&
                    e.path().extension() == ".dmp") {
                    fs::rename(e.path(), bundle / fn, dec);
                }
            }
        }
#endif

        fs::remove(marker, ec);
        RotateBundles(root);
        return bundle.string();
    } catch (...) {
        return ""; /* diagnostics must never break boot */
    }
}

/* ------------------------------------------------------------------ */
/* Opt-in crash telemetry — gCrashReports.Enabled (default OFF). The   */
/* user opts in once; every later crash bundle uploads itself on the   */
/* NEXT launch (a crash handler can't POST mid-fault).                 */
/*                                                                    */
/* What travels: build version/sha, platform, crash reason, fault     */
/* address, scene id/name, and the last 8 KiB of the dead run's log    */
/* (the backtrace). What never travels: minidumps (they may contain    */
/* heap contents), save files, config, or any path outside the log.    */
/* The POST goes to the configured netplay lobby — the same server the */
/* leaderboard already uses; a self-hosted lobby collects its own      */
/* crash data, nobody else's.                                          */
/* ------------------------------------------------------------------ */

namespace {

std::string sPendingBundle;

#ifdef SSB64_HAVE_CURL
/* ws://host/... -> http://host, wss:// -> https://, path stripped. */
std::string LobbyHttpBase() {
    std::string s = CVarGetString("gNetplay.Server", "");
    if (s.empty()) {
        s = PORT_NETLOBBY_DEFAULT_SERVER;
    }
    bool secure = true;
    if (s.rfind("ws://", 0) == 0)  { secure = false; s.erase(0, 5); }
    else if (s.rfind("wss://", 0) == 0) { s.erase(0, 6); }
    else if (s.rfind("http://", 0) == 0)  { secure = false; s.erase(0, 7); }
    else if (s.rfind("https://", 0) == 0) { s.erase(0, 8); }
    while (!s.empty() && (s.back() == '/' || s.back() == ' ')) {
        s.pop_back();
    }
    return s.empty() ? s : std::string(secure ? "https://" : "http://") + s;
}

std::string ReadText(const std::filesystem::path &p, size_t max_bytes, bool tail) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(p, ec);
    if (ec) return "";
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::string out;
    if (tail && size > max_bytes) {
        in.seekg(static_cast<std::streamoff>(size) - static_cast<std::streamoff>(max_bytes));
        std::string discard;
        std::getline(in, discard);          /* skip the torn first line */
    }
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    if (out.size() > max_bytes) out.resize(max_bytes);
    return out;
}

void UploadThread(std::string bundle_dir) {
    /* crash_info.txt is "k=v" lines; ship them verbatim (they're already
     * sanitized build/crash fields). previous.log ships only its tail —
     * the backtrace sits at the end. */
    const std::string info = ReadText(std::filesystem::path(bundle_dir) / "crash_info.txt", 8192, false);
    const std::string logtail = ReadText(std::filesystem::path(bundle_dir) / "previous.log", 8192, true);
    if (info.empty() && logtail.empty()) {
        return;
    }
    nlohmann::json doc;
    doc["kind"] = "crash";
    doc["info"] = info;
    doc["log_tail"] = logtail;
    std::string body, err;
    if (ssb64::CurlPostToString(LobbyHttpBase() + "/crash", doc.dump(),
                                "application/json", &body, &err, 15)) {
        port_log("SSB64: crash report uploaded (thanks — it helps fix it)\n");
    } else {
        port_log("SSB64: crash report upload failed: %s\n", err.c_str());
    }
}

#endif /* SSB64_HAVE_CURL */

} // namespace

void QueueCrashReport(const std::string &bundle_dir) {
    sPendingBundle = bundle_dir;
}

void MaybeUploadCrashReport() {
    if (sPendingBundle.empty() || CVarGetInteger("gCrashReports.Enabled", 0) == 0) {
        return;
    }
#ifdef SSB64_HAVE_CURL
    std::thread t(UploadThread, sPendingBundle);
    t.detach();
#endif
    sPendingBundle.clear();
}

} // namespace ssb64::diag
