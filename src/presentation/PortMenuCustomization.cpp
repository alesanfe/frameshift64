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

    // Bundled (in-archive) post-process shaders get a friendly label
    // in the picker. Names not in this map render with the shader's
    // short name directly (the user-supplied taxonomy is good enough).
static const char* PostProcessShaderLabel(const std::string& name) {
    if (name == "scanlines")  return "Scanlines";
    if (name == "crt-lottes") return "CRT - Lottes";
    return nullptr;
}

    // Draws the compat badge after a picker entry. `Any` shaders render
    // at any resolution; `Native` shaders ratio against InputSize and
    // only look right when the source FB is locked to ~240p (Low
    // Resolution Mode in the Settings → Graphics menu). The badge text
    // itself is colored, license hover-text comes from the sidecar.
static void DrawPostProcessCompatBadge(const Fast::PostProcessShaderInfo& info) {
    const char* text = info.compat == Fast::PostProcessCompat::Any ? "[any res]" : "[needs 240p]";
    const ImVec4 color = info.compat == Fast::PostProcessCompat::Any
        ? SemanticColors::Success
        : SemanticColors::Warning;
    ImGui::SameLine();
    ImGui::TextColored(color, TR("%s"), text);
    if (!info.license.empty() && ImGui::IsItemHovered()) {
        ImGui::SetTooltip(TR("License: %s"), info.license.c_str());
    }
}

    // Selecting a `compat=native` shader while Low Resolution Mode is
    // disabled is the common UX trap — the shader compiles and runs but
    // produces a tiny render in the corner of the FB. We open this
    // pending-modal token from the picker so the modal can be drawn at
    // the top of the next frame outside any combo / popup context.
    static std::string s_lowResWarnShader;
    static bool s_lowResWarnNeedsOpen = false;

    // Render the post-process shader picker as a single combo widget
    // that opens onto a tree of bundled + user-installed shaders. Drives
    // the two LUS cvars LUS::Run reads each frame:
    //   gPostProcessEnabled (int, 0/1)
    //   gPostProcessShader  (string, short name as accepted by
    //                        Fast::LoadPostProcessShader)
    // The combo's preview text reflects the current cvar state, so a
    // fresh launch picks up the previously-selected shader without the
    // picker needing to know about state-restore.
    /* Case-insensitive substring match for the shader filter. */
