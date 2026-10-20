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
#include "PortMenuSettings_internal.h"
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


/* Navigation globals owned by Menu.cpp — declared in Menu_internal.h. */
#include "Menu_internal.h"

namespace ssb64 {

using namespace UIWidgets;

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
        ToastSuccess(fmt::format(fmt::runtime(TR("Profile '{}' saved")), name));
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
        ToastSuccess(fmt::format(fmt::runtime(TR("Profile '{}' loaded - controllers remapped")), name));
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
        ToastInfo(fmt::format(fmt::runtime(TR("Profile '{}' deleted")), name));
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

} // namespace ssb64
