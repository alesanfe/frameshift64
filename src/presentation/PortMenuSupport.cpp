// SPDX-License-Identifier: MIT
//
// Portions of this file are derived from the Ship of Harkinian (SoH) project
//   Copyright (c) The Harbour Masters
//   https://github.com/HarbourMasters/Shipwright
// Licensed under the MIT License; see LICENSE at repository root.

#include "PortMenu.h"
#include "PortMenuShared.h"

#include "Toasts.h"
#include "UIWidgets.hpp"
#include "Compat.h"
#include "app_paths.h"
#include "PortI18n.h"
#include "enhancements.h"
#include "netlobby.h"
#ifndef DISABLE_SCRIPTING
#include "app_mods.h"
#include "schema.h"
#endif

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/ConsoleWindow.h>

#include <imgui.h>

#include <SDL2/SDL.h>
#include <spdlog/fmt/fmt.h>

#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

#include "crash_bundle.h"

#if defined(_WIN32)
#include <windows.h>
#include <process.h>
#elif defined(__linux__)
#include <unistd.h>
#include <limits.h>
#endif

void ssb64::RequestSafeModeRestart() {
    port_diag_request_safe_mode();
    /* The safe-mode gate only runs at process start — a console "reset"
     * doesn't re-read it, so we must relaunch for real. */
#if defined(_WIN32)
    char exePath[MAX_PATH];
    if (GetModuleFileNameA(nullptr, exePath, MAX_PATH) != 0) {
        _spawnl(_P_NOWAIT, exePath, exePath, nullptr);
        Ship::Context::GetInstance()->GetWindow()->Close();
        return;
    }
#elif defined(__linux__)
    char exePath[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (n > 0) {
        exePath[n] = '\0';
        execl(exePath, exePath, nullptr); /* replaces this process */
    }
#endif
    /* macOS / other: marker is written, user restarts manually. */
    ssb64::ToastInfo(TR("Safe mode will engage on next launch — restart the game"));
}

namespace ssb64 {

using namespace UIWidgets;

/* Diagnostics sidebar (§50): a flat, copyable snapshot of everything a
 * bug report needs — version/region, renderer + audio backend, game-data
 * integrity, mod inventory, and live lobby state. Plain text, no secrets
 * (room codes and paths are not included). */
static void RenderDiagnostics(WidgetInfo&) {
#if defined(REGION_JP)
    const char* region = "JP";
#else
    const char* region = "US";
#endif
    const char* backend = "unknown";
    const auto wb = Ship::Context::GetInstance()->GetWindow()->GetWindowBackend();
    if (windowBackendsMap.contains(wb)) {
        backend = windowBackendsMap.at(wb);
    }
    const char* audio = "unknown";
    const auto ab = Ship::Context::GetInstance()->GetAudio()->GetCurrentAudioBackend();
    if (audioBackendsMap.contains(ab)) {
        audio = audioBackendsMap.at(ab);
    }
    const std::string o2rPath = ssb64::LocateExistingFile(SSB64_O2R_NAME);
    const bool o2rOk = !o2rPath.empty();

    int mountedMods = 0, loadedMods = 0, badMods = 0;
#ifndef DISABLE_SCRIPTING
    const auto mods = ssb64::appmods::Snapshot();
    mountedMods = static_cast<int>(mods.size());
    for (const auto& m : mods) {
        if (m.state == ssb64::appmods::ModStatus::Loaded) {
            loadedMods++;
        }
        if (m.signature == ssb64::appmods::SignatureStatus::Invalid ||
            m.state == ssb64::appmods::ModStatus::InvalidManifest) {
            badMods++;
        }
    }
#endif

    ImGui::BeginChild("##diag_card", ImVec2(0, 0), ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::Text(TR("Version:     %s (%s)"), BATTLESHIP_CURRENT_VERSION, region);
    ImGui::Text(TR("Renderer:    %s"), backend);
    ImGui::Text(TR("Audio:       %s"), audio);
    ImGui::Text(TR("Game data:   %s"), o2rOk ? "Verified present" : "MISSING — first-run will re-extract");
    ImGui::Text(TR("Mods:        %d mounted, %d loaded, %d problem%s"), mountedMods, loadedMods, badMods, badMods == 1 ? "" : "s");
    ImGui::Text(TR("Netplay:     %s"), TR(port_netlobby_status()));
    if (port_diag_safe_mode_active()) {
        ImGui::TextColored(SemanticColors::Warning, "%s",
                           TR("Safe mode:   ON — mods & texture packs skipped this run"));
    }
    ImGui::EndChild();

    ImGui::SeparatorText(TR("Actions"));
    if (ImGui::Button(TR("Copy diagnostics summary"))) {
        std::string summary =
            fmt::format(fmt::runtime(TR("BattleShip {} ({})\nRenderer: {}\nAudio: {}\nGame data: {}\nMods: {} mounted, {} loaded, {} problems\nNetplay: {}\nSafe mode: {}")),
                        BATTLESHIP_CURRENT_VERSION, region, backend, audio,
                        o2rOk ? "present" : "MISSING", mountedMods, loadedMods, badMods,
                        port_netlobby_status(),
                        port_diag_safe_mode_active() ? "ON" : "off");
        ImGui::SetClipboardText(summary.c_str());
        ToastSuccess(TR("Diagnostics copied to clipboard"));
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Open data folder"))) {
        std::string appPath = Ship::Context::GetPathRelativeToAppDirectory("");
        std::error_code ec;
        fs::create_directories(appPath, ec);
        SDL_OpenURL(std::string("file:///" + fs::absolute(appPath).string()).c_str());
    }

    /* One-click support bundle: a folder a user can attach to a bug report
     * — report.txt (the same summary above + platform/build), a copy of
     * ssb64.log, and the config file. Mirrors the crash-bundle layout so
     * triage tooling reads both paths the same way. */
    if (ImGui::Button(TR("Write diagnostics bundle"))) {
        std::error_code ec;
        const fs::path logDir = fs::path(Ship::Context::GetPathRelativeToAppDirectory("logs"));
        char stamp[24] = {};
        {
            const std::time_t now = std::time(nullptr);
            std::tm tmv{};
#ifdef _WIN32
            localtime_s(&tmv, &now);
#else
            localtime_r(&now, &tmv);
#endif
            std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        }
        const fs::path dir = logDir / ("report-" + std::string(stamp));
        fs::create_directories(dir, ec);
        if (!ec) {
            {
                std::ofstream info(dir / "report.txt", std::ios::binary);
                info << "version=" << BATTLESHIP_CURRENT_VERSION << "\n"
                     << "build_sha=" << BATTLESHIP_BUILD_SHA << "\n"
                     << "platform="
#if defined(_WIN32)
                     << "windows"
#elif defined(__APPLE__)
                     << "macos"
#elif defined(__linux__)
                     << "linux"
#else
                     << "other"
#endif
                     << "\nrenderer=" << backend << "\naudio=" << audio
                     << "\nsafe_mode=" << (port_diag_safe_mode_active() ? "on" : "off")
                     << "\nmods=" << mountedMods << " mounted, " << loadedMods
                     << " loaded, " << badMods << " problems\n"
                     << "netplay=" << port_netlobby_status() << "\n";
            }
            std::error_code cec;
            const fs::path log = Ship::Context::GetPathRelativeToAppDirectory("logs/ssb64.log");
            if (fs::exists(log, cec)) {
                fs::copy_file(log, dir / "ssb64.log", fs::copy_options::overwrite_existing, cec);
            }
            const fs::path cfg = Ship::Context::GetPathRelativeToAppDirectory("BattleShip.cfg.json");
            if (fs::exists(cfg, cec)) {
                fs::copy_file(cfg, dir / "BattleShip.cfg.json", fs::copy_options::overwrite_existing, cec);
            }
            ToastSuccess(fmt::format(fmt::runtime(TR("Bundle written: {}")), dir.string()));
            SDL_OpenURL(std::string("file:///" + fs::absolute(dir, ec).string()).c_str());
        } else {
            ssb64::ToastInfo(TR("Could not write the diagnostics bundle"));
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", TR("report.txt + ssb64.log + config → attach to a bug report"));
    ImGui::TextDisabled("%s", TR("The summary excludes your ROM path, room codes, and any local paths."));
}


void PortMenu::AddMenuSupport() {
    AddMenuEntry("Support", CVAR_SETTING("Menu.SupportSidebarSection"));

    WidgetPath path = { "Support", "Actions", SECTION_COLUMN_1 };
    AddSidebarEntry("Support", "Actions", 1);
    AddWidget(path, "Actions", WIDGET_SEPARATOR_TEXT);

    AddWidget(path, "Close Menu", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([this](WidgetInfo&) { ToggleVisibility(); })
        .Options(ButtonOptions().Tooltip("Close this menu and return to the game.").Size(Sizes::Inline));

    AddWidget(path, "Restart Game", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"))
                ->Dispatch("reset");
        })
        .Options(ButtonOptions().Tooltip("Reload the game process state (same as the console reset command).")
                     .Size(Sizes::Inline));

    AddWidget(path, "Restart in Safe Mode", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) { RequestSafeModeRestart(); })
        .Options(ButtonOptions().Tooltip("Relaunches the game without mods or hi-res texture packs. "
                                         "Use when a bad mod or pack crashes on startup.")
                     .Size(Sizes::Inline));

#ifndef __ANDROID__
    AddWidget(path, "Quit BattleShip", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) { Ship::Context::GetInstance()->GetWindow()->Close(); })
        .Options(ButtonOptions().Tooltip("Exit the application entirely.").Size(Sizes::Inline));
#endif

    path.sidebarName = "Diagnostics";
    AddSidebarEntry("Support", "Diagnostics", 1);
    AddWidget(path, "Diagnostics", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "System Diagnostics", WIDGET_CUSTOM)
        .RaceDisable(false)
        .HideInSearch(true)
        .CustomFunction(RenderDiagnostics);

#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    // BUILT-IN UPDATER — hidden on Android. App updates come through the
    // Play Store on mobile; a curl-driven GitHub-releases updater can't
    // replace a system-managed installation, and the curl shell-out
    // (Updater.cpp) isn't built on Android in the first place.
    // The background check still fires when the menu loads.
    ssb64::enhancements::CheckForUpdatesAsync(false);

    path.sidebarName = "Updates";
    AddSidebarEntry("Support", "Updates", 1);
    AddWidget(path, "Updates", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Version: " + std::string(BATTLESHIP_CURRENT_VERSION), WIDGET_TEXT);

    // 1. "Checking for updates..." Text
    AddWidget(path, "Checking for updates...", WIDGET_TEXT)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = !ssb64::enhancements::IsCheckingForUpdates();
        });

    // 2. "Download Update" Button
    AddWidget(path, "Download Update", WIDGET_BUTTON)
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = ssb64::enhancements::IsCheckingForUpdates() ||
                            ssb64::enhancements::IsDownloading() ||
                            ssb64::enhancements::IsDownloadComplete() ||
                            !ssb64::enhancements::IsUpdateAvailable();

            if (!info.isHidden) {
                // One click: download, verify, install, restart (Updater.cpp).
                info.name = "Update now to " + ssb64::enhancements::GetLatestVersion();
            }
        })
        .Callback([](WidgetInfo&) {
            ssb64::enhancements::StartGameUpdate();
        })
        .Options(ButtonOptions().Tooltip("Downloads, verifies and installs the update, then restarts."));

    // 3. "Downloading... XX%" Text (Also handles the "Complete" text)
    AddWidget(path, "Download Status", WIDGET_TEXT)
        .PreFunc([](WidgetInfo& info) {
            info.isHidden = ssb64::enhancements::GetDownloadStatus().empty();   // progress, success or error
            if (!info.isHidden) {
                // Pipe the curl progress bar or completion string straight to the UI
                info.name = ssb64::enhancements::GetDownloadStatus();
            }
        });

    // 4. Update result text
    AddWidget(path, "Update Status", WIDGET_TEXT)
        .PreFunc([](WidgetInfo& info) {
            const std::string status = ssb64::enhancements::GetUpdateStatus();
            info.isHidden = ssb64::enhancements::IsCheckingForUpdates() ||
                            ssb64::enhancements::IsUpdateAvailable() ||
                            status.empty();
            if (!info.isHidden) {
                info.name = status;
            }
        });

    // 5. "Check for Updates" Manual Button
    AddWidget(path, "Check for Updates", WIDGET_BUTTON)
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) {
            // Hide this button while checking or downloading so they can't spam it
            info.isHidden = ssb64::enhancements::IsCheckingForUpdates() ||
                            ssb64::enhancements::IsDownloading() ||
                            ssb64::enhancements::IsDownloadComplete();
        })
        .Callback([](WidgetInfo&) {
            // Pass true to bypass the single-session lock
            ssb64::enhancements::CheckForUpdatesAsync(true);
        })
        .Options(ButtonOptions().Tooltip("Checks for a new release now, even if one was already checked."));