static bool ShaderMatchesFilter(const std::string& filter, const std::string& s) {
    if (filter.empty()) {
        return true;
    }
    std::string hay = s;
    std::string needle = filter;
    std::transform(hay.begin(), hay.end(), hay.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    std::transform(needle.begin(), needle.end(), needle.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return hay.find(needle) != std::string::npos;
}

static void SelectPostProcessShader(const std::string& name) {
    if (name.empty()) {
        CVarSetInteger("gPostProcessEnabled", 0);
        return;
    }
    CVarSetString("gPostProcessShader", name.c_str());
    CVarSetInteger("gPostProcessEnabled", 1);

    // Trigger the Low Resolution Mode warn modal when the picked
    // shader expects a native-resolution source FB but the user's
    // current latched mode is "off". The shader still loads - we
    // never block selection - but the modal nudges them toward
    // enabling N64 mode + restart so the picture isn't a tiny
    // render in the corner of the window.
    const Fast::PostProcessShaderInfo info = Fast::GetPostProcessShaderInfo(name);
    if (info.compat == Fast::PostProcessCompat::Native &&
        CVarGetInteger("gLowResModePending", 0) == 0) {
        s_lowResWarnShader = name;
        s_lowResWarnNeedsOpen = true;
    }
}

    /* One expandable tree group of shader entries inside the picker combo:
 * TreeNodeEx + Selectable rows + compat badge per entry. */
static void DrawShaderTreeGroup(const char* label, const std::vector<std::string>& names,
                                bool userLabels, std::map<std::string, Fast::PostProcessShaderInfo>& infoCache,
                                bool enabled, const std::string& current) {
    if (names.empty()) {
        return;
    }
    ImGui::SetNextItemOpen(true, ImGuiCond_FirstUseEver);
    if (!ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth)) {
        return;
    }
    for (const std::string& name : names) {
        auto it = infoCache.find(name);
        if (it == infoCache.end()) {
            it = infoCache.emplace(name, Fast::GetPostProcessShaderInfo(name)).first;
        }
        const Fast::PostProcessShaderInfo& info = it->second;
        std::string displayLabel;
        if (userLabels) {
            displayLabel = info.label.empty() ? name : info.label;
        } else {
            const char* friendly = PostProcessShaderLabel(name);
            displayLabel = friendly ? friendly : name;
        }
        const bool active = enabled && current == name;
        if (ImGui::Selectable(displayLabel.c_str(), active)) {
            SelectPostProcessShader(name);
        }
        DrawPostProcessCompatBadge(info);
    }
    ImGui::TreePop();
}

void RenderPostProcessShaderPicker(WidgetInfo& /*widget*/) {
    const bool enabled = CVarGetInteger("gPostProcessEnabled", 0) != 0;
    const char* currentRaw = CVarGetString("gPostProcessShader", "");
    const std::string current = currentRaw ? currentRaw : std::string();

    std::string preview = TR("Off");
    if (enabled && !current.empty()) {
        if (const char* friendly = PostProcessShaderLabel(current)) {
            preview = friendly;
        } else {
            preview = current;
        }
    }

    ImGui::TextUnformatted(TR("Post-Process Shader"));
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (!ImGui::BeginCombo("##gPostProcessShaderPicker", preview.c_str())) {
        return;
    }

    // Filter input persists across frames inside the open popup
    // only - closing the combo clears it. ImGui draws the combo
    // content inside its own internal child window each frame, so
    // a static-local is the simplest persistence story without a
    // companion cvar.
    static char filterBuf[128] = "";
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##gPostProcessShaderFilter", "Filter...", filterBuf, sizeof(filterBuf));
    const std::string filter = filterBuf;

    // Per-frame compat-info cache. Filesystem stat + sidecar parse
    // happens here so each picker entry below can show a badge without
    // re-probing on every Selectable. The map's lifetime is the
    // visible combo body; rebuilt every frame the combo is open since
    // the user's shader set rarely changes mid-popup and the cost is
    // dwarfed by ImGui itself.
    std::map<std::string, Fast::PostProcessShaderInfo> infoCache;

    // "Off" sits at the top regardless of filter - it's not a shader
    // name, just the disable affordance, and hiding it behind the
    // filter would leave the user with no way to turn the effect off
    // once they've narrowed the list.
    if (ImGui::Selectable(TR("Off"), !enabled)) {
        SelectPostProcessShader(std::string());
    }
    ImGui::Separator();

    // Bundled group: pulled from the LUS API rather than hardcoded
    // here so adding a builtin shader to f3d.o2r exposes it in the
    // picker without a port-side change.
    const std::vector<std::string> bundled = Fast::ListBuiltinPostProcessShaders();
    std::vector<std::string> bundledFiltered;
    bundledFiltered.reserve(bundled.size());
    for (const std::string& name : bundled) {
        const char* label = PostProcessShaderLabel(name);
        if (ShaderMatchesFilter(filter, name) || (label && ShaderMatchesFilter(filter, label))) {
            bundledFiltered.push_back(name);
        }
    }
    DrawShaderTreeGroup("Bundled", bundledFiltered, false, infoCache, enabled, current);

    // User-installed shaders, one tree node per folder under the
    // per-user shaders dir. The "(loose)" bucket holds shaders that
    // live directly in the root.
    const std::vector<Fast::UserPostProcessShaderFolder> userFolders =
        Fast::ListUserPostProcessShaders();
    for (const auto& folder : userFolders) {
        std::vector<std::string> matching;
        matching.reserve(folder.shaderNames.size());
        for (const std::string& name : folder.shaderNames) {
            if (ShaderMatchesFilter(filter, name) || ShaderMatchesFilter(filter, folder.displayName)) {
                matching.push_back(name);
            }
        }
        if (matching.empty()) {
            continue;
        }
        DrawShaderTreeGroup(folder.displayName.c_str(), matching, true, infoCache, enabled, current);
    }

    if (bundledFiltered.empty() && userFolders.empty() && filter.empty()) {
        ImGui::TextDisabled("%s", TR("No user shaders installed."));
        ImGui::TextDisabled("%s", TR("Drop .glsl / .glslp files into:"));
        const std::string userDir = Ship::Context::GetPathRelativeToAppDirectory("shaders");
        ImGui::TextDisabled(TR("%s"), userDir.c_str());
    }

    ImGui::EndCombo();
}

    // Live status panel for the post-process chain. Reads
    // Fast::GetPostProcessRuntimeDiagnostics(), which the chain publishes
    // on every load / unload. Lets the user confirm at a glance:
    //   * which shader is actually active (matches the cvar)
    //   * which load path produced it (legacy .glsl / .glslp, or .slang)
    //   * how many passes compiled
    //   * the last error if a load failed
    // Pair the on-screen state with the matching SPDLOG_INFO lines in
    // ssb64.log to walk through the load pipeline — see
    // docs/crt_shader_testing_protocol.md.
void RenderPostProcessDiagnostics(WidgetInfo& /*widget*/) {
    const Fast::PostProcessRuntimeDiagnostics diag = Fast::GetPostProcessRuntimeDiagnostics();

    ImGui::TextDisabled("%s", TR("Shader Runtime Diagnostics"));
    if (diag.active) {
        ImGui::Text(TR("Active:  %s"), diag.name.c_str());
        ImGui::Text(TR("Flavor:  %s"), diag.flavor.c_str());
        ImGui::Text(TR("Passes:  %zu"), diag.passCount);
    } else if (!diag.lastError.empty()) {
        ImGui::TextColored(SemanticColors::Error, "%s", TR("Inactive — last error:"));
        ImGui::TextWrapped(TR("%s"), diag.lastError.c_str());
    } else {
        ImGui::TextDisabled("%s", TR("No shader loaded."));
    }

    // Reload button — flips the enabled cvar off then on to force the
    // interpreter to redo the load (which re-runs all of the
    // SPDLOG_INFO instrumentation in the load path). Helpful when
    // tweaking sidecars or replacing files on disk during testing.
    if (ImGui::Button(TR("Reload active shader"))) {
        const char* currentRaw = CVarGetString("gPostProcessShader", "");
        const std::string current = currentRaw ? currentRaw : std::string();
        if (!current.empty()) {
            CVarSetInteger("gPostProcessEnabled", 0);
            // The interpreter's transition detection keys on the (name,
            // enabled) pair, so we re-write both even though the name
            // is unchanged — that re-arms the load path.
            CVarSetString("gPostProcessShader", current.c_str());
            CVarSetInteger("gPostProcessEnabled", 1);
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("Force the runtime to re-resolve, re-load, and re-compile the\n"
            "currently-selected shader. Watch ssb64.log for the matching\n"
            "Post-process: ... INFO lines. Useful after editing a shader\n"
            "file on disk or its .lus.json sidecar."));
    }
}

    // Shader-pack downloader UI is gated off only on UWP (MSIX packages
    // can't fetch/replace content this way). On Android the downloader now
    // runs through HttpURLConnection + in-process libzip, and
    // src/application/update/downloads/ShaderDownloader.cpp is compiled into libmain.so.
    // Switch is off too: no download service exists there.
#if !defined(BATTLESHIP_UWP) && !defined(__SWITCH__)

    // State scoped to the shader-pack modal, mirroring the Low-Res-warn
    // pattern. We keep a "needs to open this frame" flag so the
    // RenderShaderPackModal hook (called every frame from
    // RenderMenuTopLevel) can issue the OpenPopup against the correct
    // ImGui ID — clicking the picker's "Get more shaders…" button only
    // sets the flag; OpenPopup must run on the same stack as the
    // matching BeginPopupModal.

    static bool s_shaderPackModalNeedsOpen = false;

    // User-side selection state. Vector<bool> packs the picks alongside
    // the candidate list returned by GetShaderPackCandidates(); we keep
    // the candidate copy here so toggles don't reach into the worker
    // thread's data, and so cancel-then-reopen produces a fresh list
    // instead of stale state.
    static std::vector<ssb64::enhancements::ShaderPackCandidate> s_shaderPackCandidatesUi;
    static std::vector<bool>                                     s_shaderPackSelected;
    // Last phase we saw on the previous frame — used to detect the
    // AwaitingSelection edge so we snapshot the candidate list once
    // (instead of once per frame, which would also reset the user's
    // checkbox state).
    static ssb64::enhancements::ShaderPackPhase s_shaderPackLastPhase =
    ssb64::enhancements::ShaderPackPhase::Idle;
    static char s_shaderPackFilter[128] = {};

    // Renders the "Get more shaders…" affordance immediately beneath the
    // picker. Click opens the modal; the modal handles the catalog fetch,
    // the user's selection, and the install in sequence.
    /* Online play: room-code lobby (src/networking/netplay/session/netlobby.cpp). The host picks the
 * rules and gets a 6-character code; the other player joins with it. */
void RenderShaderPackDownloader(WidgetInfo& /*widget*/) {
    using namespace ssb64::enhancements;

    const ShaderPackPhase phase = GetShaderPackPhase();
    const bool busy =
        phase == ShaderPackPhase::DownloadingCatalog ||
        phase == ShaderPackPhase::ExtractingCatalog ||
        phase == ShaderPackPhase::EnumeratingCatalog ||
        phase == ShaderPackPhase::AwaitingSelection ||
        phase == ShaderPackPhase::InstallingSelected;

    if (UIWidgets::Button(TR("Get more shaders..."),
            UIWidgets::ButtonOptions()
                .Color(UIWidgets::Colors::LightBlue)
                .Tooltip("Browse and install community CRT/scanline/NTSC shaders\n"
                         "from libretro/glsl-shaders. These shaders are GPL'd and\n"
                         "are installed by the user at runtime - the binary itself\n"
                         "stays MIT. Most expect Low Resolution Mode for correct\n"
                         "rendering scale."))) {
        SPDLOG_INFO("Shader pack: button clicked, current phase={} busy={}",
                    static_cast<int>(phase), busy);
        s_shaderPackModalNeedsOpen = true;
        if (!busy) {
            ssb64::enhancements::FetchShaderPackCatalogAsync();
        }
    }

    const std::string status = GetShaderPackStatus();
    if (!status.empty() && phase == ShaderPackPhase::Done) {
        // Persistent post-install summary directly under the button
        // (the modal closes on Install, so the user wouldn't see this
        // otherwise).
        ImGui::TextDisabled(TR("%s"), status.c_str());
    }
}

#if defined(PORT_HIRES_ENABLED) && !defined(BATTLESHIP_UWP) && !defined(__SWITCH__)
    // Featured texture packs (SSB Reloaded & co): one-click download into
    // mods/ with a pinned SHA-256, then enable + rescan so textures go live
    // without a restart. Android goes through the same path — the download
    // service uses HttpURLConnection there and FileHash is in-process.
    /* One row of the featured-packs list: name + summary + the action
 * appropriate to the current install phase. */
static void RenderCuratedPackRow(const ssb64::enhancements::CuratedModPack& pack,
                                 bool busy, ssb64::enhancements::ModPackInstallPhase phase,
                                 const std::string& status) {
    using namespace ssb64::enhancements;
    ImGui::PushID(pack.id);

    ImGui::TextWrapped(TR("%s %s"), pack.displayName, pack.version);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped(TR("%s"), pack.summary);
    ImGui::PopStyleColor();

    const bool installed = IsModPackInstalled(pack);
    if (installed) {
    ImGui::TextColored(SemanticColors::Success, "%s", TR("Installed"));
    ImGui::SameLine();
    if (UIWidgets::Button(TR("Reinstall"),
                          UIWidgets::ButtonOptions()
                              .Size(UIWidgets::Sizes::Inline)
                              .Tooltip("Re-download the pack (replaces the installed copy)."))) {
        StartModPackInstallAsync(pack.id);
    }
    } else if (busy) {
    const uint64_t got = GetModPackDownloadedBytes();
    const float frac = pack.expectedSize > 0
                           ? static_cast<float>(static_cast<double>(got) / pack.expectedSize)
                           : 0.0f;
    ImGui::ProgressBar(frac, ImVec2(-1.0f, 0.0f),
                       fmt::format(fmt::runtime(TR("{} ({:.0f} / {:.0f} MB)")), status,
                                   got / 1.0e6, pack.expectedSize / 1.0e6)
                           .c_str());
    if (UIWidgets::Button(TR("Cancel"),
                          UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline))) {
        CancelModPackInstall();
    }
    } else {
    const double mb = pack.expectedSize / 1.0e6;
    // Tooltip stores the pointer — build the text once so it outlives the call.
    static std::string s_packTooltip =
        fmt::format(fmt::runtime(TR("Downloads from {} and verifies the pinned SHA-256 before installing. The pack stays zipped in mods/ — no extraction needed.")), pack.upstreamUrl);
    if (UIWidgets::Button(
            fmt::format(fmt::runtime(TR("Download & Install ({:.0f} MB)")), mb).c_str(),
            UIWidgets::ButtonOptions()
                .Color(UIWidgets::Colors::LightBlue)
                .Tooltip(s_packTooltip.c_str()))) {
        StartModPackInstallAsync(pack.id);
    }
}

    if (!installed && !busy && phase == ModPackInstallPhase::Error && !status.empty()) {
    ImGui::TextColored(SemanticColors::Error, TR("%s"), status.c_str());
}
    ImGui::PopID();
}

    /* Generic installer: user-supplied URL or a file/folder dropped on the
 * window. The worker sniffs the payload - script mod (manifest.json),
 * native texture pack, or GLideN64/Rice pack (converted via the
 * bundled hash map). */
