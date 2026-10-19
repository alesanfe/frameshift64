// Script-mods panel + customization sidebar registration — extracted
// from PortMenuCustomization.cpp (that file keeps the shader/texture-pack
// pickers; this one owns the script-mod list UI and the three
// PortMenu::AddCustomization* entry points).

#include "PortMenu.h"
#include "PortMenuShared.h"

#include "Toasts.h"
#include "UIWidgets.hpp"
#include "Compat.h"
#include "app_paths.h"
#include "PortI18n.h"
#include "enhancements.h"
#ifdef PORT_HIRES_ENABLED
#include "HiResPack.h"
#endif
#ifndef DISABLE_SCRIPTING
#include "app_mods.h"
#include "schema.h"
#endif

#include <fast/backends/gfx_rendering_api.h>
#include <fast/postprocess/PostProcessSourceLoader.h>
#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include <imgui.h>

#include <SDL2/SDL.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>


namespace fs = std::filesystem;

namespace ssb64 {
    using namespace UIWidgets;

#ifndef DISABLE_SCRIPTING
    // Cached, read-only mod list for the Mods panel. The set of installed mods
    // only changes on a reload or restart, so we snapshot on demand (first view,
    // Rescan, and after a Hot Reload) rather than re-scanning every frame.
    static std::vector<ssb64::appmods::ModEntry> s_modList;
    static bool s_modListLoaded = false;

static void RefreshModList() {
    // Sync the mounted set to what's actually on disk so the list reflects mods
    // added or deleted at runtime: drop archives whose folder is gone, pick up
    // newly dropped ones. New mods appear as "not loaded" until a Hot Reload
    // compiles them; this only refreshes the inventory, it does not recompile.
    s_modList = ssb64::appmods::Snapshot();
    s_modListLoaded = true;
}

static void DoHotReload() {
    ssb64::appmods::HotReload();
    RefreshModList();
}

    /* Bundled example mods staged next to the binary by the build
 * (tools/developer/modding/package_mod.py -> <exe>/bundled-mods/<id>/). One-click
 * install copies the packaged folder into mods/ and hot-reloads so TCC
 * compiles + ModInit runs immediately — the mod lands already active. */
    struct BundledModSpec {
    const char* id;           // folder name under bundled-mods/ and mods/
    const char* manifestName; // manifest "name" as listed in s_modList
    const char* summary;
    };
    static const BundledModSpec kBundledMods[] = {
    { "playertint", "Player Tint",
      "P1 red / P2 blue / P3 green / P4 yellow fighter tint. "
      "Cosmetic-only — does not affect netplay compatibility." },
    };

static void RenderBundledModRows() {
    namespace fs = std::filesystem;
    const fs::path bundleRoot = fs::path(ssb64::RealAppBundlePath()) / "bundled-mods";
    // The writable mods/ root — same as the pack installer and the second
    // root MountModsDir scans (the bundle dir is read-only on packaged
    // builds; in portable mode this resolves back to next-to-exe).
    const fs::path modsRoot   = Ship::Context::GetPathRelativeToAppDirectory("mods");

    ImGui::TextDisabled("%s", TR("Included with BattleShip"));
    for (const BundledModSpec& spec : kBundledMods) {
        ImGui::PushID(spec.id);
        const fs::path src = bundleRoot / spec.id;
        const fs::path dst = modsRoot / spec.id;
        std::error_code ec;
        const bool bundled   = fs::is_directory(src, ec);
        const bool installed = fs::is_directory(dst, ec);

        bool loaded = false;
        for (const ssb64::appmods::ModEntry& m : s_modList) {
            if (m.name == spec.manifestName &&
                m.state == ssb64::appmods::ModStatus::Loaded) {
                loaded = true;
                break;
            }
        }

        ImGui::TextWrapped(TR("%s"), spec.manifestName);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped(TR("%s"), spec.summary);
        ImGui::PopStyleColor();
        if (loaded) {
            ImGui::TextColored(SemanticColors::Success, "%s", TR("Active"));
        } else if (installed) {
            ImGui::TextDisabled("%s", TR("Installed — not loaded"));
            ImGui::SameLine();
            if (UIWidgets::Button(TR("Activate"),
                                  UIWidgets::ButtonOptions()
                                      .Size(UIWidgets::Sizes::Inline)
                                      .Tooltip("Hot-reloads all mods so this one compiles and starts."))) {
                DoHotReload();
            }
        } else if (bundled) {
            if (UIWidgets::Button(TR("Install & Enable"),
                                  UIWidgets::ButtonOptions()
                                      .Color(UIWidgets::Colors::LightBlue)
                                      .Size(UIWidgets::Sizes::Inline)
                                      .Tooltip("Copies the bundled mod into mods/ and hot-reloads so it "
                                               "activates immediately."))) {
                fs::create_directories(modsRoot, ec);
                fs::copy(src, dst,
                         fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                         ec);
                if (ec) {
                    SPDLOG_ERROR("Bundled mod install failed: {} -> {}: {}",
                                 src.string(), dst.string(), ec.message());
                } else {
                    SPDLOG_INFO("Bundled mod installed: {} -> {}", src.string(), dst.string());
                    DoHotReload(); // mounts, compiles, loads + resnapshots s_modList
                }
            }
        } else {
            ImGui::TextDisabled("%s", TR("Not bundled in this build"));
        }
        ImGui::PopID();
    }
    ImGui::Separator();
}
    /* One row of the Mods panel: state chip, name, signature badge,
 * netplay-compat tag, author/description/note. */
    /* Status badge column — LOADED / not loaded / manifest issue. */
static void RenderModStatusBadge(const ssb64::appmods::ModEntry& mod) {
    using ssb64::appmods::ModStatus;
    switch (mod.state) {
    case ModStatus::Loaded:
        ImGui::TextColored(SemanticColors::Success, "%s", TR("LOADED"));
        break;
    case ModStatus::NotLoaded:
        ImGui::TextColored(SemanticColors::Muted, "%s", TR("not loaded"));
        break;
    case ModStatus::InvalidManifest:
        ImGui::TextColored(SemanticColors::Warning, "%s", TR("manifest issue"));
        break;
}
}

