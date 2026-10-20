// No-op implementations of desktop-only port APIs for the browser build.
// The real files are excluded from SSB64_SRC_PORT on Emscripten: browsers
// have no subprocesses (updater, Torch first-run wizard, download service),
// no OS threads without SharedArrayBuffer (hang watchdog), no native
// window chrome (window icon), and no raw HTTP (curl downloaders).
// Menu/init code still references these symbols, so the link needs inert
// stand-ins — same pattern as port_switch_stubs.cpp and UpdaterStubs.cpp.
// Lives in src/application/ because the stubbed APIs are application-layer:
// src/platform/ may not include application headers (check_module_deps).
#include "first_run.h"
#include "port_window_icon.h"
#include "port_watchdog.h"
#include "renderdoc_trigger.h"
#include "ReleaseSig.h"
#include "enhancements.h"

#ifdef __EMSCRIPTEN__

#include <emscripten.h>
#include <emscripten/console.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>

namespace {

/* Browser env bridge: the wasm `environ` starts empty and there is no
 * exported setenv, so lobby/netplay bootstrap knobs
 * (SSB64_LOBBY_HOST/JOIN/CODE/... in netlobby_start.cpp) would be
 * unreachable from a browser session. The loader page passes them as URL
 * query params instead — either the raw env name (?SSB64_LOBBY_JOIN=...)
 * or a short ?lobby_* alias — and this pre-main constructor copies them
 * into the C environment before PortInit or any lazy getenv() static
 * can cache a miss.
 *
 * URLSearchParams already percent-decodes, so values may hold ws:// URLs
 * verbatim. Names are whitelisted (SSB64_* prefix or the alias table) and
 * any decoded value containing a newline or '=' is dropped so the
 * NAME=VALUE\n wire format to C can't be smuggled into extra vars. */
__attribute__((constructor)) void port_web_apply_url_env() {
    char* blob = emscripten_run_script_string(
        "(function(){"
        "var p=new URLSearchParams(globalThis.location?location.search:'');"
        "var alias={"
        "lobby_host:'SSB64_LOBBY_HOST',lobby_join:'SSB64_LOBBY_JOIN',"
        "lobby_code:'SSB64_LOBBY_CODE',lobby_spectate:'SSB64_LOBBY_SPECTATE',"
        "lobby_code_file:'SSB64_LOBBY_CODE_FILE',"
        "lobby_autostart:'SSB64_LOBBY_AUTOSTART',"
        "lobby_fkind:'SSB64_LOBBY_FKIND',lobby_stage:'SSB64_LOBBY_STAGE',"
        "lobby_min_players:'SSB64_LOBBY_MIN_PLAYERS',"
        "lobby_force_relay:'SSB64_LOBBY_FORCE_RELAY'};"
        "var out='';"
        "p.forEach(function(v,k){"
        "var n=/^SSB64_[A-Z0-9_]+$/.test(k)?k:alias[k.toLowerCase()];"
        "if(!n)return;"
        "if(v.indexOf('\\n')>=0||v.indexOf('=')>=0)return;"
        "out+=n+'='+v+'\\n';});"
        "return out;})()");
    if (blob == nullptr) {
        return;
    }
    for (char* line = blob; *line != '\0';) {
        char* eol = std::strchr(line, '\n');
        if (eol != nullptr) {
            *eol = '\0';
        }
        char* eq = std::strchr(line, '=');
        if (eq != nullptr && eq != line) {
            *eq = '\0';
            emscripten_errf("SSB64 web env: %s applied from URL param\n", line);
            ::setenv(line, eq + 1, /*overwrite=*/1);
        }
        line = eol != nullptr ? eol + 1 : line + std::strlen(line);
    }
    std::free(blob);
}

/* Shared inert state for the pack-installer stubs: an install request
 * reports Error with a human-readable reason instead of silently doing
 * nothing, and Cancel returns to Idle so the menu stays navigable. */
ssb64::enhancements::ModPackInstallPhase sWebPackPhase =
    ssb64::enhancements::ModPackInstallPhase::Idle;
std::string sWebPackStatus;
ssb64::enhancements::ShaderPackPhase sWebShaderPhase =
    ssb64::enhancements::ShaderPackPhase::Idle;
std::string sWebShaderStatus;

constexpr const char* kWebNoDownloads =
    "Downloads are not available in the web build";

} // namespace

