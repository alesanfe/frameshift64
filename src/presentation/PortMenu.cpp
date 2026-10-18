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

/* Navigation globals owned by Menu.cpp — a "Go to" row writes them and the
 * menu's draw path jumps to the owning sidebar, scrolls the target into
 * view and flashes an accent outline (one frame, ~1.5 s TTL). */
namespace Ship {
extern bool navigateToWidget;
extern std::string navigateMainEntry;
extern std::string navigateSidebar;
extern std::string navigateWidgetName;
}

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

static const std::map<int32_t, const char*> kMenuThemeOptions = {
    { UIWidgets::Colors::Red, "Red" },
    { UIWidgets::Colors::DarkRed, "Dark Red" },
    { UIWidgets::Colors::Orange, "Orange" },
    { UIWidgets::Colors::Green, "Green" },
    { UIWidgets::Colors::DarkGreen, "Dark Green" },
    { UIWidgets::Colors::LightBlue, "Light Blue" },
    { UIWidgets::Colors::Blue, "Blue" },
    { UIWidgets::Colors::DarkBlue, "Dark Blue" },
    { UIWidgets::Colors::Indigo, "Indigo" },
    { UIWidgets::Colors::Violet, "Violet" },
    { UIWidgets::Colors::Purple, "Purple" },
    { UIWidgets::Colors::Brown, "Brown" },
    { UIWidgets::Colors::Gray, "Gray" },
    { UIWidgets::Colors::DarkGray, "Dark Gray" },
};

static const std::map<int32_t, const char*> kMenuBaseThemeOptions = {
    { 0, "Dark" },
    { 1, "Light" },
    { 2, "High Contrast" },
};

/* Mirrors PortI18n::kLangCodes (src/i18n/PortI18n.cpp). Labels stay in
 * each language's own name on purpose — a user who picked a language they
 * can't read must still be able to find theirs. */
static const std::map<int32_t, const char*> kMenuLanguageOptions = {
    { 0, "English" },
    { 1, "Español" },
    { 2, "Français" },
    { 3, "Deutsch" },
    { 4, "Português" },
};

/* gSettings.MenuMode — 0 hides advanced widgets/sidebars (Simple), 1 shows
 * everything (Advanced, the historical behaviour). */
static const std::map<int32_t, const char*> kMenuModeOptions = {
    { 0, "Simple" },
    { 1, "Advanced" },
};

static const std::map<int32_t, const char*> kTextureFilteringMap = {
    { Fast::FILTER_THREE_POINT, "Three-Point" },
    { Fast::FILTER_LINEAR, "Linear" },
    { Fast::FILTER_NONE, "None" },
};

/* Graphics presets (§12): one-click starting points for players who don't
 * want to tune internal resolution / MSAA / widescreen separately. The
 * combo reflects live CVar state — any manual tweak flips it to Custom. */
static const std::map<int32_t, const char*> kGraphicsPresetMap = {
    { 0, "Authentic N64" },
    { 1, "Balanced" },
    { 2, "Quality" },
    { 3, "Custom" },
};

static int32_t sGraphicsPreset = 3;

static int DetectGraphicsPreset() {
    const float res = CVarGetFloat(CVAR_INTERNAL_RESOLUTION, 1.0f);
    const int msaa = CVarGetInteger(CVAR_MSAA_VALUE, 1);
    const bool wide = CVarGetInteger(enhancements::WidescreenCVarName(), 0);
    if (res == 1.0f && msaa == 1 && !wide) {
        return 0;
    }
    if (res == 2.0f && msaa == 2 && wide) {
        return 1;
    }
    if (res == 4.0f && msaa == 4 && wide) {
        return 2;
    }
    return 3;
}

static void ApplyGraphicsPreset(int preset) {
    float res = 4.0f;
    int msaa = 4, wide = 1;
    if (preset == 0) {
        res = 1.0f, msaa = 1, wide = 0;
    } else if (preset == 1) {
        res = 2.0f, msaa = 2;
    }
    CVarSetFloat(CVAR_INTERNAL_RESOLUTION, res);
    CVarSetInteger(CVAR_MSAA_VALUE, msaa);
    CVarSetInteger(enhancements::WidescreenCVarName(), wide);
    CVarSave();
    Ship::Context::GetInstance()->GetWindow()->SetResolutionMultiplier(res);
    Ship::Context::GetInstance()->GetWindow()->SetMsaaLevel(msaa);
    ToastSuccess(fmt::format(fmt::runtime(TR("Graphics preset applied: {} — widescreen applies on next launch")),
                             kGraphicsPresetMap.at(preset)));
}

// Enhanced framerate mode: CVar stores the target render fps; game logic
// always stays at 60 Hz (see src/rendering/interpolation/frame_interpolation.h).
static const std::map<int32_t, const char*> kEnhancedFpsMap = {
    { 0, "Off (60 FPS)" },
    { 120, "120 FPS" },
    { 180, "180 FPS" },
    { 240, "240 FPS" },
};

// Mirrors the LowResMode switch in libultraship Gui::CalculateGameViewport /
// Gui::DrawGame. 0 keeps the framebuffer at the window resolution; 1 forces a
// 320x240 4:3 framebuffer centered with side strips; 2/3 keep the window's
// aspect but lock vertical pixel count.
// Keys 0..3 hand off to libultraship's gLowResMode (Gui.cpp LowResMode switch);
// keys 4..7 take the PixelPerfectMode path of gAdvancedResolution and ignore
// gLowResMode. port.cpp's boot latch translates the pending menu value into
// the right combination of LUS cvars so libultraship sees a stable state for
// the whole session.
static const std::map<int32_t, const char*> kLowResModeMap = {
    { 0, "Off (window resolution)" },
    { 1, "N64 (320x240, stretched 4:3)" },
    { 2, "240p (window aspect, stretched)" },
    { 3, "480p (window aspect, stretched)" },
    { 4, "N64 integer-scaled (auto-fit, pixel-perfect)" },
    { 5, "N64 integer-scaled (2x, pixel-perfect)" },
    { 6, "N64 integer-scaled (3x, pixel-perfect)" },
    { 7, "N64 integer-scaled (4x, pixel-perfect)" },
};

// Mirrors dbObjectDisplayMode (src/sys/develop.h). 0 disables the override and
// returns the engine's normal rendering.
static const std::map<int32_t, const char*> kHitboxViewMap = {
    { 0, "Off" },
    { 1, "Filled (red cubes)" },
    { 2, "Outline + opaque hurtboxes" },
};

} // namespace

