#include "app_paths.h"

#include "exe_dir.h"

#include <libultraship/libultraship.h>

#include <filesystem>
#include <system_error>

namespace ssb64 {

std::string ExternalDataPath(const std::string& rel_path) {
    std::filesystem::path root("E:/BattleShip");
    return rel_path.empty() ? root.string() : (root / rel_path).lexically_normal().string();
}

std::string RealAppBundlePath() {
    // The OS query lives in platform/exe_dir.cpp so non-application modules
    // (distribution) can resolve exe-adjacent staging dirs too.
    return ExecutableDirPath();
}

std::string LocateExistingFile(const std::string& rel_path) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // noexcept exists(p, ec) throughout — probing a path must never throw
    // (Windows fs::exists can throw on malformed paths, BattleShip issue #58).
    fs::path p1 = fs::path(Ship::Context::GetAppDirectoryPath()) / rel_path;
    if (fs::exists(p1, ec)) {
        return p1.lexically_normal().string();
    }
#ifdef BATTLESHIP_UWP
    ec.clear();
    fs::path usb = ExternalDataPath(rel_path);
    if (fs::exists(usb, ec)) {
        return usb.lexically_normal().string();
    }
#endif
    ec.clear();
    fs::path p2 = fs::path(RealAppBundlePath()) / rel_path;
    if (fs::exists(p2, ec)) {
        return p2.lexically_normal().string();
    }
    return std::string();
}

} // namespace ssb64
