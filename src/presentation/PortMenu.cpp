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
#include "port_save.h"
#include "profiles.h"
#include "frame_interpolation.h"
#include "netlobby.h"
#include "netleaderboard.h"
#ifdef PORT_HIRES_ENABLED
#include "HiResPack.h"
#endif

#include <fast/backends/gfx_rendering_api.h>
#include <fast/postprocess/PostProcessSourceLoader.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/ConsoleWindow.h>
#include "schema.h"
#ifndef DISABLE_SCRIPTING
#include "app_mods.h"
#endif

#include <imgui.h>

#include <SDL2/SDL.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

/* Navigation globals owned by Menu.cpp — declared in Menu_internal.h. */
#include "Menu_internal.h"

namespace ssb64 {

using namespace UIWidgets;

/* Decomp-side port hooks (components/decomp/src/sc/scmanager.c, sys/netreplay.c). The
 * scene target enum is region-stable by design — SCKind numbers differ
 * between US and JP builds, so the menu asks by name. */
extern "C" void portSCManagerRequestPortScene(int32_t port_scene);
extern "C" int portNetReplayPlayFile(const char* path);


namespace {

/* Mirrors enum SCPortSceneKind in components/decomp/src/sc/scmanager.c. */
namespace PlayScene {
    constexpr int32_t Title = 0;
    constexpr int32_t MainMenu = 1;
    constexpr int32_t Mode1PMenu = 2;
    constexpr int32_t VSModeMenu = 3;
    constexpr int32_t VSCharSelect = 4;
    constexpr int32_t Mode1PCharSelect = 5;
    constexpr int32_t TrainingCharSelect = 6;
    constexpr int32_t VSRecords = 7;
    constexpr int32_t CharData = 8;
} // namespace PlayScene

} // namespace

void PortMenu::AddSidebarEntry(std::string sectionName, std::string sidebarName, uint32_t columnCount) {
    assert(!sectionName.empty());
    assert(!sidebarName.empty());
    menuEntries.at(sectionName).sidebars.emplace(sidebarName, SidebarEntry{ .columnCount = columnCount });
    menuEntries.at(sectionName).sidebarOrder.push_back(sidebarName);
}

/* Sidebar/type-derived default flags — applied before callers' own
 * fluent overrides so .Advanced()/.Impact()/.StableId() still win. */
static void ApplyWidgetDefaults(WidgetInfo& widget, const WidgetPath& pathInfo, WidgetType widgetType) {
    /* Sidebars whose whole page is non-casual get their flags for free so
     * individual widgets don't each repeat .Advanced()/.Impact(). */
    if (pathInfo.sidebarName == "Advanced") {
        widget.advanced = true;
    }
    if (pathInfo.sidebarName == "Developer") {
        widget.impact = SettingImpact::DeveloperOnly;
        /* impact is metadata-only; advanced is what SidebarAdvancedOnly checks
         * when hiding the page in Simple mode. */
        widget.advanced = true;
    }
    /* Backend switches only take effect after a relaunch — declared once
     * here instead of relying on each widget's tooltip to say it. */
    if (widgetType == WIDGET_VIDEO_BACKEND || widgetType == WIDGET_AUDIO_BACKEND) {
        widget.applyPolicy = ApplyPolicy::RequiresRestart;
    }
}

/* Factory adapter: MakeFn returns shared_ptr<WidgetOptions>, so the
 * per-type makers must name the base explicitly (function pointers
 * don't do covariant returns). */
template <typename T>
static std::shared_ptr<WidgetOptions> MakeWidgetOptionsOf() {
    return std::make_shared<T>();
}

static std::shared_ptr<WidgetOptions> MakeWidgetOptions(WidgetType widgetType) {
    using MakeFn = std::shared_ptr<WidgetOptions> (*)();
    static const std::pair<WidgetType, MakeFn> kFactories[] = {
        { WIDGET_CHECKBOX,          &MakeWidgetOptionsOf<CheckboxOptions> },
        { WIDGET_CVAR_CHECKBOX,     &MakeWidgetOptionsOf<CheckboxOptions> },
        { WIDGET_SLIDER_FLOAT,      &MakeWidgetOptionsOf<FloatSliderOptions> },
        { WIDGET_CVAR_SLIDER_FLOAT, &MakeWidgetOptionsOf<FloatSliderOptions> },
        { WIDGET_CVAR_BTN_SELECTOR, &MakeWidgetOptionsOf<BtnSelectorOptions> },
        { WIDGET_SLIDER_INT,        &MakeWidgetOptionsOf<IntSliderOptions> },
        { WIDGET_CVAR_SLIDER_INT,   &MakeWidgetOptionsOf<IntSliderOptions> },
        { WIDGET_COMBOBOX,          &MakeWidgetOptionsOf<ComboboxOptions> },
        { WIDGET_CVAR_COMBOBOX,     &MakeWidgetOptionsOf<ComboboxOptions> },
        { WIDGET_AUDIO_BACKEND,     &MakeWidgetOptionsOf<ComboboxOptions> },
        { WIDGET_VIDEO_BACKEND,     &MakeWidgetOptionsOf<ComboboxOptions> },
        { WIDGET_BUTTON,            &MakeWidgetOptionsOf<ButtonOptions> },
        { WIDGET_WINDOW_BUTTON,     &MakeWidgetOptionsOf<WindowButtonOptions> },
        { WIDGET_CVAR_COLOR_PICKER, &MakeWidgetOptionsOf<ColorPickerOptions> },
        { WIDGET_SEPARATOR_TEXT,    &MakeWidgetOptionsOf<TextOptions> },
        { WIDGET_TEXT,              &MakeWidgetOptionsOf<TextOptions> },
    };
    for (const auto& [type, make] : kFactories) {
        if (type == widgetType) {
            return make();
        }
    }
    return std::make_shared<WidgetOptions>();
}

WidgetInfo& PortMenu::AddWidget(WidgetPath& pathInfo, std::string widgetName, WidgetType widgetType) {
    assert(!widgetName.empty());
    assert(menuEntries.contains(pathInfo.sectionName));
    assert(menuEntries.at(pathInfo.sectionName).sidebars.contains(pathInfo.sidebarName));

    auto& sidebars = menuEntries.at(pathInfo.sectionName).sidebars;
    const uint8_t column = static_cast<uint8_t>(pathInfo.column);
    while (sidebars.at(pathInfo.sidebarName).columnWidgets.size() < column + 1) {
        sidebars.at(pathInfo.sidebarName).columnWidgets.push_back({});
    }

    SidebarEntry& entry = sidebars.at(pathInfo.sidebarName);
    const std::string derivedId = pathInfo.sectionName + "." + pathInfo.sidebarName + "." + widgetName;
    entry.columnWidgets.at(column).push_back(
        { .name = std::move(widgetName), .type = widgetType });
    WidgetInfo& widget = entry.columnWidgets.at(column).back();
    /* Stable automation/test ID — set before any overrides so .StableId()
     * can replace it. Also searchable: a test or support doc can reference
     * "settings.display.vsync" and search finds it. */
    widget.stableId = derivedId;
    ApplyWidgetDefaults(widget, pathInfo, widgetType);
    widget.options = MakeWidgetOptions(widgetType);
    return widget;
}

PortMenu::PortMenu()
    : Menu("gOpenPortMenu", "Port Menu", 0, UIWidgets::Colors::LightBlue) {
    /* Header keeps only Close: Quit/Restart live in Support -> Application so
     * "leave the menu" can't be confused with "quit the app". */
    mShowHeaderQuit = false;
    mShowHeaderReset = false;
    mShowActionBar = true;
    mShowHelpBar = true;
}

// Play -> Replays: .ssr recordings written by the auto-recorder into
// <save dir>/replays/replay-<stamp>-<seed>.ssr (netreplay.c). Cached on
// first view and on Rescan — a directory hit per frame is needless.
static std::vector<std::string> s_replayFiles;
static bool s_replayListScanned = false;

static std::string ReplayDir() {
    return std::string(port_save_get_dir()) + "/replays";
}

static void RefreshReplayList() {
    s_replayFiles.clear();
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(ReplayDir(), ec)) {
        if (entry.is_regular_file(ec) && entry.path().extension() == ".ssr") {
            s_replayFiles.push_back(entry.path().filename().string());
        }
    }
    /* Names lead with a YYYYMMDD-HHMMSS stamp, so a reverse sort is
     * newest-first. */
    std::sort(s_replayFiles.rbegin(), s_replayFiles.rend());
    s_replayListScanned = true;
}

