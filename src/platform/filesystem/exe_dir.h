/* exe_dir.h — directory containing the running executable.
 *
 * Platform primitive shared by every layer: application/app_paths.h's
 * RealAppBundlePath builds on it, and distribution modules (which may
 * not depend on application/) use it to find files staged next to the
 * binary (bundled-mods/, bundled-data/). */
#pragma once

#include <string>

namespace ssb64 {

// Directory containing this process's executable:
//   Linux   — readlink /proc/self/exe → parent dir
//   Windows — GetModuleFileNameW(NULL) → parent dir
//   macOS   — NSBundle resourcePath via Ship::Context (already correct
//             for both .app bundles and ad-hoc binaries)
// Falls back to Ship::Context::GetAppBundlePath() if the OS query fails.
// Never throws; may return "" when neither source resolves.
std::string ExecutableDirPath();

} // namespace ssb64
