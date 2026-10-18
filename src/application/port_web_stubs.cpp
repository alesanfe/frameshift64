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

#include <cstdio>
#include <filesystem>
#include <string>

namespace {

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

bool RunFirstRunWizard(const std::string& /*target_o2r_path*/) {
    return false;
}

void SetWindowIcon() {}

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