static void ReplayOpenFolder() {
    std::error_code ec;
    fs::create_directories(ReplayDir(), ec);
    SDL_OpenURL(("file:///" + fs::absolute(ReplayDir()).string()).c_str());
}

static void RenderReplayEmptyState() {
    ImGui::TextWrapped(TR("No replays found. Online matches record automatically and land in\n%s"), ReplayDir().c_str());
    if (ImGui::Button(TR("Rescan"))) {
        RefreshReplayList();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Open Folder"))) {
        ReplayOpenFolder();
    }
}

/* Share-state feedback tail after the Share button: progress, failure,
 * or the copyable link once the upload completes. */
static void RenderReplayShareTail() {
    const int shareState = port_netleaderboard_share_state();
    if (shareState == 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("Sharing..."));
    } else if (shareState == -1) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s", TR("Share failed"));
    } else if (shareState == 2) {
        char link[256] = {};
        port_netleaderboard_share_link(link, sizeof(link));
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Copy Link"))) {
            SDL_SetClipboardText(link);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", link);
        }
    }
}

/* One row of the community-shared replay list: timestamp, uploader,
 * and a per-row Download button. */
static void RenderSharedReplayRow(int i) {
    char id[40] = {}, by[128] = {}, stamp[24] = {};
    if (!port_netleaderboard_replays_at(i, id, sizeof(id),
                                        by, sizeof(by),
                                        stamp, sizeof(stamp))) {
        return;
    }
    ImGui::BulletText("%s", stamp[0] ? stamp : id);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", by[0] ? by : "-");
    ImGui::SameLine();
    ImGui::PushID(i);
    if (ImGui::SmallButton(TR("Download"))) {
        port_netleaderboard_replay_download(
            CVarGetString("gNetplay.Server", PORT_NETLOBBY_DEFAULT_SERVER),
            id);
    }
    ImGui::PopID();
}