    /* Signature badge — [signed] / [signature invalid] (absent when unsigned). */
static void RenderModSignatureBadge(const ssb64::appmods::ModEntry& mod) {
    using ssb64::appmods::SignatureStatus;
    switch (mod.signature) {
    case SignatureStatus::Valid:
        ImGui::TextColored(SemanticColors::Success, "%s", TR("[signed]"));
        break;
    case SignatureStatus::Invalid:
        ImGui::TextColored(SemanticColors::Error, "%s", TR("[signature invalid]"));
        break;
    case SignatureStatus::None:
        break;
}
}


static void RenderModCard(const ssb64::appmods::ModEntry& mod) {
    using ssb64::appmods::ModStatus;
    ImGui::PushID(mod.archivePath.c_str()); // unique id per card

    RenderModStatusBadge(mod);

    ImGui::SameLine();
    if (!mod.version.empty()) {
        ImGui::Text(TR("%s  v%s"), mod.name.c_str(), mod.version.c_str());
    } else {
        ImGui::TextUnformatted(mod.name.c_str());
    }
    if (ImGui::IsItemHovered() && !mod.archivePath.empty()) {
        ImGui::SetTooltip(TR("%s"), mod.archivePath.c_str());
    }

    ImGui::SameLine();
    RenderModSignatureBadge(mod);

    // §27 — netplay compatibility tag from the manifest's
    // "simulation" flag (it feeds the session compat hash).
    if (!mod.simulation) {
        ImGui::SameLine();
        ImGui::TextColored(SemanticColors::Info, "%s", TR("[netplay-safe]"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("Cosmetic only — not part of the online compatibility check."));
        }
    } else {
        ImGui::SameLine();
        ImGui::TextColored(SemanticColors::Muted, "%s", TR("[gameplay]"));
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("Touches game simulation — every player in an online match "
                              "must run a compatible set or the lobby rejects the room."));
        }
    }

    if (!mod.author.empty()) {
        ImGui::TextDisabled(TR("by %s"), mod.author.c_str());
    }
    if (!mod.description.empty()) {
        ImGui::TextWrapped(TR("%s"), mod.description.c_str());
    }
    if (!mod.note.empty()) {
        ImGui::TextColored(SemanticColors::Warning, TR("%s"), mod.note.c_str());
    }

    ImGui::PopID();
    ImGui::Separator();
}

