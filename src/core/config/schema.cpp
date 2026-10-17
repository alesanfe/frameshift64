/* See schema.h. */

#include "schema.h"

#include "port_log.h"

#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>
#include <ship/config/ConsoleVariable.h>

#include <algorithm>
#include <cstring>

namespace ssb64::config {
namespace {

/* The LUS store keeps the JSON's declared type, so a hand-edited
 * `gNetplay.Delay: 5.0` reads back as Float — `CVarGetInteger` then
 * silently returns the caller default forever. Warn once at boot. */
void WarnTypeMismatch(const SchemaEntry& e) {
    auto cv = Ship::Context::GetInstance()->GetConsoleVariables();
    const auto var = cv ? cv->Get(e.key) : nullptr;
    if (!var) return;
    const bool wantInt  = (e.type == KeyType::Int || e.type == KeyType::Bool);
    const bool typeOk   = (wantInt  && var->Type == Ship::ConsoleVariableType::Integer) ||
                          (e.type == KeyType::Float  && var->Type == Ship::ConsoleVariableType::Float) ||
                          (e.type == KeyType::String && var->Type == Ship::ConsoleVariableType::String);
    if (!typeOk) {
        port_log_ex(PORT_LOG_WARN, "config", "SSB64 Config: %s has the wrong stored type — every read "
                 "returns the caller default\n", e.key);
    }
}


/* Every first-party `g*` key. Ranges are generous — the point is catching
 * corruption/garbage (e.g. volume = -3e38, delay = -1), not restricting
 * legitimate experimentation. Gameplay keys mirror the rules the netplay
 * handshake already negotiates plus flags a peer could care about. */
constexpr int kConfigSchemaVersion = 1;

constexpr SchemaEntry kSchema[] = {
    /* ── input enhancements (alter the produced input stream) ─────────── */
    { "gEnhancements.TapJumpDisabled.P1", KeyType::Bool, 0, 1, true  },
    { "gEnhancements.TapJumpDisabled.P2", KeyType::Bool, 0, 1, true  },
    { "gEnhancements.TapJumpDisabled.P3", KeyType::Bool, 0, 1, true  },
    { "gEnhancements.TapJumpDisabled.P4", KeyType::Bool, 0, 1, true  },
    { "gEnhancements.CStickSmash.P1",     KeyType::Bool, 0, 1, true  },
    { "gEnhancements.CStickSmash.P2",     KeyType::Bool, 0, 1, true  },
    { "gEnhancements.CStickSmash.P3",     KeyType::Bool, 0, 1, true  },
    { "gEnhancements.CStickSmash.P4",     KeyType::Bool, 0, 1, true  },
    { "gEnhancements.DPadJump.P1",        KeyType::Bool, 0, 1, true  },
    { "gEnhancements.DPadJump.P2",        KeyType::Bool, 0, 1, true  },
    { "gEnhancements.DPadJump.P3",        KeyType::Bool, 0, 1, true  },
    { "gEnhancements.DPadJump.P4",        KeyType::Bool, 0, 1, true  },
    /* AnalogRemap deadzone/range are floats — AnalogRemap.cpp clamps them
     * itself ([0.0,0.99] / [0.50,1.50]); the schema mirrors those bounds. */
    { "gEnhancements.AnalogRemap.P1.Enabled",  KeyType::Bool,  0, 1,    true },
    { "gEnhancements.AnalogRemap.P1.Range",    KeyType::Float, 0.5, 1.5, true },
    { "gEnhancements.AnalogRemap.P1.Deadzone", KeyType::Float, 0.0, 0.99,true },
    { "gEnhancements.AnalogRemap.P2.Enabled",  KeyType::Bool,  0, 1,    true },
    { "gEnhancements.AnalogRemap.P2.Range",    KeyType::Float, 0.5, 1.5, true },
    { "gEnhancements.AnalogRemap.P2.Deadzone", KeyType::Float, 0.0, 0.99,true },
    { "gEnhancements.AnalogRemap.P3.Enabled",  KeyType::Bool,  0, 1,    true },
    { "gEnhancements.AnalogRemap.P3.Range",    KeyType::Float, 0.5, 1.5, true },
    { "gEnhancements.AnalogRemap.P3.Deadzone", KeyType::Float, 0.0, 0.99,true },
    { "gEnhancements.AnalogRemap.P4.Enabled",  KeyType::Bool,  0, 1,    true },
    { "gEnhancements.AnalogRemap.P4.Range",    KeyType::Float, 0.5, 1.5, true },
    { "gEnhancements.AnalogRemap.P4.Deadzone", KeyType::Float, 0.0, 0.99,true },

    /* ── gameplay / match rules (sim-visible) ─────────────────────────── */
    { "gEnhancements.CasualRules.AutoZCancel",       KeyType::Bool, 0, 1, true },
    { "gEnhancements.CasualRules.FailedZCancelFlash",KeyType::Bool, 0, 1, false },
    { "gEnhancements.CompRuleset",          KeyType::Bool, 0, 1, true  },
    { "gEnhancements.NeutralSpawns",        KeyType::Bool, 0, 1, true  },
    { "gEnhancements.StageHazardsDisabled", KeyType::Bool, 0, 1, true  },
    { "gEnhancements.ClassicCoop",          KeyType::Bool, 0, 1, true  },
    { "gEnhancements.ClassicCoopFriendlyFire", KeyType::Bool, 0, 1, true },
    { "gEnhancements.CpuLevel9",            KeyType::Bool, 0, 1, true  },
    { "gEnhancements.BonusStagesEnabled",   KeyType::Bool, 0, 1, true  },
    { "gEnhancements.BootToVSCSS",          KeyType::Bool, 0, 1, true  },
    { "gEnhancements.SkipResultsScreen",    KeyType::Bool, 0, 1, true  },

    /* ── presentation-only enhancements ──────────────────────────────── */
    { "gEnhancements.HitboxView",       KeyType::Bool, 0, 1, false },
    { "gEnhancements.DisableHUD",       KeyType::Bool, 0, 1, false },
    { "gEnhancements.ShuffleMusic",     KeyType::Bool, 0, 1, false },
    { "gEnhancements.MusicSelection",   KeyType::Bool, 0, 1, false },
    { "gEnhancements.Widescreen",       KeyType::Bool, 0, 1, false },

    /* ── unlocks (save-progress flags) ────────────────────────────────── */
    { "gCheats.UnlockAll",        KeyType::Bool, 0, 1, false },
    { "gCheats.UnlockCaptain",    KeyType::Bool, 0, 1, false },
    { "gCheats.UnlockInishie",    KeyType::Bool, 0, 1, false },
    { "gCheats.UnlockItemSwitch", KeyType::Bool, 0, 1, false },
    { "gCheats.UnlockLuigi",      KeyType::Bool, 0, 1, false },
    { "gCheats.UnlockNess",       KeyType::Bool, 0, 1, false },
    { "gCheats.UnlockPurin",      KeyType::Bool, 0, 1, false },
    { "gCheats.UnlockSoundTest",  KeyType::Bool, 0, 1, false },

    /* ── netplay ──────────────────────────────────────────────────────── */
    { "gNetplay.Delay",   KeyType::Int,    1, 15,  true  },
    { "gNetplay.AutoDelay", KeyType::Bool, 0, 1,   true  },
    { "gNetplay.Fighter", KeyType::Int,    0, 11,  true  },
    { "gNetplay.Items",   KeyType::Bool,   0, 1,   true  },
    { "gNetplay.Stage",   KeyType::Int,    0, 8,   true  },
    { "gNetplay.Stocks",  KeyType::Int,    1, 99,  true  },
    { "gNetplay.Server",  KeyType::String, 0, 0,   false },
    { "gProfiles.Name",   KeyType::String, 0, 0,   false },
    { "gProfiles.Selected",KeyType::Int, -100, 1000, false },

    /* ── audio ────────────────────────────────────────────────────────── */
    { "gSettings.Audio.MasterVolume", KeyType::Float, 0.0, 1.0, false },
    { "gSettings.Audio.MusicVolume",  KeyType::Float, 0.0, 1.0, false },
    { "gSettings.Audio.SFXVolume",    KeyType::Float, 0.0, 1.0, false },
    { "gSettings.Audio.VoiceVolume",  KeyType::Float, 0.0, 1.0, false },

    /* ── video / rendering ────────────────────────────────────────────── */
    { "gMSAAValue",                   KeyType::Int,   1,    16,   false },
    { "gInternalResolution",          KeyType::Float, 0.25, 8.0,  false },
    { "gTextureFilter",               KeyType::Int,   0,    4,    false },
    { "gVsyncEnabled",                KeyType::Bool,  0, 1, false },
    { "gSdlWindowedFullscreen",       KeyType::Bool,  0, 1, false },
    { "gLowResMode",                  KeyType::Int,   0, 4, false },
    { "gLowResModePending",           KeyType::Int,   0, 4, false },
    { "gEnhancedFps",                 KeyType::Int,   20, 360, false },
    { "gEnhancedFpsMatchDisplay",     KeyType::Bool,  0, 1, false },
    { "gPostProcessEnabled",          KeyType::Bool,  0, 1, false },
    { "gPostProcessShader",           KeyType::String,0, 0, false },
    { "gAdvancedResolution",          KeyType::Int,   0, 8, false },
    { "gAdvancedResolution.Enabled",  KeyType::Bool,  0, 1, false },
    { "gAdvancedResolution.AspectRatioX",              KeyType::Int, 1, 64, false },
    { "gAdvancedResolution.AspectRatioY",              KeyType::Int, 1, 64, false },
    { "gAdvancedResolution.VerticalPixelCount",        KeyType::Int, 1, 4320, false },
    { "gAdvancedResolution.VerticalResolutionToggle",  KeyType::Bool, 0, 1, false },
    { "gAdvancedResolution.PixelPerfectMode",          KeyType::Bool, 0, 1, false },
    { "gAdvancedResolution.IntegerScale.Factor",       KeyType::Int, 1, 8, false },
    { "gAdvancedResolution.IntegerScale.FitAutomatically",  KeyType::Bool, 0, 1, false },
    { "gAdvancedResolution.IntegerScale.NeverExceedBounds", KeyType::Bool, 0, 1, false },

    /* ── hi-res texture packs ─────────────────────────────────────────── */
    { "gHiResTextures.Enabled",       KeyType::Bool, 0, 1,     false },
    { "gHiResTextures.Preload",       KeyType::Bool, 0, 1,     false },
    { "gHiResTextures.CacheBudgetMB", KeyType::Int,  16, 4096, false },
    { "gHiResTextures.DumpMissRgba",  KeyType::Bool, 0, 1,     false },
    { "gHiResTextures.DumpSource",    KeyType::Bool, 0, 1,     false },
    { "gHiResTextures.MissDumpCap",   KeyType::Int,  0, 65536, false },

    /* ── UI / misc ────────────────────────────────────────────────────── */
    /* UI language — index into PortI18n::kLangCodes (src/core/i18n) and the
     * PortMenu language combo; English (0) is the identity table. */
    { "gSettings.Language",                   KeyType::Int,  0, 4, false },
    /* Progressive disclosure: 0 = Simple (advanced widgets/sidebars hidden
     * outside search), 1 = Advanced (everything visible, legacy default). */
    { "gSettings.MenuMode",                   KeyType::Int,  0, 1, false },
    { "gSettings.Menu.EnableDRP",             KeyType::Bool, 0, 1, false },
    { "gSettings.FocusControl.MuteOnFocusLoss", KeyType::Bool, 0, 1, false },
    /* Accessibility/UI toggles added with the menu help bar — Menu.cpp
     * clamps UIScale itself; the schema bounds mirror that clamp. */
    { "gSettings.UIScale",               KeyType::Float, 0.75, 2.5,  false },
    { "gSettings.PersistentDescriptions",KeyType::Bool,  0, 1,  false },
    { "gSettings.Narration",           KeyType::Bool,  0, 1,  false },
    { "gSettings.HighContrastFocus",     KeyType::Bool,  0, 1,  false },
    { "gSettings.ReducedMotion",         KeyType::Bool,  0, 1,  false },
    { "gSettings.EnableDevTools",        KeyType::Bool,  0, 1,  false },
    { "gConsoleEnabled",               KeyType::Bool, 0, 1, false },
    { "gControlNav",                   KeyType::Bool, 0, 1, false },
    { "gControllerConfigurationEnabled", KeyType::Bool, 0, 1, false },
    { "gAllowBackgroundInputs",        KeyType::Bool, 0, 1, false },
    { "gEnableMultiViewports",         KeyType::Bool, 0, 1, false },
    { "gStatsEnabled",                 KeyType::Bool, 0, 1, false },
    { "gGfxDebuggerEnabled",           KeyType::Bool, 0, 1, false },
    { "gOnlineStatus.Visible",         KeyType::Bool, 0, 1, false },
    { "gUpdatePrompt.Visible",         KeyType::Bool, 0, 1, false },
    { "gOpenPortMenu",                 KeyType::Int,   0, 64, false },
    { "gOpenWindows",                  KeyType::Int,   0, 64, false },
    { "gFirstRunHintShown",            KeyType::Bool, 0, 1, false },
    { "gGCFrameQueueGfxLinks",         KeyType::Int,   0, 65536, false },
    { "gUpdater.SkippedVersion",       KeyType::String, 0, 0,  false },
    { "gSettings",                     KeyType::String, 0, 0,  false },

    /* Schema stamp — bump kConfigSchemaVersion when a key is renamed or
     * changes meaning; a migration hook can then compare the stored value. */
    { "gConfig.SchemaVersion",         KeyType::Int,   0, 999, false },

    /* ── libultraship-internal keys (registered by the engine) ─────────── */
    { "gEnableGFXTrace",                            KeyType::Bool, 0, 1, false },
    { "gSinkEnabled",                               KeyType::Bool, 0, 1, false },
    { "gOpenWindows.SDLAddRemoveDeviceEventHandler",KeyType::Bool, 0, 1, false },
};

} // namespace

const SchemaEntry* PortConfigSchemaFind(const char* key) {
    for (const SchemaEntry& e : kSchema) {
        if (std::strcmp(e.key, key) == 0) return &e;
    }
    return nullptr;
}

int PortConfigValidate() {
    int clamped = 0;
    for (const SchemaEntry& e : kSchema) {
        if (!CVarExists(e.key)) continue;
        WarnTypeMismatch(e);
        switch (e.type) {
        case KeyType::Bool:
        case KeyType::Int: {
            const int32_t v = CVarGetInteger(e.key, (int32_t)e.lo);
            const int32_t c = std::clamp(v, (int32_t)e.lo, (int32_t)e.hi);
            if (c != v) {
                CVarSetInteger(e.key, c);
                port_log_ex(PORT_LOG_WARN, "config", "SSB64 Config: %s=%d out of range [%g..%g], clamped to %d\n",
                         e.key, v, e.lo, e.hi, c);
                clamped++;
            }
            break;
        }
        case KeyType::Float: {
            const float v = CVarGetFloat(e.key, (float)e.lo);
            const float c = std::clamp(v, (float)e.lo, (float)e.hi);
            if (c != v) {
                CVarSetFloat(e.key, c);
                port_log_ex(PORT_LOG_WARN, "config", "SSB64 Config: %s=%g out of range [%g..%g], clamped to %g\n",
                         e.key, v, e.lo, e.hi, c);
                clamped++;
            }
            break;
        }
        case KeyType::String:
            break;
        }
    }
    return clamped;
}

int PortConfigUnknownKeys() {
    auto cv = Ship::Context::GetInstance()->GetConsoleVariables();
    if (!cv) return 0;
    int unknown = 0;
    for (const std::string& name : cv->GetVariableNames()) {
        if (name.empty() || name[0] != 'g') continue;   /* only our g* namespace */
        if (PortConfigSchemaFind(name.c_str()) == nullptr) {
            port_log_ex(PORT_LOG_WARN, "config", "SSB64 Config: unknown key \"%s\" — not in schema "
                     "(typo or stale config entry?)\n", name.c_str());
            unknown++;
        }
    }
    return unknown;
}

void PortConfigStampVersion() {
    if (CVarGetInteger("gConfig.SchemaVersion", 0) != kConfigSchemaVersion) {
        /* Future migrations hook here: compare the stored version before
         * overwriting, transform renamed keys, then re-stamp. */
        CVarSetInteger("gConfig.SchemaVersion", kConfigSchemaVersion);
    }
}

std::vector<std::string> PortConfigGameplayKeys() {
    std::vector<std::string> out;
    for (const SchemaEntry& e : kSchema) {
        if (e.affects_gameplay) out.emplace_back(e.key);
    }
    return out;
}

} // namespace ssb64::config