/* Community-shared replays live on the lobby service — fetch the
 * index on demand, download lands the .ssr in the local replays dir. */
static void RenderSharedReplays() {
    if (!ImGui::TreeNode(TR("Shared replays"))) {
        return;
    }
    const int srvState = port_netleaderboard_replays_state();
    if (ImGui::SmallButton(TR("Refresh##sharedreplays"))) {
        port_netleaderboard_fetch_replays(
            CVarGetString("gNetplay.Server", PORT_NETLOBBY_DEFAULT_SERVER));
    }
    if (srvState == 1) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("Loading..."));
    } else if (srvState == -1) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s",
                           TR("Could not load the shared replay list."));
    } else if (srvState == 2) {
        const int shared = port_netleaderboard_replays_count();
        if (shared == 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", TR("none shared yet"));
        }
        for (int i = 0; i < shared && i < 25; i++) {
            RenderSharedReplayRow(i);
        }
    }
    const int dlState = port_netleaderboard_replay_dl_state();
    if (dlState == 1) {
        ImGui::TextDisabled("%s", TR("Downloading..."));
    } else if (dlState == -1) {
        ImGui::TextColored(ImVec4(0.95f, 0.35f, 0.35f, 1.0f), "%s",
                           TR("Download failed."));
    } else if (dlState == 2) {
        char dlPath[600] = {};
        port_netleaderboard_replay_dl_path(dlPath, sizeof(dlPath));
        ImGui::TextColored(ImVec4(0.35f, 0.9f, 0.45f, 1.0f),
                           TR("Saved %s — pick it above after Rescan"),
                           dlPath);
    }
    ImGui::TreePop();
}