static void RenderModsPanel() {
    // --- Action bar -------------------------------------------------
    if (ImGui::Button(TR("Hot Reload"))) {
        DoHotReload(); // re-snapshots s_modList when it finishes
        ToastInfo(TR("Reloading script mods..."));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("Unload + recompile + re-init all TCC mods.\n"
                          "Adding or removing mod folders still needs an engine restart."));
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Rescan"))) {
        RefreshModList();
        ToastInfo(TR("Mod list refreshed"));
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("Re-read the mods/ folder without reloading the mods."));
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Open Mods Folder"))) {
        std::string modsPath = Ship::Context::GetPathRelativeToAppDirectory("mods");
        std::error_code ec;
        fs::create_directories(modsPath, ec);
        SDL_OpenURL(std::string("file:///" + fs::absolute(modsPath).string()).c_str());
    }

    // A generic install that produced a script mod lands active via
    // Hot Reload (mounts + compile + ModInit). The texture-pack case
    // is handled by its own Done edge in RenderCuratedTexturePacks.
    {
        using namespace ssb64::enhancements;
        const ModPackInstallPhase phase = GetModPackInstallPhase();
        static ModPackInstallPhase s_prevPhase = ModPackInstallPhase::Idle;
        if (phase == ModPackInstallPhase::Done &&
            s_prevPhase != ModPackInstallPhase::Done &&
            GetLastInstalledPackKind() == ModPackInstalledKind::ScriptMod) {
            DoHotReload(); // re-snapshots s_modList when it finishes
            ToastSuccess(TR("Mod installed — reloading"));
        } else if (phase == ModPackInstallPhase::Error &&
                   s_prevPhase != ModPackInstallPhase::Error) {
            ToastError(TR("Mod install failed"));
        }
        s_prevPhase = phase;
    }

    ImGui::Spacing();
    RenderBundledModRows();

    // Build the list once on first view; thereafter only on demand.
    if (!s_modListLoaded) {
        RefreshModList();
    }

    // --- Filter -----------------------------------------------------
    static char filter[64] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##modfilter", "Filter by name or author", filter,
                             sizeof(filter));

    ImGui::Separator();

    if (s_modList.empty()) {
        ImGui::TextWrapped("%s", TR("No mods found. Drop a mod folder or .o2r into the mods/ "
                           "folder next to the executable, then press Rescan (or "
                           "restart)."));
        return;
    }

    auto toLower = [](std::string s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    };
    const std::string needle = toLower(filter);
    auto matches = [&](const ssb64::appmods::ModEntry& mod) {
        return needle.empty() ||
               toLower(mod.name).find(needle) != std::string::npos ||
               toLower(mod.author).find(needle) != std::string::npos;
    };

    // --- Mod cards --------------------------------------------------
    int loadedCount = 0;
    int issueCount = 0;
    int shown = 0;
    for (const auto& mod : s_modList) {
        using ssb64::appmods::ModStatus;
        if (mod.state == ModStatus::Loaded) {
            loadedCount++;
        }
        if (mod.state == ModStatus::InvalidManifest) {
            issueCount++;
        }
        if (!matches(mod)) {
            continue;
        }
        shown++;
    RenderModCard(mod);
    }

    if (shown == 0) {
        ImGui::TextDisabled("%s", TR("No mods match the filter."));
    }

    ImGui::Spacing();
    std::string footer = std::to_string(static_cast<int>(s_modList.size())) +
                         " installed  |  " + std::to_string(loadedCount) + " loaded";
    if (issueCount > 0) {
        footer += "  |  " + std::to_string(issueCount) + " issue(s)";
    }
    ImGui::TextUnformatted(footer.c_str());
}

#endif // DISABLE_SCRIPTING (mod inventory + bundled mod rows)

    /* "Texture Packs" sidebar — hi-res pack toggle, featured/curated
 * installs and generic-pack import. Guarded by PORT_HIRES_ENABLED
 * at the call site. */