static void RenderGenericPackInstall(bool busy,
                                     ssb64::enhancements::ModPackInstallPhase phase,
                                     const std::string& status) {
    using namespace ssb64::enhancements;
    ImGui::Separator();
    ImGui::TextDisabled("%s", TR("Install your own"));
    ImGui::TextWrapped(TR("Paste a direct link to a texture pack or mod, or drag "
                   "a .zip / .o2r / .png / folder onto the window. "
                   "GLideN64/Rice packs (most SSB64 texture packs released "
                   "for emulators) are converted automatically%s."), IsGliden64ConverterReady() ? ""
                                              : " — NOT available in this build "
                                                "(missing bundled-data/gliden64-map.json)");

    static char s_packUrl[512] = "";
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##packurl", "https://... (direct .zip link)", s_packUrl,
                         sizeof(s_packUrl));
    if (busy) {
    const uint64_t got = GetModPackDownloadedBytes();
    ImGui::TextWrapped(TR("%s"), got > 0
                                 ? fmt::format(fmt::runtime(TR("{} ({:.0f} MB)")), status, got / 1.0e6).c_str()
                                 : status.c_str());
    if (UIWidgets::Button(TR("Cancel##generic"),
                          UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline))) {
        CancelModPackInstall();
    }
    } else {
    const bool hasUrl = s_packUrl[0] != '\0';
    if (UIWidgets::Button(TR("Download & Install##generic"),
            UIWidgets::ButtonOptions()
                .Color(UIWidgets::Colors::LightBlue)
                .Tooltip("The download runs in the background. The kind of "
                         "payload is detected automatically — texture "
                         "packs go live, mods need a Hot Reload."))) {
        if (hasUrl) {
            StartPackInstallFromUrlAsync(s_packUrl);
            s_packUrl[0] = '\0';
        }
    }
}
    if (!busy && phase == ModPackInstallPhase::Error && !status.empty()) {
    ImGui::TextColored(SemanticColors::Error, TR("%s"), status.c_str());
}
    // Register the SDL_DROPFILE consumer once the LUS context is up
    // (idempotent, cheap — call every frame).
    RegisterModPackDropHandler();
}