static int s_replayIndex = 0;

static void RenderReplayFilePicker() {
    if (s_replayIndex >= static_cast<int>(s_replayFiles.size())) {
        s_replayIndex = 0;
    }
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::BeginCombo("##replayfile", s_replayFiles[s_replayIndex].c_str())) {
        for (int i = 0; i < static_cast<int>(s_replayFiles.size()); i++) {
            if (ImGui::Selectable(s_replayFiles[i].c_str(), i == s_replayIndex)) {
                s_replayIndex = i;
            }
        }
        ImGui::EndCombo();
    }
}

/* Play / Rescan / Open Folder / Share for the currently selected file. */
static void RenderReplayActionButtons(bool netBusy) {
    ImGui::BeginDisabled(netBusy);
    if (ImGui::Button(TR("Play Replay"))) {
        const std::string fullPath = ReplayDir() + "/" + s_replayFiles[s_replayIndex];
        if (portNetReplayPlayFile(fullPath.c_str())) {
            ToastInfo(TR("Loading replay..."));
        } else {
            ToastError(TR("Replay file rejected — see the log for details"));
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(TR("Rescan"))) {
        RefreshReplayList();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Open Folder"))) {
        ReplayOpenFolder();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(netBusy || port_netleaderboard_share_state() == 1);
    if (ImGui::Button(TR("Share"))) {
        const std::string fullPath = ReplayDir() + "/" + s_replayFiles[s_replayIndex];
        port_netleaderboard_share_replay(
            CVarGetString("gNetplay.Server", PORT_NETLOBBY_DEFAULT_SERVER),
            fullPath.c_str());
    }
    ImGui::EndDisabled();
}

static void RenderReplayBrowser(WidgetInfo&) {
    if (!s_replayListScanned) {
        RefreshReplayList();
    }
    const int lobbyState = port_netlobby_state();
    const bool netBusy = lobbyState != PORT_NETLOBBY_IDLE && lobbyState != PORT_NETLOBBY_FAILED;
    if (netBusy) {
        ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.25f, 1.0f),
                           "Online session in progress — leave it from Online -> Netplay first.");
    }
    if (s_replayFiles.empty()) {
        RenderReplayEmptyState();
        return;
    }

    RenderReplayFilePicker();
    RenderReplayActionButtons(netBusy);
    RenderReplayShareTail();
    RenderSharedReplays();
    ImGui::TextDisabled("%s", TR("Playback runs the recorded inputs; the match ends into the results screen."));
}

