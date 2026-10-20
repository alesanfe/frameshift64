// No-op implementations of desktop-only port APIs for Nintendo Switch.
// The real files are excluded from SSB64_SRC_PORT on Switch (no Torch
// first-run extraction — assets ship pre-extracted on the SD card).
// RunFirstRunWizard/SetWindowIcon are shared with Emscripten and live in
// port_inert_stubs.cpp; this file keeps only the Switch-specific inert
// bodies. Menu/init code still references these symbols, so the link
// needs inert stand-ins, the same pattern as
// src/application/update/updater/UpdaterStubs.cpp. Lives in
// src/application/ because the stubbed APIs are application-layer:
// src/platform/ may not include application headers (check_module_deps).
#include "first_run.h"

#ifdef __SWITCH__

namespace ssb64 {

ExtractionResult ExtractAssetsIfNeeded(const std::string& /*target_o2r_path*/, bool /*silent*/,
                                       const std::string& /*romOverridePath*/) {
    // Nothing can be extracted on-device: returning failure drops the
    // caller into the "archive missing" path, which exits cleanly.
    return {};
}

} // namespace ssb64

#endif // __SWITCH__