void PortMenu::AddSidebarEntry(std::string sectionName, std::string sidebarName, uint32_t columnCount) {
    assert(!sectionName.empty());
    assert(!sidebarName.empty());
    menuEntries.at(sectionName).sidebars.emplace(sidebarName, SidebarEntry{ .columnCount = columnCount });
    menuEntries.at(sectionName).sidebarOrder.push_back(sidebarName);
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

    switch (widgetType) {
        case WIDGET_CHECKBOX:
        case WIDGET_CVAR_CHECKBOX:
            widget.options = std::make_shared<CheckboxOptions>();
            break;
        case WIDGET_SLIDER_FLOAT:
        case WIDGET_CVAR_SLIDER_FLOAT:
            widget.options = std::make_shared<FloatSliderOptions>();
            break;
        case WIDGET_CVAR_BTN_SELECTOR:
            widget.options = std::make_shared<BtnSelectorOptions>();
            break;
        case WIDGET_SLIDER_INT:
        case WIDGET_CVAR_SLIDER_INT:
            widget.options = std::make_shared<IntSliderOptions>();
            break;
        case WIDGET_COMBOBOX:
        case WIDGET_CVAR_COMBOBOX:
        case WIDGET_AUDIO_BACKEND:
        case WIDGET_VIDEO_BACKEND:
            widget.options = std::make_shared<ComboboxOptions>();
            break;
        case WIDGET_BUTTON:
            widget.options = std::make_shared<ButtonOptions>();
            break;
        case WIDGET_WINDOW_BUTTON:
            widget.options = std::make_shared<WindowButtonOptions>();
            break;
        case WIDGET_CVAR_COLOR_PICKER:
        case WIDGET_COLOR_PICKER:
            widget.options = std::make_shared<ColorPickerOptions>();
            break;
        case WIDGET_SEPARATOR_TEXT:
        case WIDGET_TEXT:
            widget.options = std::make_shared<TextOptions>();
            break;
        case WIDGET_SEARCH:
        case WIDGET_SEPARATOR:
        default:
            widget.options = std::make_shared<WidgetOptions>();
            break;
    }

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
        ImGui::TextWrapped(TR("No replays found. Online matches record automatically and land in\n%s"), ReplayDir().c_str());
        if (ImGui::Button(TR("Rescan"))) {
            RefreshReplayList();
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Open Folder"))) {
            std::error_code ec;
            fs::create_directories(ReplayDir(), ec);
            SDL_OpenURL(("file:///" + fs::absolute(ReplayDir()).string()).c_str());
        }
        return;
    }

    static int s_replayIndex = 0;
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
        std::error_code ec;
        fs::create_directories(ReplayDir(), ec);
        SDL_OpenURL(("file:///" + fs::absolute(ReplayDir()).string()).c_str());
    }
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

namespace {

/* §35 - profile name list refreshed on demand for the widgets below. */
std::vector<std::string> PortProfileNames() {
    char buf[8192];
    port_profile_list(buf, sizeof(buf));
    std::vector<std::string> out;
    // strtok shares global state across the process; scan '\n' manually.
    for (const char* line = buf; *line != '\0';) {
        const char* eol = std::strchr(line, '\n');
        out.emplace_back(line, eol ? eol - line : static_cast<ptrdiff_t>(std::strlen(line)));
        if (eol == nullptr) break;
        line = eol + 1;
    }
    return out;
}

/* Resolves gProfiles.Selected (index into the sorted list) to a name. */
std::string PortProfileSelectedName() {
    const auto names = PortProfileNames();
    const int sel = CVarGetInteger("gProfiles.Selected", 0);
    return (sel >= 0 && sel < static_cast<int>(names.size())) ? names[sel] : std::string();
}

} // namespace

namespace {

/* "Modified" sidebar: true when a CVar-bound widget's live value
 * differs from its declared default — same type coverage as
 * ResetSectionCvars in Menu.cpp. */
static bool WidgetIsNonDefault(const WidgetInfo& w) {
    if (w.cVar == nullptr || w.cVar[0] == '\0' || w.options == nullptr) {
        return false;
    }
    switch (w.type) {
        case WIDGET_CVAR_CHECKBOX:
            return CVarGetInteger(w.cVar, 0) !=
                   (std::static_pointer_cast<CheckboxOptions>(w.options)->defaultValue ? 1 : 0);
        case WIDGET_CVAR_COMBOBOX:
        case WIDGET_AUDIO_BACKEND:
        case WIDGET_VIDEO_BACKEND:
            return CVarGetInteger(w.cVar, 0) !=
                   static_cast<int32_t>(std::static_pointer_cast<ComboboxOptions>(w.options)->defaultIndex);
        case WIDGET_CVAR_SLIDER_INT:
            return CVarGetInteger(w.cVar, 0) !=
                   std::static_pointer_cast<IntSliderOptions>(w.options)->defaultValue;
        case WIDGET_CVAR_SLIDER_FLOAT:
            return CVarGetFloat(w.cVar, 0.0f) !=
                   std::static_pointer_cast<FloatSliderOptions>(w.options)->defaultValue;
        case WIDGET_CVAR_BTN_SELECTOR:
            return CVarGetInteger(w.cVar, 0) !=
                   std::static_pointer_cast<BtnSelectorOptions>(w.options)->defaultValue;
        case WIDGET_CVAR_INPUT:
            return std::string(CVarGetString(w.cVar, "")) !=
                   std::static_pointer_cast<InputOptions>(w.options)->defaultValue;
        case WIDGET_CVAR_COLOR_PICKER: {
            const Color_RGBA8 d = std::static_pointer_cast<ColorPickerOptions>(w.options)->defaultValue;
            const Color_RGBA8 c = CVarGetColor(w.cVar, d);
            return !(c.r == d.r && c.g == d.g && c.b == d.b && c.a == d.a);
        }
        default:
            return false;
    }
}

/* Live CVar value of a widget, formatted for the "Modified" audit list -
 * same type coverage as WidgetIsNonDefault. */
static std::string WidgetCurrentValue(const WidgetInfo& w) {
    char buf[64];
    switch (w.type) {
        case WIDGET_CVAR_CHECKBOX:
            return CVarGetInteger(w.cVar, 0) ? TR("On") : TR("Off");
        case WIDGET_CVAR_COMBOBOX:
        case WIDGET_AUDIO_BACKEND:
        case WIDGET_VIDEO_BACKEND: {
            const int32_t v = CVarGetInteger(w.cVar, 0);
            const auto& map = std::static_pointer_cast<ComboboxOptions>(w.options)->comboMap;
            const auto it = map.find(v);
            return it != map.end() ? TR(it->second) : std::to_string(v);
        }
        case WIDGET_CVAR_SLIDER_INT:
        case WIDGET_CVAR_BTN_SELECTOR:
            std::snprintf(buf, sizeof(buf), "%d", CVarGetInteger(w.cVar, 0));
            return buf;
        case WIDGET_CVAR_SLIDER_FLOAT:
            std::snprintf(buf, sizeof(buf), "%.2f", CVarGetFloat(w.cVar, 0.0f));
            return buf;
        case WIDGET_CVAR_INPUT:
            return CVarGetString(w.cVar, "");
        case WIDGET_CVAR_COLOR_PICKER: {
            const Color_RGBA8 c = CVarGetColor(w.cVar, { 255, 255, 255, 255 });
            std::snprintf(buf, sizeof(buf), "#%02X%02X%02X%02X", c.r, c.g, c.b, c.a);
            return buf;
        }
        default:
            return "";
    }
}

} // namespace

void PortMenu::AddMenuSettings() {
    AddMenuEntry("Settings", CVAR_SETTING("Menu.SettingsSidebarSection"));

    WidgetPath path = { "Settings", "General", SECTION_COLUMN_1 };
    AddSettingsGeneral(path);
    AddSettingsModified(path);
    AddSettingsDisplay(path);
    AddSettingsGameplay(path);
    AddSettingsInput(path);
    AddSettingsAudio(path);
}

/* Readability presets on top of the raw opacity slider below:
 * pick a preset or drag the slider for a custom value. */