void PortMenu::AddMenuPlay() {
    AddMenuEntry("Play", CVAR_SETTING("Menu.PlaySidebarSection"));

    WidgetPath path = { "Play", "Jump to Screen", SECTION_COLUMN_1 };
    AddSidebarEntry("Play", "Jump to Screen", 1);
    AddWidget(path, "Jump straight to a screen — the current scene exits at the next frame boundary.",
              WIDGET_TEXT);
    AddWidget(path, "While an online session is active these are disabled so the room isn't abandoned silently.",
              WIDGET_TEXT);

    const auto sceneButton = [&](const char* label, int32_t scene, const char* tip) {
        AddWidget(path, label, WIDGET_BUTTON)
            .RaceDisable(false)
            .PreFunc([this](WidgetInfo& info) {
                if (disabledMap.at(DISABLE_FOR_NETPLAY_SESSION).active) {
                    info.activeDisables.push_back(DISABLE_FOR_NETPLAY_SESSION);
                }
            })
            .Callback([scene](WidgetInfo&) { portSCManagerRequestPortScene(scene); })
            .Options(ButtonOptions().Tooltip(tip).Size(Sizes::Inline));
    };
    sceneButton(TR("Versus — Character Select"), PlayScene::VSCharSelect,
                TR("Straight to the VS character select — pick fighters, then a stage."));
    sceneButton(TR("Versus — Mode Menu"), PlayScene::VSModeMenu,
                TR("The VS mode menu (versus rules, item switch, stage select)."));
    sceneButton(TR("1P Mode Menu"), PlayScene::Mode1PMenu,
                TR("Classic/1P mode menu — 1P Game, Training, Bonus stages."));
    sceneButton(TR("Training — Character Select"), PlayScene::TrainingCharSelect,
                TR("Straight to the Training character select."));
    sceneButton(TR("Main Menu"), PlayScene::MainMenu, TR("The game's own main menu."));
    sceneButton(TR("Title Screen"), PlayScene::Title, TR("Back to the title screen."));

    path.sidebarName = "Records";
    AddSidebarEntry("Play", "Records", 1);
    AddWidget(path, "Progress and records live on the cartridge save — these screens are the game's own.",
              WIDGET_TEXT);
    sceneButton(TR("VS Records"), PlayScene::VSRecords, TR("Win/loss records per fighter."));
    sceneButton(TR("Character Data"), PlayScene::CharData, TR("Per-character stats and bios."));

    path.sidebarName = "Replays";
    AddSidebarEntry("Play", "Replays", 1);
    AddWidget(path, "Replays", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Match Replays", WIDGET_CUSTOM)
        .RaceDisable(false)
        .CustomFunction(RenderReplayBrowser);
}

static void RenderExtractionStatus(WidgetInfo&) {
    // LocateExistingFile walks the same probe list the loader uses — the o2r
    // normally lives next to the exe, not in the app-data dir.
    const std::string o2rPath = ssb64::LocateExistingFile(SSB64_O2R_NAME);
    const std::string bakPath = o2rPath + ".bak";
    std::error_code ec;
    const bool exists = !o2rPath.empty();
    const double sizeMb = exists ? fs::file_size(o2rPath, ec) / 1048576.0 : 0.0;
    const bool bakExists = exists && fs::exists(bakPath, ec);
#if defined(REGION_JP)
    const char* region = "JP";
#else
    const char* region = "US";
#endif

    ImGui::BeginChild("##extraction_status", ImVec2(0, 0),
                      ImGuiChildFlags_Border | ImGuiChildFlags_AutoResizeY,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    if (exists) {
        ImGui::TextColored(SemanticColors::Success, "%s", TR("Status: Ready"));
    } else {
        ImGui::TextColored(SemanticColors::Error, "%s", TR("Status: Missing"));
    }
    if (exists) {
        ImGui::TextDisabled(TR("Archive:  %s (%.1f MB)"), SSB64_O2R_NAME, sizeMb);
    } else {
        ImGui::TextDisabled(TR("Archive:  %s"), SSB64_O2R_NAME);
    }
    ImGui::TextDisabled(TR("Region:   %s"), region);
    if (bakExists) {
        ImGui::TextColored(SemanticColors::Warning, TR("Backup:   %s.bak present"), SSB64_O2R_NAME);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(TR("%s"), UIWidgets::WrappedText(
                "A previous archive was kept as a backup. If the current one is broken, "
                "restore it by deleting the incomplete archive and renaming .bak back.").c_str());
        }
    }
    if (!exists) {
        ImGui::TextWrapped(TR("%s"), UIWidgets::WrappedText(
            "The game archive has not been generated yet — it is created from your ROM "
            "on first launch.").c_str());
    }
    ImGui::EndChild();
}

void PortMenu::AddMenuAssets() {
    AddMenuEntry("Game Files", CVAR_SETTING("Menu.AssetsSidebarSection"));

    WidgetPath path = { "Game Files", "Extraction", SECTION_COLUMN_1 };
    AddSidebarEntry("Game Files", "Extraction", 1);
    AddWidget(path, "Game Archive", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Archive Status", WIDGET_CUSTOM).RaceDisable(false).CustomFunction(RenderExtractionStatus);
    AddWidget(path, SSB64_O2R_NAME " is generated from your ROM and loaded on launch.", WIDGET_TEXT);
    AddWidget(path, "Schedule Re-extract", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([this](WidgetInfo&) {
            const auto o2rPath = Ship::Context::GetPathRelativeToAppDirectory(SSB64_O2R_NAME);
            const auto bakPath = o2rPath + ".bak";
            std::error_code ec;
            fs::remove(bakPath, ec);
            fs::rename(o2rPath, bakPath, ec);
            if (!ec) {
                mShowReextractMessage = true;
            }
        })
        .Options(ButtonOptions().Tooltip("Renames the existing " SSB64_O2R_NAME " to .bak so the next launch regenerates it from your ROM. If the new extraction fails, restore the .bak by deleting the incomplete archive.")
                     .Size(Sizes::Inline));

    path.sidebarName = "Paths";
    AddSidebarEntry("Game Files", "Paths", 1);
    AddWidget(path, "Runtime Paths", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, fmt::format(fmt::runtime(TR("App directory: {}")), Ship::Context::GetPathRelativeToAppDirectory("")), WIDGET_TEXT);
    AddWidget(path, fmt::format(fmt::runtime(TR("Main archive: {}")), Ship::Context::GetPathRelativeToAppDirectory(SSB64_O2R_NAME)),
              WIDGET_TEXT);
    AddWidget(path, "Open App Files Folder", WIDGET_BUTTON)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            std::string appPath = Ship::Context::GetPathRelativeToAppDirectory("");
            std::error_code ec;
            fs::create_directories(appPath, ec);
            SDL_OpenURL(std::string("file:///" + fs::absolute(appPath).string()).c_str());
        })
        .Options(ButtonOptions().Tooltip("Opens the BattleShip app folder. Useful for log files or manual pack installation."));

    /* Texture packs moved to Customization — see AddMenuCustomization. */
}

void PortMenu::RebuildMenu() {
    /* A sidebar-shaping CVar flipped — drop every entry and rebuild through
     * the same path first Init used. RemoveSidebarSearch assumes the
     * Settings entry still exists, so strip it while the tree is intact. */
    if (menuEntries.contains("Settings")) {
        RemoveSidebarSearch();
    }
    ClearAllEntries();
    AddMenuElements();
}

void PortMenu::AddMenuElements() {
    AddMenuPlay();
    AddMenuOnline();
    AddMenuSettings();
    AddMenuCustomization();
    AddMenuAssets();
    AddMenuSupport();
    AddMenuAbout();

    if (CVarGetInteger(CVAR_SETTING("Menu.SidebarSearch"), 0)) {
        InsertSidebarSearch();
    }

    /* Post-pass: CVar widgets inherit gameplay impact from the config schema
     * — the same affects_gameplay flag that drives the netplay compat
     * manifest. Done here because .CVar() is chained after AddWidget. */
    for (auto& menuLabel : menuOrder) {
        for (auto& [sidebarLabel, sidebar] : menuEntries.at(menuLabel).sidebars) {
            for (auto& column : sidebar.columnWidgets) {
                for (auto& widget : column) {
                    if (widget.cVar == nullptr || widget.impact != SettingImpact::LocalOnly) {
                        continue;
                    }
                    const auto* schemaEntry = ssb64::config::PortConfigSchemaFind(widget.cVar);
                    if (schemaEntry != nullptr && schemaEntry->affects_gameplay) {
                        widget.impact = SettingImpact::Gameplay;
                    }
                }
            }
        }
    }

    /* Registered init funcs are one-shot (they typically install hooks or
     * populate static registries) — re-running them on RebuildMenu would
     * double-register. */
    if (!mMenuElementsInitialized) {
        for (auto& initFunc : MenuInit::GetInitFuncs()) {
            initFunc();
        }
    }

    mMenuElementsInitialized = true;
}

void PortMenu::InitElement() {
    if (!mMenuElementsInitialized) {
        AddMenuElements();
    }

    Ship::Menu::InitElement();
    disabledMap = {
        { DISABLE_FOR_NO_VSYNC,
          { [](disabledInfo&) -> bool { return !Ship::Context::GetInstance()->GetWindow()->CanDisableVerticalSync(); },
            "Disabling VSync not supported" } },
        { DISABLE_FOR_NO_WINDOWED_FULLSCREEN,
          { [](disabledInfo&) -> bool {
               return !Ship::Context::GetInstance()->GetWindow()->SupportsWindowedFullscreen();
           },
            "Windowed Fullscreen not supported" } },
        { DISABLE_FOR_NO_MULTI_VIEWPORT,
          { [](disabledInfo&) -> bool {
               return !Ship::Context::GetInstance()->GetWindow()->GetGui()->SupportsViewports();
           },
            "Multi-viewports not supported" } },
        { DISABLE_FOR_NOT_DIRECTX,
          { [](disabledInfo&) -> bool {
               return Ship::Context::GetInstance()->GetWindow()->GetWindowBackend() !=
                      Ship::WindowBackend::FAST3D_DXGI_DX11;
           },
            "Available only on DirectX" } },
        { DISABLE_FOR_DIRECTX,
          { [](disabledInfo&) -> bool {
               return Ship::Context::GetInstance()->GetWindow()->GetWindowBackend() ==
                      Ship::WindowBackend::FAST3D_DXGI_DX11;
           },
            "Not available on DirectX" } },
        { DISABLE_FOR_MATCH_REFRESH_RATE_ON,
          { [](disabledInfo&) -> bool { return CVarGetInteger(PORT_INTERP_CVAR_MATCH_DISPLAY, 0); },
            "Match Display Refresh Rate is enabled", false, 0,
            "Disable it",
            [] {
                CVarSetInteger(PORT_INTERP_CVAR_MATCH_DISPLAY, 0);
                CVarSave();
            } } },
        { DISABLE_FOR_ADVANCED_RESOLUTION_ON,
          { [](disabledInfo&) -> bool { return CVarGetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled", 0); },
            "Advanced Resolution enabled", false, 0,
            "Disable Advanced Resolution",
            [] {
                CVarSetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled", 0);
                CVarSave();
            } } },
        { DISABLE_FOR_VERTICAL_RES_TOGGLE_ON,
          { [](disabledInfo&) -> bool {
               return CVarGetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalResolutionToggle", 0);
           },
            "Vertical Resolution Toggle enabled", false, 0,
            "Disable it",
            [] {
                CVarSetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalResolutionToggle", 0);
                CVarSave();
            } } },
        { DISABLE_FOR_LOW_RES_MODE_ON,
          { [](disabledInfo&) -> bool { return CVarGetInteger(CVAR_LOW_RES_MODE, 0); }, "N64 mode enabled", false, 0,
            "Disable N64 mode",
            [] {
                CVarSetInteger(CVAR_LOW_RES_MODE, 0);
                CVarSave();
            } } },
        { DISABLE_FOR_ADVANCED_RESOLUTION_OFF,
          { [](disabledInfo&) -> bool { return !CVarGetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled", 0); },
            "Advanced Resolution is disabled", false, 0,
            "Enable Advanced Resolution",
            [] {
                CVarSetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".Enabled", 1);
                CVarSave();
            } } },
        { DISABLE_FOR_VERTICAL_RESOLUTION_OFF,
          { [](disabledInfo&) -> bool {
               return !CVarGetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalResolutionToggle", 0);
           },
            "Vertical Resolution Toggle is off", false, 0,
            "Enable it",
            [] {
                CVarSetInteger(CVAR_PREFIX_ADVANCED_RESOLUTION ".VerticalResolutionToggle", 1);
                CVarSave();
            } } },
        /* Play -> scene jumps and replay playback would silently abandon a
         * live online session, so they explain the block instead of doing it. */
        { DISABLE_FOR_NETPLAY_SESSION,
          { [](disabledInfo&) -> bool {
               const int lobbyState = port_netlobby_state();
               return lobbyState != PORT_NETLOBBY_IDLE && lobbyState != PORT_NETLOBBY_FAILED;
           },
            "Online session in progress — leave it from Online -> Netplay first" } },
    };
}

void PortMenu::UpdateElement() {
    Ship::Menu::UpdateElement();
}

void PortMenu::Draw() {
    /* Auto-revert fires even with the menu closed: the whole point is that
     * the screen may be unusable, in which case the modal can't be seen
     * either — revert silently once the deadline passes. */
    if (mDisplayRevertArmed && !IsVisible() &&
        std::chrono::steady_clock::now() > mDisplayRevertDeadline) {
        Ship::Context::GetInstance()->GetWindow()->ToggleFullscreen();
        mDisplayRevertArmed = false;
    }
    Ship::Menu::Draw();
}

/* Re-extract confirmation: rendered as an in-context ImGui modal rather
 * than SDL_ShowSimpleMessageBox, which would nest a blocking native modal
 * (NSAlert / Win32 MessageBox) inside the in-progress ImGui frame and the
 * GPU command encode for that frame. ImGui's own popup runs in the same
 * context with no thread/native-modal nesting. */
void PortMenu::DrawReextractModal(const ImVec2& viewportCenter) {
    if (mShowReextractMessage) {
        ImGui::OpenPopup("Re-extract scheduled");
        mShowReextractMessage = false;
    }
    ImGui::SetNextWindowPos(viewportCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Re-extract scheduled", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped(
            SSB64_O2R_NAME " will be regenerated from your ROM the next time the game launches.");
        ImGui::Spacing();
        if (ImGui::Button(TR("OK"), ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

/* Display-change auto-revert: countdown modal after toggling fullscreen.
 * Keep commits the new mode; timeout or Revert undoes the toggle. */
void PortMenu::DrawDisplayRevertModal(const ImVec2& viewportCenter) {
    if (mDisplayRevertArmed) {
        ImGui::OpenPopup("Keep this display mode?");
    }
    ImGui::SetNextWindowPos(viewportCenter, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Keep this display mode?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        const float secondsLeft =
            -std::chrono::duration<float>(std::chrono::steady_clock::now() - mDisplayRevertDeadline)
                 .count();
        if (secondsLeft <= 0) {
            Ship::Context::GetInstance()->GetWindow()->ToggleFullscreen();
            mDisplayRevertArmed = false;
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextWrapped(TR("The display mode was changed. Reverting automatically in %d seconds "
                               "if it isn't confirmed."), static_cast<int>(std::ceil(secondsLeft)));
            ImGui::Spacing();
            if (ImGui::Button(TR("Keep"), ImVec2(140, 0))) {
                mDisplayRevertArmed = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button(TR("Revert now"), ImVec2(140, 0))) {
                Ship::Context::GetInstance()->GetWindow()->ToggleFullscreen();
                mDisplayRevertArmed = false;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }
}

void PortMenu::DrawQuitConfirmModal(const ImVec2& viewportCenter) {
    if (mShowQuitConfirm) {
        ImGui::OpenPopup("Quit during online session?");
        mShowQuitConfirm = false;
    }
    ImGui::SetNextWindowPos(viewportCenter, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Quit during online session?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextWrapped("%s", TR("You're still in an online session. Quitting now will disconnect you."));
        ImGui::Spacing();
        if (ImGui::Button(TR("Quit anyway"), ImVec2(140, 0))) {
            ImGui::CloseCurrentPopup();
            Ship::Context::GetInstance()->GetWindow()->Close();
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Stay"), ImVec2(140, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void PortMenu::DrawElement() {
    if (mMenuElementsInitialized) {
        Ship::Menu::DrawElement();
    }

    const ImVec2 viewportCenter = ImGui::GetMainViewport()->GetCenter();
    DrawReextractModal(viewportCenter);
    DrawDisplayRevertModal(viewportCenter);
    DrawQuitConfirmModal(viewportCenter);

    RenderPostProcessLowResWarnModal();
#if !defined(BATTLESHIP_UWP)
    RenderShaderPackModal();
#endif
}

} // namespace ssb64