void RenderCuratedTexturePacks(WidgetInfo& /*widget*/) {
    using namespace ssb64::enhancements;

    const ModPackInstallPhase phase = GetModPackInstallPhase();
    const bool busy =
        phase == ModPackInstallPhase::Downloading ||
        phase == ModPackInstallPhase::Verifying ||
        phase == ModPackInstallPhase::Validating ||
        phase == ModPackInstallPhase::Converting ||
        phase == ModPackInstallPhase::Installing;

    // Enable + rescan on the Done edge so the freshly installed pack goes
    // live without a restart — only for texture packs; a generic install
    // that produced a script mod is activated via Hot Reload from the
    // Script Mods panel instead (its Done edge lives there).
    static ModPackInstallPhase s_prevPhase = ModPackInstallPhase::Idle;
    if (phase == ModPackInstallPhase::Done &&
        s_prevPhase != ModPackInstallPhase::Done) {
        if (GetLastInstalledPackKind() == ModPackInstalledKind::TexturePack) {
            CVarSetInteger("gHiResTextures.Enabled", 1);
            CVarSave();
            ssb64::hires::HiResPack::Get().Init();
            ToastSuccess(TR("Texture pack installed and enabled"));
        } else {
            ToastSuccess(TR("Pack installed"));
        }
    } else if (phase == ModPackInstallPhase::Error &&
               s_prevPhase != ModPackInstallPhase::Error) {
        ToastError(TR("Pack install failed"));
    }
    s_prevPhase = phase;

    size_t packCount = 0;
    const CuratedModPack* packs = GetCuratedModPacks(&packCount);
    const std::string status = GetModPackInstallStatus();

    for (size_t i = 0; i < packCount; i++) {
        const CuratedModPack& pack = packs[i];
        RenderCuratedPackRow(pack, busy, phase, status);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", TR("Third-party pack by GhostlyDark — downloaded from its own "
                       "GitHub release page (not redistributed by this project)."));
    ImGui::PopStyleColor();

    // --- Install your own --------------------------------------------------
    // Generic installer: user-supplied URL or a file/folder dropped on the
    // window. The worker sniffs the payload — script mod (manifest.json),
    // native texture pack, or GLideN64/Rice pack (converted via the
    // bundled hash map).
    RenderGenericPackInstall(busy, phase, status);
    // Register the SDL_DROPFILE consumer once the LUS context is up
    // (idempotent, cheap - call every frame).
    RegisterModPackDropHandler();
}
#endif // PORT_HIRES_ENABLED && desktop

    // Drawn from RenderMenuTopLevel each frame so the modal survives the
    // picker combo collapsing. Six conceptual states laid out in one
    // function because they share the same window+title+button layout
    // scaffold; switching them out frame-to-frame is the entire UX:
    //
    //   1. Loading (DownloadingCatalog / ExtractingCatalog /
    //      EnumeratingCatalog) — spinner + status string + Cancel.
    //   2. Awaiting selection — checkbox list + filter + Select All /
    //      None + Install + Cancel.
    //   3. Installing — spinner + status + (no Cancel; copy is fast).
    //   4. Done / Error — status line + OK.