static void DrawMenuBackgroundPreset(WidgetInfo&) {
    // Readability presets on top of the raw opacity slider below:
    // pick a preset or drag the slider for a custom value.
    const float opacity = CVarGetFloat(CVAR_SETTING("Menu.BackgroundOpacity"), 0.85f);
    int preset = 3; // Custom
    if (opacity <= 0.45f) {
        preset = 0;
    } else if (opacity <= 0.8f) {
        preset = 1;
    } else if (opacity >= 0.99f) {
        preset = 2;
    }
    static const char* const kPresets[] = { "Transparent", "Dim", "Opaque", "Custom" };
    ImGui::TextUnformatted(TR("Menu background"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##MenuBgPreset", &preset, kPresets, IM_ARRAYSIZE(kPresets))) {
        static const float kValues[] = { 0.40f, 0.75f, 1.0f, 0.85f };
        CVarSetFloat(CVAR_SETTING("Menu.BackgroundOpacity"), kValues[preset]);
        CVarSave();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("How solid the menu backdrop is. Opaque hides the running game entirely — best for readability and reduced motion."));
    }
}


/* Discord Rich Presence opt-in — excluded on Android/UWP (no
 * discord-rpc there). */
void PortMenu::AddSettingsDrp(WidgetPath& path) {
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
// DRP — Android drops discord-rpc entirely (no curl + Play Store
// distribution friction; the Discord SDK isn't built for mobile).
AddWidget(path, "Enable Discord Rich Presence", WIDGET_CVAR_CHECKBOX)
    .CVar(CVAR_SETTING("Menu.EnableDRP"))
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Adds Discord Rich Presence (DRP)."));
#endif
}


/* Interaction options under "General Settings": cursor visibility and
 * the sidebar-search controls. */
void PortMenu::AddSettingsGeneralInteraction(WidgetPath& path) {
AddWidget(path, "General Settings", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Cursor Always Visible", WIDGET_CVAR_CHECKBOX)
    .CVar(CVAR_SETTING("CursorVisibility"))
    .RaceDisable(false)
    .Callback([](WidgetInfo&) {
        Ship::Context::GetInstance()->GetWindow()->SetForceCursorVisibility(
            CVarGetInteger(CVAR_SETTING("CursorVisibility"), 0));
    })
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Makes the cursor always visible, even in fullscreen."));

AddWidget(path, "Search In Sidebar", WIDGET_CVAR_CHECKBOX)
    .CVar(CVAR_SETTING("Menu.SidebarSearch"))
    .RaceDisable(false)
    .Callback([this](WidgetInfo&) {
        if (CVarGetInteger(CVAR_SETTING("Menu.SidebarSearch"), 0)) {
            InsertSidebarSearch();
        } else {
            RemoveSidebarSearch();
        }
    })
    .Options(CheckboxOptions().DefaultValue(false).Tooltip(
        "Displays the Search page as a sidebar entry in Settings instead of in the header."));

AddWidget(path, "Search Input Autofocus", WIDGET_CVAR_CHECKBOX)
    .CVar(CVAR_SETTING("Menu.SearchAutofocus"))
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Gives the search input focus when it becomes visible."));

AddWidget(path, "All settings save automatically as you change them.", WIDGET_TEXT)
    .RaceDisable(false)
    .HideInSearch(true);
}


void PortMenu::AddSettingsGeneral(WidgetPath& path) {
    AddSidebarEntry("Settings", "General", 2);

    AddWidget(path, "Menu Settings", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Language", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("Language"))
        .RaceDisable(false)
        .SearchTerms({ "idioma", "langue", "sprache", "translation", "localisation" })
        .Options(ComboboxOptions().Tooltip("UI language. Translations load from lang/<code>.json next to the "
                                           "binary and apply immediately — missing strings stay in English.")
                     .ComboMap(kMenuLanguageOptions)
                     .DefaultIndex(0));
    AddWidget(path, "Menu complexity", WIDGET_CVAR_COMBOBOX)
        .CVar("gSettings.MenuMode")
        .RaceDisable(false)
        .SearchTerms({ "simple", "basic", "advanced", "progressive disclosure", "expert" })
        .Options(ComboboxOptions().Tooltip("Simple hides advanced widgets and sidebars while browsing — "
                                           "search still finds them. Advanced shows everything.")
                     .ComboMap(kMenuModeOptions)
                     .DefaultIndex(1));
    AddWidget(path, "Base theme", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("Menu.BaseTheme"))
        .RaceDisable(false)
        .SearchTerms({ "dark", "light", "contrast", "appearance" })
        .Options(ComboboxOptions().Tooltip("Surface palette of the port menu. High Contrast uses pure-white text, "
                                           "hard borders and an opaque backdrop.")
                     .ComboMap(kMenuBaseThemeOptions)
                     .DefaultIndex(0));
    AddWidget(path, "Accent color", WIDGET_CVAR_COMBOBOX)
        .CVar(CVAR_SETTING("Menu.Theme"))
        .RaceDisable(false)
        .SearchTerms({ "theme", "color", "highlight" })
        .Options(ComboboxOptions().Tooltip("Highlight color of the menu widgets, independent of the base theme.")
                     .ComboMap(kMenuThemeOptions)
                     .DefaultIndex(Colors::LightBlue));

    AddWidget(path, "Menu Controller Navigation", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_IMGUI_CONTROLLER_NAV)
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip(
            "Allows controller navigation of the port menu.\nCAUTION: this disables game inputs while the menu is visible."));

    AddWidget(path, "Allow background inputs", WIDGET_CVAR_CHECKBOX)
        .CVar(CVAR_ALLOW_BACKGROUND_INPUTS)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,
                        CVarGetInteger(CVAR_ALLOW_BACKGROUND_INPUTS, 1) ? "1" : "0");
        })
        .Options(CheckboxOptions().Tooltip(
                     "Allows controller input to be picked up by the game even when the window is not focused.")
                     .DefaultValue(true));

    AddWidget(path, "Menu Background", WIDGET_CUSTOM)
        .RaceDisable(false)
        .CustomFunction(DrawMenuBackgroundPreset);

    AddWidget(path, "Menu Background Opacity", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_SETTING("Menu.BackgroundOpacity"))
        .RaceDisable(false)
        .Options(FloatSliderOptions().DefaultValue(0.85f).IsPercentage().Tooltip(
            "Sets the opacity of the background behind the port menu."));

    AddSettingsGeneralInteraction(path);

    AddSettingsDrp(path);

    path.sidebarName = "Display";
}

/* "Modified" sidebar registration — the audit view of divergent
 * CVars is its own sidebar so it stays discoverable. */
