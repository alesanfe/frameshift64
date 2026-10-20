// Inert implementations of desktop-only port APIs shared by the
// capability-limited platforms (Switch, Emscripten). The real files are
// excluded from SSB64_SRC_PORT on both: hbmenu draws its own window chrome
// and browsers have none; neither platform can run the Torch first-run
// wizard (Switch ships assets pre-extracted, web uploads them through the
// loader). Menu/init code still references these symbols, so the link
// needs inert stand-ins — the same pattern as UpdaterStubs.cpp.
// Lives in src/application/ because the stubbed APIs are application-layer:
// src/platform/ may not include application headers (check_module_deps).
// Per-platform behavior that diverges (ExtractAssetsIfNeeded's
// MEMFS-presence check on web) stays in the port_*_stubs.cpp files.
#include "first_run.h"
#include "port_window_icon.h"

#if defined(__SWITCH__) || defined(__EMSCRIPTEN__)

namespace ssb64 {

bool RunFirstRunWizard(const std::string& /*target_o2r_path*/) {
    return false;
}

void SetWindowIcon() {}

} // namespace ssb64

#endif // __SWITCH__ || __EMSCRIPTEN__