void PortMenu::AddCustomizationTexturePacks(WidgetPath& path) {
    path.sidebarName = "Texture Packs";
    AddSidebarEntry("Customization", "Texture Packs", 1);

    AddWidget(path, "Hi-Res Texture Pack", WIDGET_SEPARATOR_TEXT);
    AddWidget(path,
          "Drop a PNG pack into the mods/ folder. "
          "Files are matched by decoded-RGBA8 CRC32 — no per-PNG configuration needed. "
          "Toggle takes effect on the next texture cache miss.",
          WIDGET_TEXT);
#if !defined(BATTLESHIP_UWP) && !defined(__SWITCH__)
    // One-click installs for curated packs (SSB Reloaded). Downloads via
    // ModPackDownloader on a worker thread with pinned SHA-256 + zip audit —
    // Android goes through the JNI download service; UWP/Switch keep it out.
    AddWidget(path, "Featured packs", WIDGET_CUSTOM)
    .RaceDisable(false)
    .CustomFunction(RenderCuratedTexturePacks);
#endif
    AddWidget(path, "Enable Hi-Res Texture Pack", WIDGET_CVAR_CHECKBOX)
    .CVar("gHiResTextures.Enabled")
    .RaceDisable(false)
    .Options(CheckboxOptions().Tooltip("When on, every cache-miss texture upload hashes the decoded RGBA8 image "
                                        "and substitutes a matching PNG from the mods/ index at the pack's higher "
                                        "resolution.")
                 .DefaultValue(ssb64::hires::kHiResEnabledDefault != 0));
    AddWidget(path, "Preload Pack at Boot", WIDGET_CVAR_CHECKBOX)
    .CVar("gHiResTextures.Preload")
    .RaceDisable(false)
    .Options(CheckboxOptions().Tooltip("Decodes the pack into RAM on background threads at startup so first use "
                                        "of a texture doesn't stutter on a synchronous PNG decode. Warms up to "
                                        "the cache budget below. Takes effect on next launch.")
                 .DefaultValue(ssb64::hires::kHiResPreloadDefault != 0));
    AddWidget(path, "Decoded Cache Budget (MB)", WIDGET_CVAR_SLIDER_INT)
    .CVar("gHiResTextures.CacheBudgetMB")
    .RaceDisable(false)
    .Options(IntSliderOptions()
                 .Tooltip("RAM budget for decoded pack textures. A large HD pack whose working set "
                          "exceeds this re-decodes evicted textures on the render thread (stutter) — "
                          "raise it if you have RAM to spare. Takes effect on next launch.")
                 .Min(ssb64::hires::kMinLruBudgetMB)
                 .Max(4096)
                 .DefaultValue(ssb64::hires::kDefaultLruBudgetMB));
#if defined(__ANDROID__)
    AddWidget(path,
          "Mobile note: decoded textures are uncompressed in RAM and on the GPU. "
          "The pack is capped to a 128 MB cache (gHiResTextures.CacheBudgetMB) and "
          "per-texture upscale limit, but large packs can still pressure memory on "
          "low-RAM devices. Off by default — enable only if your device has headroom.",
          WIDGET_TEXT);
#endif
    AddWidget(path, "Open Mods Folder", WIDGET_BUTTON)
    .RaceDisable(false)
    .Callback([](WidgetInfo&) {
        std::string modsPath = Ship::Context::GetPathRelativeToAppDirectory("mods");
        std::error_code ec;
        fs::create_directories(modsPath, ec); // make it if it isn't there yet
        SDL_OpenURL(std::string("file:///" + fs::absolute(modsPath).string()).c_str());
    })
    .Options(ButtonOptions().Tooltip("Opens the mods/ folder in your file browser. Drop pack subfolders here."));
    AddWidget(path,
          fmt::format(fmt::runtime(TR("Indexed PNGs: {}")),
                       ssb64::hires::HiResPack::Get().Stats().indexedTextures),
          WIDGET_TEXT);

}


    /* Pack-authoring dump tooling (desktop only) — source/miss dumps
 * feeding the offline conversion pipeline. */
