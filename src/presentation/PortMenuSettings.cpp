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

namespace {

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
    ImGui::TextUnformatted(TR("Menu Background"));
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

} // namespace ssb64