static void CloseShaderPackModal() {
    using namespace ssb64::enhancements;
    CancelShaderPackFlow();
    s_shaderPackCandidatesUi.clear();
    s_shaderPackSelected.clear();
    s_shaderPackLastPhase = ShaderPackPhase::Idle;
    ImGui::CloseCurrentPopup();
}

    /* AwaitingSelection phase: filterable checklist + install action.
 * Extracted from RenderShaderPackModal to keep the modal switch
 * one-line-per-phase. */
static void RenderShaderPackSelection() {
    using namespace ssb64::enhancements;
    /* Same fixed action-row sizes as the modal switch. */
    const ImVec2 kSmallButton(120.0f, 0.0f);
    const ImVec2 kMediumButton(160.0f, 0.0f);
    ImGui::TextWrapped(TR("%zu shaders available. Tick the ones you'd like to install."), s_shaderPackCandidatesUi.size());
    ImGui::Spacing();

    if (UIWidgets::Button(TR("Select all"),
                          UIWidgets::ButtonOptions()
                              .Size(kMediumButton)
                              .Color(UIWidgets::Colors::Gray))) {
        s_shaderPackSelected.assign(s_shaderPackCandidatesUi.size(), true);
    }
    ImGui::SameLine();
    if (UIWidgets::Button(TR("Select none"),
                          UIWidgets::ButtonOptions()
                              .Size(kMediumButton)
                              .Color(UIWidgets::Colors::Gray))) {
        s_shaderPackSelected.assign(s_shaderPackCandidatesUi.size(), false);
    }
    ImGui::SameLine();
    // The InputString widget has its own padded layout; let it
    // claim the remaining row width so the placeholder doesn't
    // crowd the buttons.
    std::string filterValue = s_shaderPackFilter;
    ImGui::SetNextItemWidth(-1.0f);
    if (UIWidgets::InputString(
            "##shaderPackFilter", &filterValue,
            UIWidgets::InputOptions()
                .LabelPosition(UIWidgets::LabelPositions::None)
                .PlaceholderText("Filter by name..."))) {
        std::strncpy(s_shaderPackFilter, filterValue.c_str(),
                     sizeof(s_shaderPackFilter) - 1);
        s_shaderPackFilter[sizeof(s_shaderPackFilter) - 1] = '\0';
    }

    // Lower-case copy of the filter — case-insensitive substring
    // match is what users expect from a name filter.
    std::string filterLower = s_shaderPackFilter;
    std::transform(filterLower.begin(), filterLower.end(), filterLower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    // Reserve room above for the action row at the bottom so
    // the Install button doesn't shove the list off-screen on
    // the last frame. Themed checkboxes are taller than the
    // ImGui defaults; use the actual frame height + spacing
    // for the reservation.
    ImGui::BeginChild("##shaderPackList",
                      ImVec2(0.0f, -ImGui::GetFrameHeightWithSpacing() - 8.0f),
                      true);
    int visibleCount = 0;
    for (size_t i = 0; i < s_shaderPackCandidatesUi.size(); ++i) {
        const auto& cand = s_shaderPackCandidatesUi[i];
        if (!filterLower.empty()) {
            std::string labelLower = cand.displayLabel;
            std::transform(labelLower.begin(), labelLower.end(), labelLower.begin(),
                           [](unsigned char c) { return std::tolower(c); });
            if (labelLower.find(filterLower) == std::string::npos) {
                continue;
            }
        }
        ++visibleCount;
        bool checked = s_shaderPackSelected[i];
        if (UIWidgets::Checkbox(cand.displayLabel.c_str(), &checked,
                                UIWidgets::CheckboxOptions().Color(
                                    UIWidgets::Colors::LightBlue))) {
            s_shaderPackSelected[i] = checked;
        }
    }
    if (visibleCount == 0 && !filterLower.empty()) {
        ImGui::TextDisabled("%s", TR("No shaders match this filter."));
    }
    ImGui::EndChild();

    int totalChecked = 0;
    for (bool b : s_shaderPackSelected) {
        if (b) ++totalChecked;
    }

    const std::string installLabel =
        "Install " + std::to_string(totalChecked) + " shader" +
        (totalChecked == 1 ? "" : "s");
    const bool canInstall = totalChecked > 0;
    if (!canInstall) {
        ImGui::BeginDisabled();
    }
    if (UIWidgets::Button(installLabel.c_str(),
                          UIWidgets::ButtonOptions()
                              .Size(ImVec2(220.0f, 0.0f))
                              .Color(UIWidgets::Colors::Green))) {
        std::vector<std::string> picks;
        picks.reserve(static_cast<size_t>(totalChecked));
        for (size_t i = 0; i < s_shaderPackCandidatesUi.size(); ++i) {
            if (s_shaderPackSelected[i]) {
                picks.push_back(s_shaderPackCandidatesUi[i].stem);
            }
        }
        ssb64::enhancements::InstallSelectedShaderPackAsync(picks);
    }
    if (!canInstall) {
        ImGui::EndDisabled();
    }
    ImGui::SameLine();
    if (UIWidgets::Button(TR("Cancel"),
                          UIWidgets::ButtonOptions()
                              .Size(kSmallButton)
                              .Color(UIWidgets::Colors::Gray))) {
        CloseShaderPackModal();
    }
}


void RenderShaderPackModal() {
    using namespace ssb64::enhancements;

    if (s_shaderPackModalNeedsOpen) {
        SPDLOG_INFO("Shader pack: opening modal");
        ImGui::OpenPopup("Shader pack##downloader");
        s_shaderPackModalNeedsOpen = false;
    }

    const ShaderPackPhase phase = GetShaderPackPhase();

    // Detect the just-arrived AwaitingSelection edge — when we cross
    // it we snapshot the candidate list and reset the selection
    // bitmap to "all on" so the user's first interaction is to
    // de-select rather than to re-tick everything.
    if (phase == ShaderPackPhase::AwaitingSelection &&
        s_shaderPackLastPhase != ShaderPackPhase::AwaitingSelection) {
        s_shaderPackCandidatesUi = GetShaderPackCandidates();
        s_shaderPackSelected.assign(s_shaderPackCandidatesUi.size(), true);
        s_shaderPackFilter[0] = '\0';
    }
    s_shaderPackLastPhase = phase;

    // Always re-pin position + size on every open. NoSavedSettings
    // prevents imgui.ini from restoring stale geometry that could
    // place the modal off-screen or at zero size — a known cause of
    // "modal looks stuck because it's invisible". Matches the flags
    // used by the Re-extract modal lower in this file.
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 480.0f), ImGuiCond_Always);
    if (!ImGui::BeginPopupModal("Shader pack##downloader", nullptr,
                                ImGuiWindowFlags_NoSavedSettings)) {
        return;
    }

    const std::string status = GetShaderPackStatus();

    auto closeWithCancel = []() { CloseShaderPackModal(); };

    // Fixed sizes for the action-row buttons so the modal layout
     // doesn't reflow as their labels change between phases. The
     // theme's default Fill size would consume the entire content
     // region and we want the rest of the modal (text, list) to keep
     // its space.
    const ImVec2 kSmallButton(120.0f, 0.0f);
    const ImVec2 kMediumButton(160.0f, 0.0f);

    switch (phase) {
        case ShaderPackPhase::Idle:
        case ShaderPackPhase::Error: {
            // Fallback for the rare case where the modal opens before
            // the catalog phase has actually fired. Offer to retry
            // the fetch rather than leaving the user with an empty
            // window.
            ImGui::TextWrapped(TR("%s"), !status.empty()
                ? status.c_str()
                : "No catalog loaded. Try fetching again.");
            ImGui::Spacing();
            if (UIWidgets::Button(TR("Retry"),
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::LightBlue))) {
                ssb64::enhancements::FetchShaderPackCatalogAsync();
            }
            ImGui::SameLine();
            if (UIWidgets::Button(TR("Close"),
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::Gray))) {
                ImGui::CloseCurrentPopup();
            }
            break;
        }
        case ShaderPackPhase::DownloadingCatalog:
        case ShaderPackPhase::ExtractingCatalog:
        case ShaderPackPhase::EnumeratingCatalog: {
            ImGui::TextWrapped(TR("%s"), status.empty() ? "Working..." : status.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("%s", TR("Validating each shader against this build's\n"
                                "post-process pipeline. Takes a few seconds."));
            ImGui::Spacing();
            if (UIWidgets::Button(TR("Cancel"),
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::Gray))) {
                closeWithCancel();
            }
            break;
        }
        case ShaderPackPhase::AwaitingSelection: {
            RenderShaderPackSelection();
            break;
        }

        case ShaderPackPhase::InstallingSelected: {
            ImGui::TextWrapped(TR("%s"), status.empty() ? "Installing..." : status.c_str());
            ImGui::Spacing();
            ImGui::TextDisabled("%s", TR("Just copying files now - this should take less than a second."));
            break;
        }
        case ShaderPackPhase::Done: {
            ImGui::TextWrapped(TR("%s"), status.empty() ? "Done." : status.c_str());
            ImGui::Spacing();
            if (UIWidgets::Button(TR("OK"),
                                  UIWidgets::ButtonOptions()
                                      .Size(kSmallButton)
                                      .Color(UIWidgets::Colors::Green))) {
                s_shaderPackCandidatesUi.clear();
                s_shaderPackSelected.clear();
                s_shaderPackLastPhase = ShaderPackPhase::Idle;
                ImGui::CloseCurrentPopup();
            }
            break;
        }
    }

    ImGui::EndPopup();
}
#else
    // Switch keeps a no-op so the unconditional per-frame call in
    // RenderMenuTopLevel links without a platform guard there.
    void RenderShaderPackModal() {}