void PortMenu::AddSettingsModified(WidgetPath& path) {
/* "Modified" sidebar — audit view of every CVar-bound widget whose live
 * value differs from its declared default, with a Go jump back to the
 * owning section. A dedicated sidebar (not buried in General) so it's
 * discoverable and screenshot-verifiable. */
path.column = SECTION_COLUMN_1;
path.sidebarName = "Modified";
AddSidebarEntry("Settings", "Modified", 1);
AddWidget(path, "Modified settings", WIDGET_CUSTOM)
    .RaceDisable(false)
    .HideInSearch(true)
    .CustomFunction([this](WidgetInfo&) {
        struct ModRow {
            std::string label;
            std::string section;
            std::string sidebar;
            std::string name;
            std::string value;
        };
        std::vector<ModRow> rows;
        for (const auto& [entryName, entry] : menuEntries) {
            for (const auto& [sbName, sb] : entry.sidebars) {
                for (const auto& col : sb.columnWidgets) {
                    for (const auto& w : col) {
                        if (WidgetIsNonDefault(w)) {
                            rows.push_back({ std::string(w.name), entryName, sbName, std::string(w.name),
                                             WidgetCurrentValue(w) });
                        }
                    }
                }
            }
        }
        ImGui::TextUnformatted(TR("Modified settings"));
        ImGui::SameLine();
        ImGui::TextDisabled("(%d)", static_cast<int>(rows.size()));
        if (rows.empty()) {
            ImGui::TextDisabled("%s", TR("All options are at their defaults."));
            return;
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginChild("##modified-settings", ImVec2(0, ImGui::GetTextLineHeightWithSpacing() * 6.5f),
                              ImGuiChildFlags_Borders)) {
            for (const auto& r : rows) {
                ImGui::PushID(r.name.c_str());
                ImGui::TextUnformatted(r.label.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("= %s", r.value.c_str());
                ImGui::SameLine();
                ImGui::TextDisabled("(%s -> %s)", r.section.c_str(), r.sidebar.c_str());
                ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() -
                                ImGui::CalcTextSize("Go").x - ImGui::GetStyle().FramePadding.x * 2.0f -
                                ImGui::GetStyle().ItemSpacing.x);
                if (ImGui::SmallButton(TR("Go"))) {
                    Ship::navigateToWidget = true;
                    Ship::navigateMainEntry = r.section;
                    Ship::navigateSidebar = r.sidebar;
                    Ship::navigateWidgetName = r.name;
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    });

}


void PortMenu::AddSettingsDisplay(WidgetPath& path) {
    AddSidebarEntry("Settings", "Display", 1);

    AddWidget(path, "Display", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Toggle Fullscreen", WIDGET_BUTTON)
        .RaceDisable(false)
        .SearchTerms({ "borderless", "windowed", "window", "maximize" })
        .Callback([this](WidgetInfo&) {
            Ship::Context::GetInstance()->GetWindow()->ToggleFullscreen();
            /* Auto-revert: fullscreen can leave the picture unusable on some
             * monitor/renderer pairs, so confirm or undo after 15 s. */
            mDisplayRevertArmed = true;
            mDisplayRevertDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        })
        .Options(ButtonOptions().Tooltip("Toggles fullscreen on or off. You'll be asked to confirm "
                                         "the new mode — if it can't be seen, it reverts in 15 s."));

    path.sidebarName = "Image Quality";
    AddSidebarEntry("Settings", "Image Quality", 1);
    AddWidget(path, "Image Quality", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Graphics Preset", WIDGET_COMBOBOX)
        .ValuePointer(&sGraphicsPreset)
        .RaceDisable(false)
        .PreFunc([](WidgetInfo& info) { *std::get<int32_t*>(info.valuePointer) = DetectGraphicsPreset(); })
        .Callback([](WidgetInfo& info) { ApplyGraphicsPreset(*std::get<int32_t*>(info.valuePointer)); })
        .SearchTerms({ "profile", "quality", "performance", "retro", "crt", "authentic" })
        .Options(ComboboxOptions()
                     .Tooltip("One-click starting points. Authentic N64 = native resolution, no AA, 4:3. "
                              "Balanced = 2x resolution, light AA, widescreen. Quality = 4x + MSAA 4. "
                              "Tweak any option to mark it Custom.")
                     .ComboMap(kGraphicsPresetMap));
    AddWidget(path, "Internal Resolution", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(CVAR_INTERNAL_RESOLUTION)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            Ship::Context::GetInstance()->GetWindow()->SetResolutionMultiplier(
                CVarGetFloat(CVAR_INTERNAL_RESOLUTION, 1.0f));
        })
        .PreFunc([this](WidgetInfo& info) {
            if (disabledMap.at(DISABLE_FOR_ADVANCED_RESOLUTION_ON).active &&
                disabledMap.at(DISABLE_FOR_VERTICAL_RES_TOGGLE_ON).active) {
                info.activeDisables.push_back(DISABLE_FOR_ADVANCED_RESOLUTION_ON);
                info.activeDisables.push_back(DISABLE_FOR_VERTICAL_RES_TOGGLE_ON);
            } else if (disabledMap.at(DISABLE_FOR_LOW_RES_MODE_ON).active) {
                info.activeDisables.push_back(DISABLE_FOR_LOW_RES_MODE_ON);
            }
        })
        .Options(FloatSliderOptions().DefaultValue(1.0f)
                     .Tooltip("Multiplies the internal rendering resolution.")
                     .ShowButtons(false)
                     .IsPercentage()
                     .Min(0.5f)
                     .Max(2.0f));

    path.sidebarName = "Display";
    AddWidget(path, "Low Resolution Mode", WIDGET_CVAR_COMBOBOX)
        .Tag("Restart required")
        .ApplyPolicyOf(ApplyPolicy::RequiresRestart)
        .CVar("gLowResModePending")
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Forces the internal framebuffer to a low resolution. "
                              "Stretched modes scale the framebuffer to fill the game viewport. "
                              "Pixel-perfect modes render N64-native 320x240 and draw it at an integer "
                              "factor, centred with black borders — sharper retro look, no sub-pixel blur. "
                              "Auto-fit picks the largest factor that still fits in the window. "
                              "Overrides Internal Resolution while active. "
                              "Latched at startup — changes take effect on next launch (toggling mid-session "
                              "would resize the framebuffer and race the Metal renderer).")
                     .ComboMap(kLowResModeMap)
                     .DefaultIndex(0));

    path.sidebarName = "Image Quality";
#ifndef __WIIU__
    AddWidget(path, "Anti-aliasing (MSAA)", WIDGET_CVAR_SLIDER_INT)
        .CVar(CVAR_MSAA_VALUE)
        .RaceDisable(false)
        .Callback([](WidgetInfo&) {
            Ship::Context::GetInstance()->GetWindow()->SetMsaaLevel(CVarGetInteger(CVAR_MSAA_VALUE, 1));
        })
        .Options(IntSliderOptions()
                     .Tooltip("Sets the MSAA level for rendered geometry.")
                     .Min(1)
                     .Max(8)
                     .DefaultValue(1));
#endif
    AddSettingsImageQuality(path);
    AddSettingsAdvanced(path);
}

/* Image Quality sidebar registration — renderer, filtering, CRT
 * shaders and overscan toggles (interleaved with Display above). */
void PortMenu::AddSettingsImageQuality(WidgetPath& path) {
/* Shader stack lives under Customization now — see AddMenuCustomization. */

path.sidebarName = "Display";
AddWidget(path, "Renderer API", WIDGET_VIDEO_BACKEND).RaceDisable(false)
    .Tag("Restart required")
    .Options(ComboboxOptions().Tooltip("Graphics backend (DirectX 11, OpenGL, Metal). Requires a restart."))
    .SearchTerms({ "directx", "opengl", "metal", "graphics backend", "gpu" });
AddWidget(path, "Enable Vsync", WIDGET_CVAR_CHECKBOX)
    .CVar(CVAR_VSYNC_ENABLED)
    .RaceDisable(false)
    .SearchTerms({ "vsync", "tearing", "lag", "latency", "fps cap" })
    .PreFunc([this](WidgetInfo& info) { info.isHidden = disabledMap.at(DISABLE_FOR_NO_VSYNC).active; })
    .Options(CheckboxOptions().Tooltip("Removes tearing, but can cap the game to the display refresh rate.")
                 .DefaultValue(true));

path.sidebarName = "Image Quality";
AddWidget(path, "Enhanced Framerate (Interpolated)", WIDGET_CVAR_COMBOBOX)
    .CVar(PORT_INTERP_CVAR_FPS)
    .RaceDisable(false)
    .Callback([](WidgetInfo&) { portInterpApplyConfig(); })
    .PreFunc([this](WidgetInfo& info) {
        if (disabledMap.at(DISABLE_FOR_MATCH_REFRESH_RATE_ON).active) {
            info.activeDisables.push_back(DISABLE_FOR_MATCH_REFRESH_RATE_ON);
        }
    })
    .Options(ComboboxOptions()
                 .Tooltip("Renders extra frames between 60 Hz game ticks by interpolating object "
                          "and camera motion. Game logic, physics, and input timing stay at exactly "
                          "60 Hz; only rendering is smoother. Adds up to one tick (16 ms) of visual "
                          "latency. Requires a display refresh rate at or above the chosen value "
                          "(or Vsync off) — if the host can't keep up, the game automatically steps "
                          "back down to protect game speed.")
                 .ComboMap(kEnhancedFpsMap)
                 .DefaultIndex(0));

AddWidget(path, "Match Display Refresh Rate", WIDGET_CVAR_CHECKBOX)
    .CVar(PORT_INTERP_CVAR_MATCH_DISPLAY)
    .RaceDisable(false)
    .Callback([](WidgetInfo&) { portInterpApplyConfig(); })
    .Options(CheckboxOptions()
                 .Tooltip("Locks interpolated rendering to the active display refresh rate, from 60 through "
                          "240 Hz. Fractional frame cadence preserves the fixed 60 Hz game simulation on "
                          "90/120/144/165 Hz phones, TVs, monitors, and Xbox display modes.")
                 .DefaultValue(false));

path.sidebarName = "Display";
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
AddWidget(path, "Windowed Fullscreen", WIDGET_CVAR_CHECKBOX)
    .CVar(CVAR_SDL_WINDOWED_FULLSCREEN)
    .RaceDisable(false)
    .PreFunc([this](WidgetInfo& info) {
        info.isHidden = disabledMap.at(DISABLE_FOR_NO_WINDOWED_FULLSCREEN).active;
    })
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Enables borderless windowed fullscreen mode."));
#endif
}

/* Advanced sidebar registration — low-level toggles most players
 * never need; hidden unless Menu.Mode = Advanced. */
void PortMenu::AddSettingsAdvanced(WidgetPath& path) {
path.sidebarName = "Advanced";
AddSidebarEntry("Settings", "Advanced", 1);
AddWidget(path, "Advanced", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Allow multi-windows", WIDGET_CVAR_CHECKBOX)
    .Tag("Experimental")
    .CVar(CVAR_ENABLE_MULTI_VIEWPORTS)
    .RaceDisable(false)
    .PreFunc([this](WidgetInfo& info) {
        info.isHidden = disabledMap.at(DISABLE_FOR_NO_MULTI_VIEWPORT).active;
    })
    .Options(CheckboxOptions()
                 .Tooltip("Allows multiple ImGui windows to be opened at once. Requires reload to take effect.")
                 .DefaultValue(true));

path.sidebarName = "Image Quality";
AddWidget(path, "Texture Filter", WIDGET_CVAR_COMBOBOX)
    .Tag("Restart required")
    .ApplyPolicyOf(ApplyPolicy::RequiresRestart)
    .CVar(CVAR_TEXTURE_FILTER)
    .RaceDisable(false)
    .Options(ComboboxOptions().DefaultIndex(0).Tooltip("Sets the active texture filtering mode.").ComboMap(kTextureFilteringMap));

path.sidebarName = "Display";
AddWidget(path, "Widescreen", WIDGET_CVAR_CHECKBOX)
    .Tag("Experimental")
    .SearchTerms({ "aspect ratio", "ultrawide", "16:9", "anamorphic" })
    .CVar(enhancements::WidescreenCVarName())
    .RaceDisable(false)
    .Options(CheckboxOptions()
                 .Tooltip(
                     "Renders battle scenes with a wider field of view to fill widescreen "
                     "windows. Title screen, menus, and intros stretch to fill the window "
                     "(no widescreen art). Some stages may show authored background edges "
                     "that were never visible in 4:3. Experimental — restart required for "
                     "the toggle to take effect.")
                 .DefaultValue(true));

// Overscan crop. SSB64 bakes a 10px title-safe margin into its GBI scissor on
// every side (decomp sys/rdp.c + sys/objdisplay.c); a CRT's overscan hid it,
// but a desktop window shows it as a black border framing the whole image.
// libultraship's Gui::DrawGame crops this proportional margin out of the
// presented framebuffer so the safe area fills the viewport. Takes effect
// immediately (read every frame at present time, no reload needed).
path.sidebarName = "Image Quality";
AddWidget(path, "Overscan", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Crop Overscan Border", WIDGET_CVAR_CHECKBOX)
    .CVar(CVAR_PREFIX_ADVANCED_RESOLUTION ".Overscan.Enabled")
    .RaceDisable(false)
    .Options(CheckboxOptions()
                 .Tooltip("Removes the black border framing the image by cropping the game's built-in "
                          "title-safe margin, so the picture fills the window like it filled a CRT. "
                          "Disable to see the raw N64 framebuffer including its overscan border.")
                 .DefaultValue(true));

AddWidget(path, "Overscan Crop — Horizontal", WIDGET_CVAR_SLIDER_FLOAT)
    .CVar(CVAR_PREFIX_ADVANCED_RESOLUTION ".Overscan.Horizontal")
    .RaceDisable(false)
    .Options(FloatSliderOptions()
                 .Tooltip("How many N64 pixels (of 320 wide) to crop from each of the left and right "
                          "edges. The game's authored margin is 10.")
                 .Min(0.0f)
                 .Max(32.0f)
                 .DefaultValue(10.0f));

AddWidget(path, "Overscan Crop — Vertical", WIDGET_CVAR_SLIDER_FLOAT)
    .CVar(CVAR_PREFIX_ADVANCED_RESOLUTION ".Overscan.Vertical")
    .RaceDisable(false)
    .Options(FloatSliderOptions()
                 .Tooltip("How many N64 pixels (of 240 tall) to crop from each of the top and bottom "
                          "edges. The game's authored margin is 10.")
                 .Min(0.0f)
                 .Max(32.0f)
                 .DefaultValue(10.0f));

path.sidebarName = "Advanced";
AddWidget(path, "Enable Developer Tools", WIDGET_CVAR_CHECKBOX)
    .CVar("gSettings.EnableDevTools")
    .RaceDisable(false)
    .Callback([this](WidgetInfo&) { RebuildMenu(); })
    .Options(CheckboxOptions().DefaultValue(false).Tooltip(
        "Shows the Developer sidebar in Settings (Stats, Console, Gfx Debugger)."));

path.sidebarName = "Match Rules";
}


void PortMenu::AddSettingsGameplay(WidgetPath& path) {
    AddSidebarEntry("Settings", "Match Rules", 1);

    AddWidget(path, "Disable Stage Hazards", WIDGET_CVAR_CHECKBOX)
        .CVar(enhancements::StageHazardsDisabledCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "Disables stage specific hazards such as moving platforms, tornadoes, Arwings, wind, etc."
            "Does not take effect mid-battle, only between battles.")
                     .DefaultValue(false));
    path.sidebarName = "Classic Mode";
    AddSidebarEntry("Settings", "Classic Mode", 1);
    AddWidget(path, "Classic Mode", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Bonus Stages", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::BonusStagesCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions()
                     .Tooltip("Shows the port-added bonus stage page (Final Destination, "
                              "Metal Cavern, Battlefield) on the VS / Training stage select "
                              "screen. Turn off for the original stage roster. Takes effect "
                              "the next time the stage select screen opens.")
                     .DefaultValue(true));
    path.sidebarName = "Practice";
    AddSidebarEntry("Settings", "Practice", 1);
    AddWidget(path, "Practice", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Hitbox View", WIDGET_CVAR_COMBOBOX)
        .CVar(enhancements::HitboxViewCVarName())
        .RaceDisable(false)
        .Options(ComboboxOptions()
                     .Tooltip("Replaces fighter/item/weapon rendering with the debug "
                              "hitbox visualization the decomp ships. Filled mode "
                              "draws red cubes for active hitboxes; Outline mode "
                              "draws hitbox edges and shows hurtboxes as solid "
                              "yellow/green/blue boxes (yellow=normal, green="
                              "invincible, blue=intangible).")
                     .ComboMap(kHitboxViewMap)
                     .DefaultIndex(0));

    path.sidebarName = "Match Rules";
    AddWidget(path, "Competitive Rules", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Use Competitive Ruleset", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::CompRulesetCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip("Forces the following in VS Mode: \n"
                                           "- 4 Stocks \n"
                                           "- 8 Minutes \n"
                                           "- Items Off \n"
                                           "- Team Attack ON \n"
                                           "- Dream Land only"));
    AddWidget(path, "Neutral Spawns on Dreamland", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::NeutralSpawnsCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip("Forces balanced, opposite-side starting positions for 1v1 and Team Battles."));

    path.sidebarName = "Practice";
    AddWidget(path, "Z-Cancel Options", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Auto Z-Cancel", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::AutoZCancelCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip("Automatically cancels the landing lag from aerial attacks."));
    AddWidget(path, "Flash on Failed Z-Cancel", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::FailedZCancelCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip("Makes your character temporarily flash in flames on a failed Z-Cancel."));
    AddSettingsInterface(path);
}

/* Interface sidebar registration — HUD and presentation toggles. */
void PortMenu::AddSettingsInterface(WidgetPath& path) {
path.sidebarName = "Interface";
AddSidebarEntry("Settings", "Interface", 1);
AddWidget(path, "Interface", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Boot to VS CSS", WIDGET_CVAR_CHECKBOX)
    .CVar(ssb64::enhancements::BootToVSCSSCVarName())
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Skips the intro sequences and boots directly to the VS Mode Character Select Screen."));
path.sidebarName = "Classic Mode";
AddWidget(path, "Classic Co-op", WIDGET_CVAR_CHECKBOX)
    .Tag("Restart required")
    .ApplyPolicyOf(ApplyPolicy::RequiresRestart)
    .CVar(ssb64::enhancements::ClassicCoopCVarName())
    .RaceDisable(false)
    .Options(CheckboxOptions()
                 .Tooltip("Classic (1P) mode uses the VS Mode Character Select Screen so a "
                          "second player can join the whole run as a co-op partner. If only "
                          "one player picks a character, the run plays as normal solo "
                          "Classic. Restart required for the toggle to take effect.")
                 .DefaultValue(true));
AddWidget(path, "Co-op Friendly Fire", WIDGET_CVAR_CHECKBOX)
    .CVar(ssb64::enhancements::ClassicCoopFriendlyFireCVarName())
    .RaceDisable(false)
    .Options(CheckboxOptions()
                 .Tooltip("Lets the two Classic Co-op players hit each other, like the "
                          "original 1P mode's CPU ally could. Off by default; takes effect "
                          "from the next stage.")
                 .DefaultValue(false));
path.sidebarName = "Interface";
AddWidget(path, "Skip Results Screen", WIDGET_CVAR_CHECKBOX)
    .CVar(ssb64::enhancements::SkipResultsScreenCVarName())
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Skips the victory podium and returns directly to the Character Select Screen."));
path.sidebarName = "Match Rules";
AddWidget(path, "Force CPU Level 9", WIDGET_CVAR_CHECKBOX)
    .CVar(ssb64::enhancements::CpuLevel9CVarName())
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Automatically forces all CPU players to Level 9."));
path.sidebarName = "Interface";
    AddWidget(path, "Disable HUD", WIDGET_CVAR_CHECKBOX)
    .CVar(ssb64::enhancements::DisableHUDCVarName())
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Disables the in-game HUD. Note that some icons will still be visible (e.g. CPU icons in 1P Mode on the top-left.)"));

// --- Input customization ---
path.sidebarName = "Input Mappings";
path.column = SECTION_COLUMN_1;
}


void PortMenu::AddSettingsInput(WidgetPath& path) {
    AddSidebarEntry("Settings", "Input Mappings", 1);

    AddWidget(path, "Controller", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Controller Configuration", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_CONTROLLER_CONFIGURATION_WINDOW_OPEN)
        .RaceDisable(false)
        .WindowName("Input Editor")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the controller configuration window."));
    AddSettingsControls(path);
}

/* Controls sidebar — per-player input remapping table and its
 * supporting controller options. */
void PortMenu::AddSettingsControls(WidgetPath& path) {
// --- Controls sidebar: per-player input remapping ---
path.sidebarName = "Control Enhancements";
path.column = SECTION_COLUMN_1;
AddSidebarEntry("Settings", "Control Enhancements", 1);

for (int p = 0; p < PORT_ENHANCEMENT_MAX_PLAYERS; ++p) {
    const std::string playerLabel = fmt::format(fmt::runtime(TR("Player {}")), p + 1);
    AddWidget(path, playerLabel, WIDGET_SEPARATOR_TEXT);

    AddWidget(path, fmt::format(fmt::runtime(TR("Disable Tap Jump (P{})")), p + 1), WIDGET_CVAR_CHECKBOX)
        .CVar(enhancements::TapJumpCVarName(p))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Disables jumping by pushing up on the analog stick."));

    AddWidget(path, fmt::format(fmt::runtime(TR("C-Stick Smash (P{})")), p + 1), WIDGET_CVAR_CHECKBOX)
        .CVar(enhancements::CStickSmashCVarName(p))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Replaces C-Button inputs with instant smash attacks."));

    AddWidget(path, fmt::format(fmt::runtime(TR("D-Pad to Jump (P{})")), p + 1), WIDGET_CVAR_CHECKBOX)
        .CVar(enhancements::DPadJumpCVarName(p))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip("Translates N64 D-Pad inputs into C-Up (Jump)."));

    AddWidget(path, fmt::format(fmt::runtime(TR("NRage Analog Stick Remap (P{})")), p + 1), WIDGET_CVAR_CHECKBOX)
        .CVar(enhancements::AnalogRemapCVarName(p))
        .RaceDisable(false)
        .Options(CheckboxOptions().Tooltip(
            "When ON: replaces libultraship's stick processing with the NRage per-axis "
            "deadzone+range formula (no octagonal gate, no radial deadzone, no notch snap). "
            "When OFF: libultraship's stock pipeline runs unchanged. Both schemes coexist; "
            "this toggle picks which one runs for this player."));

    const char* deadzoneCVar = enhancements::AnalogRemapDeadzoneCVarName(p);
    const char* enableCVar   = enhancements::AnalogRemapCVarName(p);
    AddWidget(path, fmt::format(fmt::runtime(TR("Remap Deadzone (P{})")), p + 1), WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(deadzoneCVar)
        .RaceDisable(false)
        .PreFunc([enableCVar](WidgetInfo& info) {
            if (!CVarGetInteger(enableCVar, 0)) {
                info.options->disabled = true;
                info.options->disabledTooltip = "Enable NRage Analog Stick Remap to adjust this.";
            }
        })
        .DisabledAction("Enable NRage Remap", [enableCVar] {
            CVarSetInteger(enableCVar, 1);
            CVarSave();
        })
        .Options(FloatSliderOptions()
                     .Min(0.0f).Max(0.99f).DefaultValue(0.10f)
                     .IsPercentage()
                     .Tooltip("Per-axis deadzone (0–99%). Inputs inside this fraction of "
                              "max deflection on each axis read as 0."));

    const char* rangeCVar = enhancements::AnalogRemapRangeCVarName(p);
    AddWidget(path, fmt::format(fmt::runtime(TR("Remap Range (P{})")), p + 1), WIDGET_CVAR_SLIDER_FLOAT)
        .CVar(rangeCVar)
        .RaceDisable(false)
        .PreFunc([enableCVar](WidgetInfo& info) {
            if (!CVarGetInteger(enableCVar, 0)) {
                info.options->disabled = true;
                info.options->disabledTooltip = "Enable NRage Analog Stick Remap to adjust this.";
            }
        })
        .DisabledAction("Enable NRage Remap", [enableCVar] {
            CVarSetInteger(enableCVar, 1);
            CVarSave();
        })
        .Options(FloatSliderOptions()
                     .Min(0.5f).Max(1.5f).DefaultValue(1.0f)
                     .IsPercentage()
                     .Tooltip("Output scale (50–150%). 100% caps at the N64's natural max."));
}

AddWidget(path, "Copy Player 1 settings to all players", WIDGET_BUTTON)
    .RaceDisable(false)
    .Callback([](WidgetInfo&) {
        for (int p = 1; p < PORT_ENHANCEMENT_MAX_PLAYERS; ++p) {
            for (const auto& fn : { enhancements::TapJumpCVarName, enhancements::CStickSmashCVarName,
                                    enhancements::DPadJumpCVarName, enhancements::AnalogRemapCVarName }) {
                CVarSetInteger(fn(p), CVarGetInteger(fn(0), 0));
            }
            for (const auto& fn : { enhancements::AnalogRemapDeadzoneCVarName,
                                    enhancements::AnalogRemapRangeCVarName }) {
                CVarSetFloat(fn(p), CVarGetFloat(fn(0), 1.0f));
            }
        }
        CVarSave();
        ToastSuccess(TR("Player 1 settings copied to players 2-4"));
    })
    .Options(ButtonOptions().Tooltip("Copies every Control Enhancements value from Player 1 to "
                                     "players 2-4, including NRage deadzone and range."));

    AddSettingsProfiles(path);

// --- Audio ---
path.sidebarName = "Audio";
path.column = SECTION_COLUMN_1;
}


/* Profiles sidebar — capture/apply/delete controller+enhancement
 * snapshots. */
void PortMenu::AddSettingsProfiles(WidgetPath& path) {
AddWidget(path, "Profiles", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "A profile captures all controller mappings and control enhancements for quick switching.", WIDGET_TEXT);
AddWidget(path, "New profile name", WIDGET_CVAR_INPUT)
.CVar("gProfiles.Name")
.RaceDisable(false)
.Options(InputOptions().DefaultValue("")
             .PlaceholderText("e.g. tournament-ready")
             .Tooltip("Name that 'Save current as profile' will use."));

AddWidget(path, "Save current as profile", WIDGET_BUTTON)
.RaceDisable(false)
.Callback([](WidgetInfo&) {
    const char* name = CVarGetString("gProfiles.Name", "");
    if (name == nullptr || name[0] == '\0') {
        ToastError(TR("Type a profile name first"));
        return;
    }
    if (port_profile_save(name) == 0) {
        ToastSuccess(fmt::format(fmt::runtime(TR("Profile '{}' saved")), name).c_str());
    } else {
        ToastError(TR("Could not save profile - use only letters, numbers, spaces, - and _"));
    }
})
.Options(ButtonOptions().Tooltip(
    "Stores the current controller mappings and control enhancements under the given name."));

AddWidget(path, "Profile", WIDGET_CVAR_COMBOBOX)
.CVar("gProfiles.Selected")
.RaceDisable(false)
.PreFunc([](WidgetInfo& info) {
    static std::vector<std::string> sNames;
    const auto fresh = PortProfileNames();
    /* comboMap labels point into sNames — only rebuild when the list
     * actually changes; otherwise every frame's reassignment leaves
     * the stored c_str() pointers dangling. */
    if (fresh != sNames) {
        sNames = fresh;
        auto opts = std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options);
        opts->comboMap.clear();
        for (int i = 0; i < static_cast<int>(sNames.size()); i++) {
            opts->comboMap[i] = sNames[i].c_str();
        }
    }
    if (sNames.empty()) {
        info.options->disabled = true;
        info.options->disabledTooltip = TR("No saved profiles yet - save one above.");
    }
})
.Options(ComboboxOptions().DefaultIndex(0).Tooltip("Pick which saved profile to load or delete."));

AddWidget(path, "Load selected profile", WIDGET_BUTTON)
.RaceDisable(false)
.PreFunc([](WidgetInfo& info) {
    if (PortProfileSelectedName().empty()) {
        info.options->disabled = true;
        info.options->disabledTooltip = TR("Select a profile first.");
    }
})
.Callback([](WidgetInfo&) {
    const std::string name = PortProfileSelectedName();
    if (port_profile_load(name.c_str()) == 0) {
        ToastSuccess(fmt::format(fmt::runtime(TR("Profile '{}' loaded - controllers remapped")), name).c_str());
    } else {
        ToastError(TR("Failed to load profile"));
    }
})
.Options(ButtonOptions().Tooltip("Restores the profile's mappings and enhancements."));

AddWidget(path, "Delete selected profile", WIDGET_BUTTON)
.RaceDisable(false)
.PreFunc([](WidgetInfo& info) {
    if (PortProfileSelectedName().empty()) {
        info.options->disabled = true;
        info.options->disabledTooltip = TR("Select a profile first.");
    }
})
.Callback([](WidgetInfo&) {
    const std::string name = PortProfileSelectedName();
    if (port_profile_delete(name.c_str()) == 0) {
        ToastInfo(fmt::format(fmt::runtime(TR("Profile '{}' deleted")), name).c_str());
    } else {
        ToastError(TR("Failed to delete profile"));
    }
})
.Options(ButtonOptions()
             .Color(UIWidgets::Colors::Red)
             .Tooltip("Deletes the profile file. Current settings are not affected."));
}


void PortMenu::AddSettingsAudio(WidgetPath& path) {
    AddSidebarEntry("Settings", "Audio", 1);

    AddWidget(path, "Volume Levels", WIDGET_SEPARATOR_TEXT);


    // Master
    AddWidget(path, "Master Volume", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gSettings.Audio.MasterVolume")
        .RaceDisable(false)
        .Options(FloatSliderOptions()
        .Tooltip("Adjust the overall volume of the entire game.")
        .DefaultValue(1.0f)
        .Min(0.0f)
        .Max(1.0f)
        .IsPercentage());

    // Music Volume Slider
    AddWidget(path, "Music Volume", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gSettings.Audio.MusicVolume")
        .RaceDisable(false)
        .Options(FloatSliderOptions()
        .Tooltip("Adjust the volume of the background music.")
        .DefaultValue(1.0f)
        .Min(0.0f)
        .Max(1.0f)
        .IsPercentage());

    AddWidget(path, "Shuffle Music", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::ShuffleMusicCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip("Randomizes the BGM after a stage is chosen. Useful when enabling the competitive ruleset."));
    AddWidget(path, "Add Music Selection Screen", WIDGET_CVAR_CHECKBOX)
        .CVar(ssb64::enhancements::MusicSelectionCVarName())
        .RaceDisable(false)
        .Options(CheckboxOptions().DefaultValue(false).Tooltip("Allows the player to pick a custom BGM track after the stage selection screen (music shuffling is ignored if this is turned on)."));

    // Sound Effects Volume Slider
    AddWidget(path, "Sound Volume", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gSettings.Audio.SFXVolume") // Standard LUS native CVar
        .RaceDisable(false)
        .Options(FloatSliderOptions()
        .Tooltip("Adjust the volume of sound effects and voices.")
        .DefaultValue(1.0f)
        .Min(0.0f)
        .Max(1.0f)
        .IsPercentage());

    // voice
    AddWidget(path, "Voice Volume", WIDGET_CVAR_SLIDER_FLOAT)
        .CVar("gSettings.Audio.VoiceVolume")
        .RaceDisable(false)
        .Options(FloatSliderOptions()
        .Tooltip("Adjust the volume of character voices and the announcer.")
        .DefaultValue(1.0f)
        .Min(0.0f)
        .Max(1.0f)
        .IsPercentage());

    AddWidget(path, "Focus Behavior", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Mute on Focus Loss", WIDGET_CVAR_CHECKBOX)
        .CVar("gSettings.FocusControl.MuteOnFocusLoss")
        .RaceDisable(false)
        .Options(CheckboxOptions()
            .Tooltip("Silences game audio while the window is unfocused. Restores volume on refocus.")
            .DefaultValue(false));
    AddSettingsAccessibility(path);
    AddSettingsCheats(path);
}

/* Accessibility sidebar — captions, contrast and reduced-motion
 * options. */
void PortMenu::AddSettingsAccessibility(WidgetPath& path) {
// --- Accessibility ---
path.sidebarName = "Accessibility";
path.column = SECTION_COLUMN_1;
AddSidebarEntry("Settings", "Accessibility", 1);

AddWidget(path, "Visual", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "UI Scale", WIDGET_CVAR_SLIDER_FLOAT)
    .CVar("gSettings.UIScale")
    .RaceDisable(false)
    .Options(FloatSliderOptions()
                 .Tooltip("Scales the menu text and controls. Applies immediately.")
                 .DefaultValue(1.0f)
                 .Min(0.75f)
                 .Max(2.5f)
                 .IsPercentage());
AddWidget(path, "High Contrast Focus", WIDGET_CVAR_CHECKBOX)
    .CVar("gSettings.HighContrastFocus")
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip(
        "Makes the keyboard/controller focus ring near-opaque so the focused control is easy to spot."));

AddWidget(path, "Text & Help", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Persistent Descriptions", WIDGET_CVAR_CHECKBOX)
    .CVar("gSettings.PersistentDescriptions")
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip(
        "Shows each option's help text under the control instead of requiring a hover. Useful with gamepad or touch."));
AddWidget(path, "Narration", WIDGET_CVAR_CHECKBOX)
    .CVar("gSettings.Narration")
    .RaceDisable(false)
    .SearchTerms({ "screen reader", "speech", "speak", "blind", "accessibility" })
    .Options(CheckboxOptions().DefaultValue(false).Tooltip(
        "Speaks the name of each menu option as it gains focus, via the OS voice. "
        "Currently Windows (SAPI) only — does nothing on other platforms."));

AddWidget(path, "Motion", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Reduce Motion", WIDGET_CVAR_CHECKBOX)
    .CVar("gSettings.ReducedMotion")
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Disables blinking/animated UI elements such as the text cursor."));
}

/* Cheats + hidden Developer-tools sidebars. */
void PortMenu::AddSettingsCheats(WidgetPath& path) {
// --- Cheats ---
path.sidebarName = "Cheats";
path.column = SECTION_COLUMN_1;
AddSidebarEntry("Settings", "Cheats", 1);

AddWidget(path, "Master Unlocks", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Unlock Everything", WIDGET_CVAR_CHECKBOX)
    .CVar("gCheats.UnlockAll")
    .RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Unlocks every character and bonus feature at once."));

AddWidget(path, "Individual Characters", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Unlock Captain Falcon", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockCaptain").RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Adds Captain Falcon to the character select screen."));
AddWidget(path, "Unlock Jigglypuff", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockPurin").RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Adds Jigglypuff to the character select screen."));
AddWidget(path, "Unlock Luigi", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockLuigi").RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Adds Luigi to the character select screen."));
AddWidget(path, "Unlock Ness", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockNess").RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Adds Ness to the character select screen."));

AddWidget(path, "Individual Features", WIDGET_SEPARATOR_TEXT);
AddWidget(path, "Unlock Mushroom Kingdom", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockInishie").RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Unlocks the hidden Mushroom Kingdom stage."));
AddWidget(path, "Unlock Item Switch Menu", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockItemSwitch").RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Enables the VS-mode item switch menu."));
AddWidget(path, "Unlock Sound Test Menu", WIDGET_CVAR_CHECKBOX).CVar("gCheats.UnlockSoundTest").RaceDisable(false)
    .Options(CheckboxOptions().DefaultValue(false).Tooltip("Enables the sound test screen in the Data menu."));

// --- Developer tools ---
// Hidden behind a CVar so the average player doesn't see debugging
// tools mixed with normal settings. Toggling the CVar rebuilds the menu.
if (CVarGetInteger("gSettings.EnableDevTools", 0)) {
    path.sidebarName = "Developer";
    path.column = SECTION_COLUMN_1;
    AddSidebarEntry("Settings", "Developer", 1);

    AddWidget(path, "Debug Windows", WIDGET_SEPARATOR_TEXT);
    AddWidget(path, "Stats", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_STATS_WINDOW_OPEN)
        .RaceDisable(false)
        .WindowName("Stats")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the Stats window."));
    AddWidget(path, "Console", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_CONSOLE_WINDOW_OPEN)
        .RaceDisable(false)
        .WindowName("Console")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the Console window."));
    AddWidget(path, "Gfx Debugger", WIDGET_WINDOW_BUTTON)
        .CVar(CVAR_GFX_DEBUGGER_WINDOW_OPEN)
        .RaceDisable(false)
        .WindowName("GfxDebuggerWindow")
        .HideInSearch(true)
        .Options(WindowButtonOptions().Tooltip("Toggles the graphics debugger window."));
}
}



/* Extraction sidebar — status card instead of a bare sentence: whether the
 * runtime archive is usable, its size, the build region, and whether a
 * recoverable .bak is sitting next to it. */
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

void PortMenu::DrawElement() {
    if (mMenuElementsInitialized) {
        Ship::Menu::DrawElement();
    }

    // Re-extract confirmation: render as an in-context ImGui modal rather
    // than calling SDL_ShowSimpleMessageBox, which would nest a blocking
    // native modal (NSAlert / Win32 MessageBox) inside the in-progress
    // ImGui frame and the GPU command encode for that frame. ImGui's own
    // popup runs in the same context with no thread/native-modal nesting.
    if (mShowReextractMessage) {
        ImGui::OpenPopup("Re-extract scheduled");
        mShowReextractMessage = false;
    }
    const ImVec2 viewportCenter = ImGui::GetMainViewport()->GetCenter();
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

    /* Display-change auto-revert: countdown modal after toggling fullscreen.
     * Keep commits the new mode; timeout or Revert undoes the toggle. */
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

    RenderPostProcessLowResWarnModal();
#if !defined(BATTLESHIP_UWP)
    RenderShaderPackModal();
#endif
}

} // namespace ssb64