void PortMenu::AddCustomizationPackTools(WidgetPath& path) {
    // Pack-authoring dump tooling is desktop-only — it writes hundreds of
    // files into <app>/ and is driven from a separate offline conversion
    // pipeline, neither of which makes sense on a touch device.
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
    AddWidget(path, "Pack Authoring", WIDGET_SEPARATOR_TEXT);
    AddWidget(path,
          "Dump Source Textures writes one .bin per unique texture into "
          "<app>/hires_dump/. Used by the offline conversion tool to "
          "re-name a third-party Rice-CRC pack into our hash. Leave off "
          "in normal play.",
          WIDGET_TEXT);
    AddWidget(path, "Dump Source Textures", WIDGET_CVAR_CHECKBOX)
    .CVar("gHiResTextures.DumpSource")
    .RaceDisable(false)
    .Options(CheckboxOptions().Tooltip("On: every cache-miss with no pack hit dumps the source bytes + dimensions "
                                        "to <app>/hires_dump/. One file per unique texture per session.")
                 .DefaultValue(false));
    AddWidget(path, "Open Dump Folder", WIDGET_BUTTON)
    .RaceDisable(false)
    .Callback([](WidgetInfo&) {
        std::string dumpPath = Ship::Context::GetPathRelativeToAppDirectory("hires_dump");
        std::error_code ec;
        fs::create_directories(dumpPath, ec);
        SDL_OpenURL(std::string("file:///" + fs::absolute(dumpPath).string()).c_str());
    })
    .Options(ButtonOptions().Tooltip("Opens the hires_dump/ folder where source-texture dumps land."));

    AddWidget(path, "Miss Dump (native key)", WIDGET_SEPARATOR_TEXT);
    AddWidget(path,
          "On: every cache-miss with no pack hit writes a .bmp + .rgba into "
          "<app>/hires_miss_dump/, named with the port's native lookup key "
          "(miss#<hash>#<fmt>#<siz>_<w>x<h>). Rename the matching pack PNG to "
          "\"<anything>#<hash>#<fmt>#<siz>_all.png\" and drop it in mods/ to "
          "bind textures whose Rice CRC no dumper can reach (VS-records "
          "digits, tiny UI glyphs). Captures a full playthrough by default.",
          WIDGET_TEXT);
    AddWidget(path, "Dump Unmatched (native key)", WIDGET_CVAR_CHECKBOX)
    .CVar("gHiResTextures.DumpMissRgba")
    .RaceDisable(false)
    .Options(CheckboxOptions().Tooltip("On: each unique unmatched texture is written as a viewable .bmp plus the "
                                        "raw .rgba the hash was computed over. The filename is the exact "
                                        "<hash>#<fmt>#<siz> the pack scanner looks up.")
                 .DefaultValue(false));
    AddWidget(path, "Open Miss-Dump Folder", WIDGET_BUTTON)
    .RaceDisable(false)
    .Callback([](WidgetInfo&) {
        std::string missPath = Ship::Context::GetPathRelativeToAppDirectory("hires_miss_dump");
        std::error_code ec;
        fs::create_directories(missPath, ec);
        SDL_OpenURL(std::string("file:///" + fs::absolute(missPath).string()).c_str());
    })
    .Options(ButtonOptions().Tooltip("Opens the hires_miss_dump/ folder where native-key dumps land."));
#endif // !__ANDROID__ (pack-authoring dump tooling)
}


void PortMenu::AddMenuCustomization() {
    AddMenuEntry("Customization", CVAR_SETTING("Menu.CustomizationSidebarSection"));

    WidgetPath path = { "Customization", "Shaders", SECTION_COLUMN_1 };
    AddSidebarEntry("Customization", "Shaders", 1);
    AddWidget(path, "Shaders", WIDGET_SEPARATOR_TEXT);
#if !defined(BATTLESHIP_UWP)
    // Post-process / CRT shader stack — enabled on Android since the
    // downloader went in-process (HttpURLConnection + libzip). Note:
    // the transpile path emits GLSL ES through the same Fast3D GLES
    // backend, but has had limited device validation — treat shader
    // load failures on Android as bug reports, not unsupported config.
    AddWidget(path, "Post-Process Shader", WIDGET_CUSTOM)
        .RaceDisable(false)
        .CustomFunction(RenderPostProcessShaderPicker);

#if !defined(__SWITCH__)
    // Switch has no download service (no curl/JNI bridge) — the picker and
    // runtime diagnostics still apply to pre-installed shaders.
    AddWidget(path, "Shader Pack Downloader", WIDGET_CUSTOM)
        .RaceDisable(false)
        .CustomFunction(RenderShaderPackDownloader);
#endif

    AddWidget(path, "Shader Runtime Diagnostics", WIDGET_CUSTOM)
        .RaceDisable(false)
        .CustomFunction(RenderPostProcessDiagnostics);
#endif

#ifdef PORT_HIRES_ENABLED
    AddCustomizationTexturePacks(path);
    AddCustomizationPackTools(path);
#endif // PORT_HIRES_ENABLED

#ifndef DISABLE_SCRIPTING
    path.sidebarName = "Script Mods";
    AddSidebarEntry("Customization", "Script Mods", 1);
    AddWidget(path, "Mods Panel", WIDGET_CUSTOM)
        .CustomFunction([](WidgetInfo&) { RenderModsPanel(); });
#endif // DISABLE_SCRIPTING
}

} // namespace ssb64
