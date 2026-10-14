// No-op implementations of the enhancements.h updater API for platforms
// that exclude Updater.cpp (Android updates via APK installs, UWP via
// MSIX packages — neither can shell out to a self-update installer).
// UpdatePrompt.cpp and other menu code still reference these symbols, so
// the link needs *something*; every entry point here is an inert stub.

#include "gameplay/enhancements.h"

#if defined(BATTLESHIP_UWP) || defined(__ANDROID__)

namespace ssb64 {
namespace enhancements {

void CheckForUpdatesAsync(bool /*force*/) {}
void StartGameUpdate() {}
bool IsUpdateAvailable() { return false; }
bool DidUpdateCheckFail() { return false; }
bool IsDownloading() { return false; }
bool IsDownloadComplete() { return false; }
bool IsCheckingForUpdates() { return false; }
std::string GetUpdateStatus() { return {}; }
std::string GetDownloadStatus() { return {}; }
std::string GetLatestVersion() { return {}; }
std::string GetCurrentVersion() { return {}; }
std::string GetReleaseNotes() { return {}; }
void SkipLatestVersion() {}
bool IsLatestVersionSkipped() { return true; }
void OpenReleasePage() {}
void TickUpdater() {}

} // namespace enhancements
} // namespace ssb64

#endif // BATTLESHIP_UWP || __ANDROID__