#endif // !BATTLESHIP_UWP && !__SWITCH__ (shader-pack downloader)

    // Modal companion to RenderPostProcessShaderPicker. Drawn from
    // RenderMenuTopLevel each frame so the popup survives the picker's
    // combo closing. The picker sets s_lowResWarnNeedsOpen when the user
    // selects a `compat=native` shader while Low Resolution Mode is off.
void RenderPostProcessLowResWarnModal() {
    if (s_lowResWarnNeedsOpen) {
        ImGui::OpenPopup("Low Resolution Mode##postprocess");
        s_lowResWarnNeedsOpen = false;
    }

    // Center the modal even though the picker may have triggered it
    // from a deep ImGui menu — the user shouldn't have to chase it
    // back to the picker's screen position.
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Low Resolution Mode##postprocess",
                                nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped("%s",
                       fmt::format(fmt::runtime(TR("\"{}\" was designed for the native 240p source resolution.")),
                                   s_lowResWarnShader)
                           .c_str());
    ImGui::Spacing();
    ImGui::TextWrapped("%s", TR("Low Resolution Mode is off, so the source framebuffer is the "
                       "full window size — the shader will render correctly, but the "
                       "picture will live in a small corner of the screen."));
    ImGui::Spacing();
    ImGui::TextWrapped("%s", TR("Enabling Low Resolution Mode forces the source FB to 320x240 "
                       "and requires a restart to take effect (it's latched at boot to "
                       "avoid mid-session Fast3D resize hazards)."));
    ImGui::Spacing();

    if (ImGui::Button(TR("Enable N64 mode (latched, restart required)"))) {
        CVarSetInteger("gLowResModePending", 1);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Keep current settings"))) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(TR("Disable post-process"))) {
        CVarSetInteger("gPostProcessEnabled", 0);
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

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