namespace ssb64 {

/* The web loader places a pre-extracted BattleShip.o2r into MEMFS before
 * main() runs (platforms/web/). There is nothing to extract here — success
 * iff the archive is already present, otherwise the caller falls back to
 * its "archive missing" path which exits cleanly. */
ExtractionResult ExtractAssetsIfNeeded(const std::string& target_o2r_path, bool /*silent*/,
                                       const std::string& /*romOverridePath*/) {
    ExtractionResult result;
    if (std::filesystem::exists(target_o2r_path)) {
        result.success = true;
        result.outputPath = target_o2r_path;
    } else {
        result.error = "assets must be uploaded through the web loader";
    }
    return result;
}

/* RunFirstRunWizard/SetWindowIcon are shared with Switch — see
 * port_inert_stubs.cpp. */
namespace enhancements {

void FetchShaderPackCatalogAsync() {
    sWebShaderPhase = ShaderPackPhase::Error;
    sWebShaderStatus = kWebNoDownloads;
}
void InstallSelectedShaderPackAsync(const std::vector<std::string>&) {
    sWebShaderPhase = ShaderPackPhase::Error;
    sWebShaderStatus = kWebNoDownloads;
}
void CancelShaderPackFlow() { sWebShaderPhase = ShaderPackPhase::Idle; }
ShaderPackPhase GetShaderPackPhase() { return sWebShaderPhase; }
std::vector<ShaderPackCandidate> GetShaderPackCandidates() { return {}; }
std::string GetShaderPackStatus() { return sWebShaderStatus; }
int GetShaderPackInstalledCount() { return 0; }
bool IsShaderPackDownloadInProgress() { return false; }
bool IsShaderPackDownloadComplete() { return false; }

const CuratedModPack* GetCuratedModPacks(size_t* count) {
    if (count) {
        *count = 0;
    }
    return nullptr;
}
const CuratedModPack* FindCuratedModPack(const std::string&) { return nullptr; }
std::string GetModPackInstallPath(const CuratedModPack&) { return {}; }
bool IsModPackInstalled(const CuratedModPack&) { return false; }

void StartModPackInstallAsync(const std::string&) {
    sWebPackPhase = ModPackInstallPhase::Error;
    sWebPackStatus = kWebNoDownloads;
}
void StartPackInstallFromUrlAsync(const std::string&) {
    sWebPackPhase = ModPackInstallPhase::Error;
    sWebPackStatus = kWebNoDownloads;
}
void StartPackInstallFromPathAsync(const std::string&) {
    sWebPackPhase = ModPackInstallPhase::Error;
    sWebPackStatus = kWebNoDownloads;
}
void CancelModPackInstall() { sWebPackPhase = ModPackInstallPhase::Idle; }
ModPackInstallPhase GetModPackInstallPhase() { return sWebPackPhase; }
std::string GetModPackInstallStatus() { return sWebPackStatus; }
uint64_t GetModPackDownloadedBytes() { return 0; }
ModPackInstalledKind GetLastInstalledPackKind() { return ModPackInstalledKind::None; }
void RegisterModPackDropHandler() {}
bool IsGliden64ConverterReady() { return false; }

/* Discord rich presence — no Discord IPC from a browser tab. */
void TickDiscordPresence() {}

/* Release-manifest signature verify — no updater on web, so nothing ever
 * calls this; failing closed is the only honest stub. */
bool VerifyEd25519MinisigFields(const std::string&, const std::string&,
                                const std::string&, std::string& outError) {
    outError = "release signature verification is not available in the web build";
    return false;
}

} // namespace enhancements
} // namespace ssb64

extern "C" {

/* RenderDoc capture trigger — desktop GL debugging only. */
void portRenderDocInit(void) {}
void portRenderDocOnFrame(unsigned int /*frame_count*/) {}
void portRenderDocShutdown(void) {}


/* No hang watchdog on web: pthreads need SharedArrayBuffer + COOP/COEP
 * isolation, and a coroutine wedge in a cooperative browser loop is
 * already fatal to the tab — a poller thread buys nothing. */
void port_watchdog_init(void) {}
void port_watchdog_shutdown(void) {}
void port_watchdog_note_yield(void) {}
void port_watchdog_note_resume_start(int /*thread_id*/) {}
void port_watchdog_note_resume_end(int /*thread_id*/) {}
void port_watchdog_note_frame_end(void) {}
void port_dump_backtrace(void) { /* no symbols/traps worth dumping on wasm */ }

}

#endif // __EMSCRIPTEN__