#endif // !__ANDROID__ (Updater)
}

void PortMenu::AddMenuAbout() {
    AddMenuEntry("About", CVAR_SETTING("Menu.AboutSidebarSection"));

    // revamped credits menu
    WidgetPath path = { "About", "Build", SECTION_COLUMN_1 };
    AddSidebarEntry("About", "Build", 1);
    AddWidget(path, "About", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "BattleShip", WIDGET_TEXT);
    AddWidget(path, "Native Smash 64 PC port powered by libultraship", WIDGET_TEXT);
    AddWidget(path, "Built from the ssb-decomp-re decompilation", WIDGET_TEXT);

    AddWidget(path, "Credits", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "JRickey: Lead Developer", WIDGET_TEXT);
    AddWidget(path, "Jameriquiah: Developer", WIDGET_TEXT);
    AddWidget(path, "the-outcaster: Developer", WIDGET_TEXT);
    AddWidget(path, "TechnicallyComputers and NyxTheShield: Netcode Developers", WIDGET_TEXT);
    AddWidget(path, "VetriTheRetri: Former Decompilation Lead", WIDGET_TEXT);
    AddWidget(path, "MarioReincarnate: Decompilation Lead", WIDGET_TEXT);
    AddWidget(path, "Krix08: Playtesting", WIDGET_TEXT);
    AddWidget(path, "Bleee: Playtesting", WIDGET_TEXT);
    AddWidget(path, "Fray: Nrage Control Advising", WIDGET_TEXT);
    AddWidget(path, "ElBateSoli: Raphnet Playtesting", WIDGET_TEXT);
    AddWidget(path, "MickleMoose: C Modding Toolkit Developer", WIDGET_TEXT);
    AddWidget(path, "Zorkats: C Modding Documentation", WIDGET_TEXT);
}

} // namespace ssb64
