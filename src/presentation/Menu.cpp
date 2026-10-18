// SPDX-License-Identifier: MIT
//
// Portions of this file are derived from the Ship of Harkinian (SoH) project
//   Copyright (c) The Harbour Masters
//   https://github.com/HarbourMasters/Shipwright
// Licensed under the MIT License; see LICENSE at repository root.

#include "Menu.h"
#include "UIWidgets.hpp"
#include "Compat.h"
#include "app_paths.h"
#include "netlobby.h"
#include "narration.h"
#include "PortI18n.h"
#include "Toasts.h"
#include "PortMenuShared.h"
#include <ship/Context.h>
#include <ship/window/gui/ConsoleWindow.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>
#include <ship/window/gui/GuiMenuBar.h>
#include <ship/window/gui/GuiElement.h>
#include <ship/window/gui/Fonts.h>
#include <ship/window/gui/IconsFontAwesome4.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <filesystem>
#include <variant>
#include <SDL2/SDL.h>
#include <spdlog/fmt/fmt.h>
#include <tuple>
std::vector<ImVec2> windowTypeSizes = { {} };
std::vector<SearchWidget> extraSearchWidgets = {};

extern "C" int port_diag_safe_mode_active(void);

namespace Ship {
namespace {
/* §5 — pad hints follow the connected controller's family, since button
 * letters sit at different physical positions (FaceDown = accept is A on
 * Xbox, B on Nintendo pads, Cross on PlayStation). SDL's name for the first
 * recognized controller is enough — hot-plug changes apply next frame. */
enum class PadFamily { Xbox, PlayStation, Nintendo };
static PadFamily DetectPadFamily() {
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) {
            continue;
        }
        const char* raw = SDL_GameControllerNameForIndex(i);
        if (raw == nullptr) {
            continue;
        }
        std::string name = raw;
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (name.find("ps4") != std::string::npos || name.find("ps5") != std::string::npos ||
            name.find("dualshock") != std::string::npos || name.find("dualsense") != std::string::npos ||
            name.find("playstation") != std::string::npos) {
            return PadFamily::PlayStation;
        }
        if (name.find("switch") != std::string::npos || name.find("pro controller") != std::string::npos ||
            name.find("joy-con") != std::string::npos || name.find("joycon") != std::string::npos) {
            return PadFamily::Nintendo;
        }
        return PadFamily::Xbox;
    }
    return PadFamily::Xbox;
}

std::unordered_map<std::string, ImFont*> gMenuFonts;
bool gMenuFontsLoaded = false;

std::string FindMenuAssetPath(const std::string& relativePath) {
    // Use ssb64::RealAppBundlePath so AppImage / non-prefix-install layouts
    // resolve via /proc/self/exe instead of the literal CMAKE_INSTALL_PREFIX
    // that Ship::Context::GetAppBundlePath() returns under NON_PORTABLE.
    std::vector<std::filesystem::path> roots = {
        std::filesystem::path(ssb64::RealAppBundlePath()),
        std::filesystem::current_path(),
    };

    for (const auto& root : roots) {
        auto dir = root;
        while (!dir.empty()) {
            const auto candidate = dir / relativePath;
            if (std::filesystem::exists(candidate)) {
                return candidate.string();
            }
            const auto parent = dir.parent_path();
            if (parent.empty() || parent == dir) {
                break;
            }
            dir = parent;
        }
    }

    return {};
}

ImFont* AddMergedMenuFont(ImGuiIO& io, float size, const std::string& path = std::string()) {
    // HiDPI/Retina: bake the glyph atlas at device density so the menu text is
    // crisp instead of a bilinearly-upscaled 1x atlas. On a Retina display the
    // ImGui SDL2 backend sets DisplayFramebufferScale=2, which otherwise
    // stretches the 1x-baked glyphs and makes the menu look soft. Raising
    // RasterizerDensity increases only the rasterization resolution, not the
    // layout metrics, so on-screen sizes are unchanged. A fixed 2x is used on
    // macOS (the common backing scale): perfect 1:1 on a 2x display, and on a
    // 1x display the 2x atlas simply downsamples — still crisp, just a little
    // extra atlas memory. (A window DPI accessor in libultraship would let this
    // be derived dynamically; kept port-local here to avoid a submodule change.)
#ifdef __APPLE__
    constexpr float kMenuFontDensity = 2.0f;
#else
    constexpr float kMenuFontDensity = 1.0f;
#endif

    /* Default ranges cover Latin-1 only — UI copy using — ‘ ’ “ ” … •
     * (General Punctuation) rendered as '?'. Bake those in too; glyphs the
     * font doesn't have simply aren't added. */
    static const ImWchar kMenuGlyphRanges[] = {
        0x0020, 0x00FF, // Basic Latin + Latin-1 Supplement
        0x2010, 0x205F, // General Punctuation (dashes, quotes, ellipsis, bullets)
        0,
    };

    ImFont* font = nullptr;
    if (path.empty()) {
        ImFontConfig fontCfg;
        fontCfg.OversampleH = fontCfg.OversampleV = 1;
        fontCfg.PixelSnapH = true;
        fontCfg.SizePixels = size;
        fontCfg.RasterizerDensity = kMenuFontDensity;
        fontCfg.GlyphRanges = kMenuGlyphRanges;
        font = io.Fonts->AddFontDefault(&fontCfg);
    } else {
        ImFontConfig fontCfg;
        fontCfg.FontDataOwnedByAtlas = false;
        fontCfg.RasterizerDensity = kMenuFontDensity;
        fontCfg.GlyphRanges = kMenuGlyphRanges;
        font = io.Fonts->AddFontFromFileTTF(path.c_str(), size, &fontCfg);
    }

    const float iconFontSize = size * 2.0f / 3.0f;
    static const ImWchar sIconsRanges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig iconsConfig;
    iconsConfig.MergeMode = true;
    iconsConfig.PixelSnapH = true;
    iconsConfig.GlyphMinAdvanceX = iconFontSize;
    iconsConfig.RasterizerDensity = kMenuFontDensity;
    io.Fonts->AddFontFromMemoryCompressedBase85TTF(fontawesome_compressed_data_base85, iconFontSize, &iconsConfig,
                                                   sIconsRanges);
    return font;
}

void EnsureMenuFontsLoaded() {
    if (gMenuFontsLoaded) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    const auto standardFontPath = FindMenuAssetPath("assets/fonts/Montserrat-Regular.ttf");
    if (!standardFontPath.empty()) {
        gMenuFonts["MenuStandard"] = AddMergedMenuFont(io, 24.0f, standardFontPath);
        gMenuFonts["MenuStandardSmall"] = AddMergedMenuFont(io, 18.0f, standardFontPath);
    }

    gMenuFontsLoaded = true;
}

ImFont* GetMenuFont(const std::string& name) {
    if (gMenuFonts.contains(name)) {
        return gMenuFonts.at(name);
    }
    return nullptr;
}

/* Base menu themes (§8): a window-scoped palette pushed before Begin() each
 * frame — the accent color stays independent (Menu.Theme drives widget
 * highlights/NavHighlight), these only move the surfaces/text contrast.
 * Returns the number of colors pushed so every exit path pops exactly. */
enum class MenuBaseTheme { Dark = 0, Light = 1, HighContrast = 2 };

static int PushMenuBasePalette(MenuBaseTheme theme) {
    if (theme == MenuBaseTheme::Dark) {
        return 0; // stock ImGui dark palette is already the default
    }
    int n = 0;
    auto push = [&n](ImGuiCol col, const ImVec4& v) {
        ImGui::PushStyleColor(col, v);
        n++;
    };
    if (theme == MenuBaseTheme::Light) {
        push(ImGuiCol_Text, ImVec4(0.10f, 0.10f, 0.12f, 1.0f));
        push(ImGuiCol_ChildBg, ImVec4(0.93f, 0.93f, 0.96f, 0.35f));
        push(ImGuiCol_PopupBg, ImVec4(0.95f, 0.95f, 0.97f, 0.98f));
        push(ImGuiCol_Border, ImVec4(0.45f, 0.45f, 0.50f, 0.70f));
        push(ImGuiCol_FrameBg, ImVec4(0.86f, 0.86f, 0.90f, 1.0f));
        push(ImGuiCol_FrameBgHovered, ImVec4(0.80f, 0.80f, 0.87f, 1.0f));
        push(ImGuiCol_FrameBgActive, ImVec4(0.72f, 0.72f, 0.82f, 1.0f));
        push(ImGuiCol_TitleBgActive, ImVec4(0.82f, 0.82f, 0.88f, 1.0f));
        push(ImGuiCol_CheckMark, ImVec4(0.10f, 0.10f, 0.15f, 1.0f));
        push(ImGuiCol_SliderGrab, ImVec4(0.55f, 0.55f, 0.65f, 1.0f));
        push(ImGuiCol_SliderGrabActive, ImVec4(0.45f, 0.45f, 0.60f, 1.0f));
        push(ImGuiCol_Button, ImVec4(0.82f, 0.82f, 0.88f, 1.0f));
        push(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.74f, 0.84f, 1.0f));
        push(ImGuiCol_ButtonActive, ImVec4(0.66f, 0.66f, 0.80f, 1.0f));
        push(ImGuiCol_Header, ImVec4(0.78f, 0.78f, 0.86f, 0.80f));
        push(ImGuiCol_HeaderHovered, ImVec4(0.70f, 0.70f, 0.82f, 0.90f));
        push(ImGuiCol_HeaderActive, ImVec4(0.62f, 0.62f, 0.78f, 1.0f));
        push(ImGuiCol_Separator, ImVec4(0.45f, 0.45f, 0.50f, 0.80f));
        push(ImGuiCol_ScrollbarBg, ImVec4(0.85f, 0.85f, 0.90f, 0.50f));
        push(ImGuiCol_ScrollbarGrab, ImVec4(0.65f, 0.65f, 0.72f, 1.0f));
        push(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.55f, 0.55f, 0.65f, 1.0f));
        push(ImGuiCol_ScrollbarGrabActive, ImVec4(0.48f, 0.48f, 0.60f, 1.0f));
        return n;
    }
    // HighContrast — near-black surfaces, pure-white text, hard borders.
    push(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    push(ImGuiCol_ChildBg, ImVec4(0.02f, 0.02f, 0.02f, 0.60f));
    push(ImGuiCol_PopupBg, ImVec4(0.02f, 0.02f, 0.02f, 0.98f));
    push(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    push(ImGuiCol_FrameBg, ImVec4(0.08f, 0.08f, 0.08f, 1.0f));
    push(ImGuiCol_FrameBgHovered, ImVec4(0.20f, 0.20f, 0.20f, 1.0f));
    push(ImGuiCol_FrameBgActive, ImVec4(0.30f, 0.30f, 0.30f, 1.0f));
    push(ImGuiCol_TitleBgActive, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
    push(ImGuiCol_CheckMark, ImVec4(1.0f, 1.0f, 0.30f, 1.0f));
    push(ImGuiCol_SliderGrab, ImVec4(0.85f, 0.85f, 0.85f, 1.0f));
    push(ImGuiCol_SliderGrabActive, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    push(ImGuiCol_Button, ImVec4(0.10f, 0.10f, 0.10f, 1.0f));
    push(ImGuiCol_ButtonHovered, ImVec4(0.28f, 0.28f, 0.28f, 1.0f));
    push(ImGuiCol_ButtonActive, ImVec4(0.40f, 0.40f, 0.40f, 1.0f));
    push(ImGuiCol_Header, ImVec4(0.15f, 0.15f, 0.15f, 1.0f));
    push(ImGuiCol_HeaderHovered, ImVec4(0.32f, 0.32f, 0.32f, 1.0f));
    push(ImGuiCol_HeaderActive, ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
    push(ImGuiCol_Separator, ImVec4(1.0f, 1.0f, 1.0f, 0.90f));
    push(ImGuiCol_ScrollbarBg, ImVec4(0.05f, 0.05f, 0.05f, 0.80f));
    push(ImGuiCol_ScrollbarGrab, ImVec4(0.75f, 0.75f, 0.75f, 1.0f));
    push(ImGuiCol_ScrollbarGrabHovered, ImVec4(0.90f, 0.90f, 0.90f, 1.0f));
    push(ImGuiCol_ScrollbarGrabActive, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    return n;
}

static ImVec4 MenuWindowBgColor(MenuBaseTheme theme, float alpha) {
    if (theme == MenuBaseTheme::Light) {
        return ImVec4(0.97f, 0.97f, 0.98f, alpha);
    }
    if (theme == MenuBaseTheme::HighContrast) {
        return ImVec4(0.0f, 0.0f, 0.0f, (std::max)(alpha, 0.95f));
    }
    return ImVec4(0.0f, 0.0f, 0.0f, alpha);
}

static ImVec4 MenuTextDisabledColor(MenuBaseTheme theme) {
    if (theme == MenuBaseTheme::Light) {
        return ImVec4(0.40f, 0.40f, 0.45f, 1.0f);
    }
    if (theme == MenuBaseTheme::HighContrast) {
        return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
    }
    return ImVec4(0.80f, 0.80f, 0.80f, 1.0f);
}

/* Per-section "restore defaults" (U-01): walks every widget in a section's
 * columns. With apply=false it only counts CVar-bound widgets so the button
 * can hide on sections that have none; with apply=true it writes each
 * widget's declared defaultValue back through CVarSet* and re-runs its
 * ShipInit hook — the same path a manual edit takes. */

/* Undo slot for the last section restore — single-entry (a second restore
 * overwrites the first snapshot). Values are captured pre-reset through the
 * same CVarGet* types the reset writes with CVarSet*. */
struct SectionUndoEntry {
    std::string cvar;
    WidgetType type;
    int32_t i = 0;
    float f = 0.0f;
    std::string s;
    Color_RGBA8 c = { 255, 255, 255, 255 };
};
struct SectionUndo {
    std::string sidebar;
    std::vector<SectionUndoEntry> entries;
};
static SectionUndo sSectionUndo;

static void SnapshotSectionCvars(const SidebarEntry& section, SectionUndo& undo) {
    undo.entries.clear();
    for (const auto& column : section.columnWidgets) {
        for (const auto& widget : column) {
            if (widget.cVar == nullptr || *widget.cVar == '\0' || widget.options == nullptr) {
                continue;
            }
            SectionUndoEntry e;
            e.cvar = widget.cVar;
            e.type = widget.type;
            switch (widget.type) {
                case WIDGET_CVAR_CHECKBOX:
                case WIDGET_CVAR_COMBOBOX:
                case WIDGET_AUDIO_BACKEND:
                case WIDGET_VIDEO_BACKEND:
                case WIDGET_CVAR_SLIDER_INT:
                case WIDGET_CVAR_BTN_SELECTOR:
                    e.i = CVarGetInteger(widget.cVar, 0);
                    break;
                case WIDGET_CVAR_SLIDER_FLOAT:
                    e.f = CVarGetFloat(widget.cVar, 0.0f);
                    break;
                case WIDGET_CVAR_INPUT:
                    e.s = CVarGetString(widget.cVar, "");
                    break;
                case WIDGET_CVAR_COLOR_PICKER:
                    e.c = CVarGetColor(widget.cVar, e.c);
                    break;
                default:
                    continue;
            }
            undo.entries.push_back(std::move(e));
        }
    }
}

static void ApplySectionUndo(SectionUndo& undo) {
    for (const auto& e : undo.entries) {
        switch (e.type) {
            case WIDGET_CVAR_CHECKBOX:
            case WIDGET_CVAR_COMBOBOX:
            case WIDGET_AUDIO_BACKEND:
            case WIDGET_VIDEO_BACKEND:
            case WIDGET_CVAR_SLIDER_INT:
            case WIDGET_CVAR_BTN_SELECTOR:
                CVarSetInteger(e.cvar.c_str(), e.i);
                break;
            case WIDGET_CVAR_SLIDER_FLOAT:
                CVarSetFloat(e.cvar.c_str(), e.f);
                break;
            case WIDGET_CVAR_INPUT:
                CVarSetString(e.cvar.c_str(), e.s.c_str());
                break;
            case WIDGET_CVAR_COLOR_PICKER:
                CVarSetColor(e.cvar.c_str(), e.c);
                break;
            default:
                break;
        }
        ShipInit::Init(e.cvar.c_str());
    }
    undo.entries.clear();
}
static size_t ResetSectionCvars(const SidebarEntry& section, bool apply) {
    size_t touched = 0;
    for (const auto& column : section.columnWidgets) {
        for (const auto& widget : column) {
            if (widget.cVar == nullptr || *widget.cVar == '\0' || widget.options == nullptr) {
                continue;
            }
            const size_t before = touched;
            switch (widget.type) {
                case WIDGET_CVAR_CHECKBOX:
                    if (apply) {
                        CVarSetInteger(
                            widget.cVar,
                            std::static_pointer_cast<UIWidgets::CheckboxOptions>(widget.options)->defaultValue ? 1
                                                                                                             : 0);
                    }
                    touched++;
                    break;
                case WIDGET_CVAR_COMBOBOX:
                case WIDGET_AUDIO_BACKEND:
                case WIDGET_VIDEO_BACKEND:
                    if (apply) {
                        CVarSetInteger(widget.cVar,
                                       static_cast<int32_t>(
                                           std::static_pointer_cast<UIWidgets::ComboboxOptions>(widget.options)
                                               ->defaultIndex));
                    }
                    touched++;
                    break;
                case WIDGET_CVAR_SLIDER_INT:
                    if (apply) {
                        CVarSetInteger(
                            widget.cVar,
                            std::static_pointer_cast<UIWidgets::IntSliderOptions>(widget.options)->defaultValue);
                    }
                    touched++;
                    break;
                case WIDGET_CVAR_SLIDER_FLOAT:
                    if (apply) {
                        CVarSetFloat(
                            widget.cVar,
                            std::static_pointer_cast<UIWidgets::FloatSliderOptions>(widget.options)->defaultValue);
                    }
                    touched++;
                    break;
                case WIDGET_CVAR_BTN_SELECTOR:
                    if (apply) {
                        CVarSetInteger(
                            widget.cVar,
                            std::static_pointer_cast<UIWidgets::BtnSelectorOptions>(widget.options)->defaultValue);
                    }
                    touched++;
                    break;
                case WIDGET_CVAR_INPUT:
                    if (apply) {
                        CVarSetString(
                            widget.cVar,
                            std::static_pointer_cast<UIWidgets::InputOptions>(widget.options)->defaultValue.c_str());
                    }
                    touched++;
                    break;
                case WIDGET_CVAR_COLOR_PICKER:
                    if (apply) {
                        CVarSetColor(
                            widget.cVar,
                            std::static_pointer_cast<UIWidgets::ColorPickerOptions>(widget.options)->defaultValue);
                    }
                    touched++;
                    break;
                default:
                    break;
            }
            if (apply && touched != before) {
                ShipInit::Init(widget.cVar);
            }
        }
    }
    return touched;
}
} // namespace

std::string disabledTempTooltip;
const char* disabledTooltip;
bool disabledValue = false;

bool navigateToWidget = false;
std::string navigateMainEntry = "";
std::string navigateSidebar = "";
std::string navigateWidgetName = "";
bool highlightWidget = false;
static int sHighlightTtl = 0; /* frames left to find the navigation target */

bool operator==(Color_RGB8 const& l, Color_RGB8 const& r) noexcept {
    return l.r == r.r && l.g == r.g && l.b == r.b;
}

bool operator==(Color_RGBA8 const& l, Color_RGBA8 const& r) noexcept {
    return l.r == r.r && l.g == r.g && l.b == r.b && l.a == r.a;
}

bool operator<(Color_RGB8 const& l, Color_RGB8 const& r) noexcept {
    return (l.r < r.r && l.g <= r.g && l.b <= r.b) || (l.r <= r.r && l.g < r.g && l.b <= r.b) ||
           (l.r <= r.r && l.g <= r.g && l.b < r.b);
}

bool operator<(Color_RGBA8 const& l, Color_RGBA8 const& r) noexcept {
    return (l.r < r.r && l.g <= r.g && l.b <= r.b && l.a <= r.a) ||
           (l.r <= r.r && l.g < r.g && l.b <= r.b && l.a <= r.a) ||
           (l.r <= r.r && l.g <= r.g && l.b < r.b && l.a <= r.a) ||
           (l.r <= r.r && l.g <= r.g && l.b <= r.b && l.a < r.a);
}

bool operator>(Color_RGB8 const& l, Color_RGB8 const& r) noexcept {
    return (l.r > r.r && l.g >= r.g && l.b >= r.b) || (l.r >= r.r && l.g > r.g && l.b >= r.b) ||
           (l.r >= r.r && l.g >= r.g && l.b > r.b);
}

bool operator>(Color_RGBA8 const& l, Color_RGBA8 const& r) noexcept {
    return (l.r > r.r && l.g >= r.g && l.b >= r.b && l.a >= r.a) ||
           (l.r >= r.r && l.g > r.g && l.b >= r.b && l.a >= r.a) ||
           (l.r >= r.r && l.g >= r.g && l.b > r.b && l.a >= r.a) ||
           (l.r >= r.r && l.g >= r.g && l.b >= r.b && l.a > r.a);
}

uint32_t GetVectorIndexOf(std::vector<std::string>& vector, std::string value) {
    return (uint32_t)std::distance(vector.begin(), std::find(vector.begin(), vector.end(), value));
}

static bool raceDisableActive = false;

void Menu::InsertSidebarSearch() {
    menuEntries["Settings"].sidebars.emplace("Search", searchSidebarEntry);
    uint32_t curIndex = 0;
    if (!Ship_IsCStringEmpty(CVarGetString(menuEntries["Settings"].sidebarCvar, ""))) {
        curIndex = GetVectorIndexOf(menuEntries["Settings"].sidebarOrder,
                                    CVarGetString(menuEntries["Settings"].sidebarCvar, ""));
    }
    menuEntries["Settings"].sidebarOrder.insert(menuEntries["Settings"].sidebarOrder.begin() + searchSidebarIndex,
                                                "Search");
    /* The insert shifts every entry at searchSidebarIndex and beyond one slot
     * right, so a valid selection must be read back at curIndex + 1. A stale
     * CVar naming a nonexistent sidebar resolves to size() (pre-insert); the
     * bound check skips it instead of silently selecting the last entry. */
    if (curIndex > searchSidebarIndex &&
        curIndex + 1 < menuEntries["Settings"].sidebarOrder.size()) {
        CVarSetString(menuEntries["Settings"].sidebarCvar, menuEntries["Settings"].sidebarOrder.at(curIndex + 1).c_str());
    }
}

void Menu::RemoveSidebarSearch() {
    uint32_t curIndex = GetVectorIndexOf(menuEntries["Settings"].sidebarOrder,
                                         CVarGetString(menuEntries["Settings"].sidebarCvar, "General"));
    menuEntries["Settings"].sidebars.erase("Search");
    std::erase_if(menuEntries["Settings"].sidebarOrder, [](std::string& name) { return name == "Search"; });
    if (menuEntries["Settings"].sidebarOrder.empty()) {
        return;
    }
    if (curIndex > searchSidebarIndex) {
        curIndex--;
    }
    if (curIndex >= menuEntries["Settings"].sidebarOrder.size()) {
        curIndex = (uint32_t)menuEntries["Settings"].sidebarOrder.size() - 1;
    }
    CVarSetString(menuEntries["Settings"].sidebarCvar, menuEntries["Settings"].sidebarOrder.at(curIndex).c_str());
}

void Menu::UpdateWindowBackendObjects() {
    Ship::WindowBackend runningWindowBackend = Ship::Context::GetInstance()->GetWindow()->GetWindowBackend();
    int32_t configWindowBackendId = Ship::Context::GetInstance()->GetConfig()->GetInt("Window.Backend.Id", -1);
    if (Ship::Context::GetInstance()->GetWindow()->IsAvailableWindowBackend(configWindowBackendId)) {
        configWindowBackend = static_cast<Ship::WindowBackend>(configWindowBackendId);
    } else {
        configWindowBackend = runningWindowBackend;
    }

    availableWindowBackends = Ship::Context::GetInstance()->GetWindow()->GetAvailableWindowBackends();
    for (auto& backend : *availableWindowBackends) {
        availableWindowBackendsMap[backend] = windowBackendsMap.at(backend);
    }
}

bool Menu::IsMenuPopped() {
    return popped;
}

UIWidgets::Colors Menu::GetMenuThemeColor() {
    return menuThemeIndex;
}

Menu::Menu(const std::string& cVar, const std::string& name, uint8_t searchSidebarIndex_,
           UIWidgets::Colors defaultThemeIndex_)
    : GuiWindow(cVar, name), searchSidebarIndex(searchSidebarIndex_), defaultThemeIndex(defaultThemeIndex_) {
}

void Menu::InitElement() {
    EnsureMenuFontsLoaded();
    popped = CVarGetInteger(CVAR_SETTING("Menu.Popout"), 0);
    poppedSize.x = (float)CVarGetInteger(CVAR_SETTING("Menu.PoppedWidth"), 1280);
    poppedSize.y = (float)CVarGetInteger(CVAR_SETTING("Menu.PoppedHeight"), 800);
    poppedPos.x = (float)CVarGetInteger(CVAR_SETTING("Menu.PoppedPos.x"), 0);
    poppedPos.y = (float)CVarGetInteger(CVAR_SETTING("Menu.PoppedPos.y"), 0);
    menuThemeIndex = static_cast<UIWidgets::Colors>(CVarGetInteger(CVAR_SETTING("Menu.Theme"), defaultThemeIndex));

    UpdateWindowBackendObjects();
}

void Menu::UpdateElement() {
    menuThemeIndex = static_cast<UIWidgets::Colors>(CVarGetInteger(CVAR_SETTING("Menu.Theme"), defaultThemeIndex));
}

bool ModernMenuSidebarEntry(std::string label) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 pos = window->DC.CursorPos;
    const ImGuiID sidebarId = window->GetID(std::string(label + "##Sidebar").c_str());
    const char* tlabel = TR(label.c_str());
    ImVec2 labelSize = ImGui::CalcTextSize(tlabel, ImGui::FindRenderedTextEnd(tlabel), true);
    pos.y += style.FramePadding.y;
    pos.x = window->WorkRect.GetCenter().x - labelSize.x / 2;
    ImRect bb = { pos - style.FramePadding, pos + labelSize + style.FramePadding };
    ImGui::ItemSize(bb, style.FramePadding.y);
    ImGui::ItemAdd(bb, sidebarId);
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, sidebarId, &hovered, &held);
    if (pressed) {
        ImGui::MarkItemEdited(sidebarId);
    }
    window->DrawList->AddRectFilled(pos - style.FramePadding, pos + labelSize + style.FramePadding,
                                    ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive
                                                       : hovered         ? ImGuiCol_ButtonHovered
                                                                         : ImGuiCol_Button),
                                    3.0f);
    UIWidgets::RenderText(pos, tlabel, ImGui::FindRenderedTextEnd(tlabel), true);
    return pressed;
}

bool ModernMenuHeaderEntry(std::string label) {
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec2 pos = window->DC.CursorPos;
    const ImGuiID headerId = window->GetID(std::string(label + "##Header").c_str());
    const char* tlabel = TR(label.c_str());
    ImVec2 labelSize = ImGui::CalcTextSize(tlabel, ImGui::FindRenderedTextEnd(tlabel), true);
    ImRect bb = { pos, pos + labelSize + style.FramePadding * 2 };
    ImGui::ItemSize(bb, style.FramePadding.y);
    ImGui::ItemAdd(bb, headerId);
    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(bb, headerId, &hovered, &held);
    window->DrawList->AddRectFilled(bb.Min, bb.Max,
                                    ImGui::GetColorU32((held && hovered) ? ImGuiCol_ButtonActive
                                                       : hovered         ? ImGuiCol_ButtonHovered
                                                                         : ImGuiCol_Button),
                                    3.0f);
    pos += style.FramePadding;
    UIWidgets::RenderText(pos, tlabel, ImGui::FindRenderedTextEnd(tlabel), true);
    return pressed;
}

static bool SimpleMode();

uint32_t Menu::DrawSearchResults(std::string& menuSearchText) {
    int searchCount = 0;
    std::transform(menuSearchText.begin(), menuSearchText.end(), menuSearchText.begin(), ::tolower);
    menuSearchText.erase(std::remove(menuSearchText.begin(), menuSearchText.end(), ' '), menuSearchText.end());
    ImGui::SetNextWindowSizeConstraints({ ImGui::GetContentRegionAvail().x / 2, 0 },
                                        { ImGui::GetContentRegionAvail().x / 2, ImGui::GetContentRegionAvail().y });
    if (ImGui::BeginChild("Search Results Col 1", { ImGui::GetContentRegionAvail().x / 2, 0 },
                          ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoTitleBar)) {
        for (auto& menuLabel : menuOrder) {
            auto& menuEntry = menuEntries.at(menuLabel);
            for (auto& sidebarLabel : menuEntry.sidebarOrder) {
                auto& sidebar = menuEntry.sidebars[sidebarLabel];
                for (size_t i = 0; i < sidebar.columnWidgets.size(); i++) {
                    auto& column = sidebar.columnWidgets.at(i);
                    for (auto& info : column) {
                        if (info.type == WIDGET_SEARCH || info.type == WIDGET_SEPARATOR ||
                            info.type == WIDGET_SEPARATOR_TEXT || info.isHidden || info.hideInSearch) {
                            continue;
                        }
                        const char* tooltip = info.options->tooltip;
                        std::string widgetStr = std::string(info.name) + TR(info.name.c_str()) +
                            std::string(tooltip != NULL ? tooltip : "") +
                            (tooltip != NULL ? TR(tooltip) : "") + info.stableId;
                        for (const auto& term : info.searchTerms) {
                            widgetStr += term;
                        }
                        std::transform(widgetStr.begin(), widgetStr.end(), widgetStr.begin(), ::tolower);
                        widgetStr.erase(std::remove(widgetStr.begin(), widgetStr.end(), ' '), widgetStr.end());
                        if (widgetStr.find(menuSearchText) != std::string::npos) {
                            UIWidgets::ComponentAlignments backupAlignment{};
                            UIWidgets::LabelPositions backupLabelPos{};
                            if (info.type == WIDGET_COMBOBOX || info.type == WIDGET_CVAR_COMBOBOX) {
                                backupAlignment =
                                    std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options)->alignment;
                                backupLabelPos =
                                    std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options)->labelPosition;
                                std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options)->alignment =
                                    UIWidgets::ComponentAlignments::Left;
                                std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options)->labelPosition =
                                    UIWidgets::LabelPositions::Above;
                            }
                            MenuDrawItem(info, 400, menuThemeIndex);
                            ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(UIWidgets::Colors::Gray));
                            std::string origin =
                                fmt::format("  ({} -> {})", menuEntry.label, sidebarLabel);
                            if (info.advanced && SimpleMode()) {
                                origin += TR(" - hidden in Simple mode");
                            }
                            ImGui::Text(TR("%s"), origin.c_str());
                            ImGui::PopStyleColor();
                            ImGui::SameLine();
                            ImGui::PushID(info.stableId.empty() ? info.name.c_str() : info.stableId.c_str());
                            if (ImGui::SmallButton(TR("Go to"))) {
                                navigateToWidget = true;
                                navigateMainEntry = menuEntry.label;
                                navigateSidebar = sidebarLabel;
                                navigateWidgetName = info.name;
                            }
                            ImGui::PopID();
                            searchCount++;
                            if (info.type == WIDGET_COMBOBOX || info.type == WIDGET_CVAR_COMBOBOX) {
                                std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options)->alignment =
                                    backupAlignment;
                                std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options)->labelPosition =
                                    backupLabelPos;
                            }
                        }
                    }
                }
            }
        }
        for (auto& entry : extraSearchWidgets) {
            if (entry.info.type == WIDGET_SEARCH || entry.info.type == WIDGET_SEPARATOR ||
                entry.info.type == WIDGET_SEPARATOR_TEXT || entry.info.isHidden || entry.info.hideInSearch) {
                continue;
            }
            const char* extraTooltip = entry.info.options->tooltip;
            std::string widgetStr = entry.info.name + TR(entry.info.name.c_str()) +
                                    std::string(extraTooltip != NULL ? extraTooltip : "") +
                                    (extraTooltip != NULL ? TR(extraTooltip) : "") +
                                    entry.extraTerms + entry.sidebarName;
            std::transform(widgetStr.begin(), widgetStr.end(), widgetStr.begin(), ::tolower);
            widgetStr.erase(std::remove(widgetStr.begin(), widgetStr.end(), ' '), widgetStr.end());
            if (widgetStr.find(menuSearchText) != std::string::npos) {
                MenuDrawItem(entry.info, 400, menuThemeIndex);
                ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(UIWidgets::Colors::Gray));
                std::string origin = fmt::format("  ({} -> {}, {})", entry.menuName, entry.sidebarName, entry.location);
                if (entry.info.advanced && SimpleMode()) {
                    origin += TR(" - hidden in Simple mode");
                }
                ImGui::Text(TR("%s"), origin.c_str());
                ImGui::PopStyleColor();
                ImGui::SameLine();
                ImGui::PushID(entry.info.stableId.empty() ? entry.info.name.c_str() : entry.info.stableId.c_str());
                if (ImGui::SmallButton(TR("Go to"))) {
                    navigateToWidget = true;
                    navigateMainEntry = entry.menuName;
                    navigateSidebar = entry.sidebarName;
                    navigateWidgetName = entry.info.name;
                }
                ImGui::PopID();
                searchCount++;
            }
        }
        /* Action entries — the search box doubles as a command palette for
         * the few global actions players hunt for most. */
        struct SearchAction {
            const char* name;
            const char* terms;
            std::function<void()> run;
        };
        const SearchAction kSearchActions[] = {
            { "Close Menu", "close exit hide back game", [this]() { ToggleVisibility(); } },
            { "Restart Game", "reset reload reboot soft", []() {
                std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
                    Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"))
                    ->Dispatch("reset");
            } },
#if !defined(__ANDROID__) && !defined(BATTLESHIP_UWP)
            { "Restart in Safe Mode", "safe mode clean boot no mods crash", []() {
                ssb64::RequestSafeModeRestart();
            } },
            { "Quit BattleShip", "quit exit shutdown close app", []() {
                Ship::Context::GetInstance()->GetWindow()->Close();
            } },
#endif
        };
        for (const auto& action : kSearchActions) {
            std::string hay = std::string(action.name) + action.terms;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            hay.erase(std::remove(hay.begin(), hay.end(), ' '), hay.end());
            if (hay.find(menuSearchText) == std::string::npos) {
                continue;
            }
            ImGui::PushID(action.name);
            if (UIWidgets::Button(TR(action.name), UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline))) {
                action.run();
            }
            ImGui::PopID();
            ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(UIWidgets::Colors::Gray));
            ImGui::Text("%s", TR("  (action)"));
            ImGui::PopStyleColor();
            searchCount++;
        }
        ImGui::EndChild();
    }
    return searchCount;
}

void Menu::AddMenuEntry(std::string entryName, const char* entryCvar) {
    menuEntries.emplace(entryName, MainMenuEntry{ entryName, entryCvar });
    menuOrder.push_back(entryName);
}

void Menu::AddSearchWidget(SearchWidget widget) {
    extraSearchWidgets.push_back(widget);
}

void Menu::ClearAllEntries() {
    extraSearchWidgets.clear();
    menuEntries.clear();
    menuOrder.clear();
}

std::unordered_map<uint32_t, disabledInfo>& Menu::GetDisabledMap() {
    return disabledMap;
}

void Menu::MenuDrawItem(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex) {
    disabledTempTooltip = "This setting is disabled because: \n";
    disabledValue = false;
    disabledTooltip = " ";

    if (widget.preFunc != nullptr) {
        widget.ResetDisables();
        widget.preFunc(widget);
        if (widget.isHidden) {
            return;
        }
        if (!widget.activeDisables.empty()) {
            widget.options->disabled = true;
            for (auto option : widget.activeDisables) {
                disabledTempTooltip += std::string("\n- ") + disabledMap.at(option).reason;
            }
            widget.options->disabledTooltip = disabledTempTooltip.c_str();
        }
    }
    if (widget.raceDisable && raceDisableActive) {
        widget.options->disabled = true;
        disabledTempTooltip += std::string("\n- Race Lockout Active");
        widget.options->disabledTooltip = disabledTempTooltip.c_str();
    }

    if (widget.sameLine) {
        ImGui::SameLine();
    }

    try {
        switch (widget.type) {
            case WIDGET_CHECKBOX: {
                bool* pointer = std::get<bool*>(widget.valuePointer);
                if (pointer == nullptr) {
                    SPDLOG_ERROR("Checkbox Widget requires a value pointer, currently nullptr");
                    assert(false);
                    return;
                }
                auto options = std::static_pointer_cast<UIWidgets::CheckboxOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::Checkbox(UIWidgets::WrappedText(TR(widget.name.c_str()), width).c_str(), pointer,
                                        *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                }
            } break;
            case WIDGET_CVAR_CHECKBOX: {
                auto options = std::static_pointer_cast<UIWidgets::CheckboxOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarCheckbox(UIWidgets::WrappedText(TR(widget.name.c_str()), width).c_str(), widget.cVar,
                                            *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                };
            } break;
            case WIDGET_AUDIO_BACKEND: {
                auto currentAudioBackend = Ship::Context::GetInstance()->GetAudio()->GetCurrentAudioBackend();
                UIWidgets::ComboboxOptions options = {};
                options.color = themeIndex;
                options.tooltip = "Sets the audio API used by the game. Requires a relaunch to take effect.";
                options.disabled = Ship::Context::GetInstance()->GetAudio()->GetAvailableAudioBackends()->size() <= 1;
                options.disabledTooltip = "Only one audio API is available on this platform.";
                if (UIWidgets::Combobox("Audio API", &currentAudioBackend, audioBackendsMap, options)) {
                    Ship::Context::GetInstance()->GetAudio()->SetCurrentAudioBackend(currentAudioBackend);
                }
            } break;
            case WIDGET_VIDEO_BACKEND: {
                UIWidgets::ComboboxOptions options = {};
                options.color = themeIndex;
                options.tooltip = "Sets the renderer API used by the game.";
                options.disabled = availableWindowBackends->size() <= 1;
                options.disabledTooltip = "Only one renderer API is available on this platform.";
                if (UIWidgets::Combobox("Renderer API", &configWindowBackend, availableWindowBackendsMap,
                                        options)) {
                    Ship::Context::GetInstance()->GetConfig()->SetInt("Window.Backend.Id",
                                                                      (int32_t)(configWindowBackend));
                    Ship::Context::GetInstance()->GetConfig()->SetString("Window.Backend.Name",
                                                                         windowBackendsMap.at(configWindowBackend));
                    Ship::Context::GetInstance()->GetConfig()->Save();
                    UpdateWindowBackendObjects();
                }
            } break;
            case WIDGET_SEPARATOR: {
                ImGui::Separator();
            } break;
            case WIDGET_SEPARATOR_TEXT: {
                auto options = std::static_pointer_cast<UIWidgets::TextOptions>(widget.options);
                if (options->color != UIWidgets::Colors::NoColor) {
                    ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(options->color));
                }
                ImGui::SeparatorText(TR(widget.name.c_str()));
                if (options->color != UIWidgets::Colors::NoColor) {
                    ImGui::PopStyleColor();
                }
            } break;
            case WIDGET_TEXT: {
                auto options = std::static_pointer_cast<UIWidgets::TextOptions>(widget.options);
                if (options->color != UIWidgets::Colors::NoColor) {
                    ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(options->color));
                }
                ImGui::AlignTextToFramePadding();
                ImGui::TextWrapped(TR("%s"), TR(widget.name.c_str()));
                if (options->color != UIWidgets::Colors::NoColor) {
                    ImGui::PopStyleColor();
                }
            } break;
            case WIDGET_COMBOBOX: {
                int32_t* pointer = std::get<int32_t*>(widget.valuePointer);
                if (pointer == nullptr) {
                    SPDLOG_ERROR("Combobox Widget requires a value pointer, currently nullptr");
                    assert(false);
                    return;
                }
                auto options = std::static_pointer_cast<UIWidgets::ComboboxOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::Combobox(TR(widget.name.c_str()), pointer, options->comboMap, *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                };
            } break;
            case WIDGET_CVAR_COMBOBOX: {
                auto options = std::static_pointer_cast<UIWidgets::ComboboxOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarCombobox(TR(widget.name.c_str()), widget.cVar, options->comboMap, *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                }
            } break;
            case WIDGET_SLIDER_INT: {
                int32_t* pointer = std::get<int32_t*>(widget.valuePointer);
                if (pointer == nullptr) {
                    SPDLOG_ERROR("int32 Slider Widget requires a value pointer, currently nullptr");
                    assert(false);
                    return;
                }
                auto options = std::static_pointer_cast<UIWidgets::IntSliderOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::SliderInt(TR(widget.name.c_str()), pointer, *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                };
            } break;
            case WIDGET_CVAR_SLIDER_INT: {
                auto options = std::static_pointer_cast<UIWidgets::IntSliderOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarSliderInt(TR(widget.name.c_str()), widget.cVar, *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                };
            } break;
            case WIDGET_SLIDER_FLOAT: {
                float* pointer = std::get<float*>(widget.valuePointer);

                if (pointer == nullptr) {
                    SPDLOG_ERROR("float Slider Widget requires a value pointer, currently nullptr");
                    assert(false);
                    return;
                }
                auto options = std::static_pointer_cast<UIWidgets::FloatSliderOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::SliderFloat(TR(widget.name.c_str()), pointer, *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                }
            } break;
            case WIDGET_CVAR_SLIDER_FLOAT: {
                auto options = std::static_pointer_cast<UIWidgets::FloatSliderOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarSliderFloat(TR(widget.name.c_str()), widget.cVar, *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                }
            } break;
            case WIDGET_CVAR_BTN_SELECTOR: {
                auto options = std::static_pointer_cast<UIWidgets::BtnSelectorOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarBtnSelector(TR(widget.name.c_str()), widget.cVar, *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                }
            } break;
            case WIDGET_BUTTON: {
                auto options = std::static_pointer_cast<UIWidgets::ButtonOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::Button(TR(widget.name.c_str()), *options)) {
                    if (widget.callback != nullptr) {
                        widget.callback(widget);
                    }
                }
            } break;
            case WIDGET_CUSTOM: {
                if (widget.customFunction != nullptr) {
                    widget.customFunction(widget);
                }
            } break;
            case WIDGET_WINDOW_BUTTON: {
                if (widget.windowName == nullptr || widget.windowName[0] == '\0') {
                    std::string msg =
                        fmt::format(fmt::runtime(TR("Error drawing window contents for {}: windowName not defined")), widget.name);
                    SPDLOG_ERROR(msg.c_str());
                    break;
                }
                auto window = Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow(widget.windowName);
                if (!window) {
                    std::string msg =
                        fmt::format(fmt::runtime(TR("Error drawing window contents: windowName {} does not exist")), widget.windowName);
                    SPDLOG_ERROR(msg.c_str());
                    break;
                }
                auto options = std::static_pointer_cast<UIWidgets::WindowButtonOptions>(widget.options);
                options->color = themeIndex;
                if (options->showButton) {
                    UIWidgets::WindowButton(TR(widget.name.c_str()), widget.cVar, window, *options);
                }
                if (!window->IsVisible() && options->embedWindow) {
                    window->DrawElement();
                }
            } break;
            case WIDGET_CVAR_COLOR_PICKER: {
                auto options = std::static_pointer_cast<UIWidgets::ColorPickerOptions>(widget.options);
                uint32_t modifiers = 0;
                if (options->showLock)
                    modifiers |= UIWidgets::ColorPickerLockCheck;
                if (options->showRandom)
                    modifiers |= UIWidgets::ColorPickerRandomButton;
                if (options->showReset)
                    modifiers |= UIWidgets::ColorPickerResetButton;
                if (options->showRainbow)
                    modifiers |= UIWidgets::ColorPickerRainbowCheck;
                UIWidgets::CVarColorPicker(TR(widget.name.c_str()), widget.cVar, options->defaultValue, options->useAlpha,
                                           static_cast<uint8_t>(modifiers), options->color);
            } break;
            case WIDGET_SEARCH: {
                UIWidgets::PushStyleButton(themeIndex);
                if (ImGui::Button(TR("Clear"))) {
                    menuSearch.Clear();
                }
                ImGui::SameLine();
                if (CVarGetInteger(CVAR_SETTING("Menu.SearchAutofocus"), 0) &&
                    ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !ImGui::IsAnyItemActive() &&
                    !ImGui::IsMouseClicked(0)) {
                    ImGui::SetKeyboardFocusHere(0);
                }
                UIWidgets::PushStyleCombobox(themeIndex);
                ImGui::PushStyleColor(ImGuiCol_Border, UIWidgets::ColorValues.at(themeIndex));
                menuSearch.Draw();
                ImGui::PopStyleColor();
                UIWidgets::PopStyleCombobox();
                UIWidgets::PopStyleButton();
                std::string menuSearchText(menuSearch.InputBuf);

                if (menuSearchText == "") {
                    ImGui::Text(TR("%s"), TR("Start typing to see results."));
                    return;
                }
                DrawSearchResults(menuSearchText);
            } break;
            default:
                break;
        }
        /* Focus help capture — the widget just drawn is the "last item", so
         * reading hover/focus here maps to this control. The focused widget's
         * name + tooltip feed the persistent help bar at the bottom of the
         * menu (gamepad/touch users never see hover tooltips). */
        if (mShowHelpBar &&
            (ImGui::IsItemFocused() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))) {
            mFocusHelpName = widget.name;
            const char* tip = TR(widget.options->tooltip);
            if (widget.options->disabled && !Ship_IsCStringEmpty(widget.options->disabledTooltip)) {
                tip = widget.options->disabledTooltip;
            }
            mFocusHelpText = tip != nullptr ? tip : "";
        }
        /* §9 narration: announce the focused/hovered widget once per change,
         * independent of the help bar — Speak() purges pending utterances so
         * rapid moves only voice the latest. */
        if (ImGui::IsItemFocused() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            static std::string sLastNarrated;
            if (sLastNarrated != widget.name) {
                sLastNarrated = widget.name;
                port_narrate(TR(widget.name.c_str()));
            }
        }
        if (widget.tag != nullptr) {
            ImGui::SameLine();
            ImGui::TextDisabled(TR("[%s]"), widget.tag);
        }
        /* Recovery action for a disabled control: the widget-level action
         * wins, otherwise the first activeDisables reason that carries one. */
        if (widget.options->disabled) {
            const char* actionLabel = widget.disabledActionLabel;
            std::function<void()> action = widget.disabledAction;
            for (auto option : widget.activeDisables) {
                if (actionLabel == nullptr && disabledMap.at(option).actionLabel != nullptr) {
                    actionLabel = disabledMap.at(option).actionLabel;
                    action = disabledMap.at(option).action;
                }
            }
            if (actionLabel != nullptr && action != nullptr) {
                ImGui::SameLine();
                if (ImGui::SmallButton(actionLabel)) {
                    action();
                }
            }
        }
        /* Persistent-descriptions mode: print the tooltip inline under the
         * control instead of requiring a hover. */
        if (CVarGetInteger("gSettings.PersistentDescriptions", 0) &&
            !Ship_IsCStringEmpty(widget.options->tooltip)) {
            ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(UIWidgets::Colors::Gray));
            ImGui::TextWrapped(TR("%s"), TR(widget.options->tooltip));
            ImGui::PopStyleColor();
        }
        if (widget.postFunc != nullptr) {
            widget.postFunc(widget);
        }
    } catch (const std::bad_variant_access& e) {
        SPDLOG_ERROR("Failed to draw menu item \"{}\" due to: {}", widget.name, e.what());
        assert(false);
    }
}

void Menu::Draw() {
    if (!IsVisible()) {
        return;
    }
    DrawElement();
    // Sync up the IsVisible flag if it was changed by ImGui
    SyncVisibilityConsoleVariable();
}

static bool freshOpen = true;
/* §34 pending-changes: RequiresRestart widgets write their CVar at once but
 * only take effect on reset. Snapshot each such CVar on menu open and diff
 * per frame - the footer chip shows the pending count + a restart action. */
struct PendingRestart { int32_t baseline; std::string name; };
static std::unordered_map<std::string, PendingRestart> sRestartBaseline;
static int sPendingRestart = 0;

/* §34 progressive disclosure: Simple mode (gSettings.MenuMode=0) hides
 * advanced widgets and whole-advanced sidebars in normal browsing; search
 * still surfaces them so nothing becomes unreachable. */
static bool SimpleMode() {
    return CVarGetInteger("gSettings.MenuMode", 1) == 0;
}
static bool SidebarAdvancedOnly(const SidebarEntry& sb) {
    bool any = false;
    for (const auto& col : sb.columnWidgets) {
        for (const auto& w : col) {
            if (!w.advanced) {
                return false;
            }
            any = true;
        }
    }
    return any;
}
static bool SidebarHiddenInSimple(const SidebarEntry& sb) {
    return SimpleMode() && SidebarAdvancedOnly(sb);
}

/* SetScrollHere* can't resolve on the first frame: the child window's
 * ScrollMax isn't known until the frame ends, so the snap lands halfway.
 * Keep requesting the active entry's position for a few frames after open. */
static int scrollToActiveFrames = 0;
void Menu::DrawElement() {
    ImFont* standardLargest = GetMenuFont("MenuStandard");
    ImFont* standardSmall = GetMenuFont("MenuStandardSmall");
    if (standardLargest == nullptr) {
        standardLargest = ImGui::GetFont();
    }
    if (standardSmall == nullptr) {
        standardSmall = standardLargest;
    }
    for (auto& [reason, info] : disabledMap) {
        info.active = info.evaluation(info);
    }
    const char* headerCvar = CVAR_SETTING("Menu.ActiveHeader");

    if (navigateToWidget) {
        if (menuEntries.contains(navigateMainEntry) &&
            menuEntries.at(navigateMainEntry).sidebars.contains(navigateSidebar)) {
            menuSearch.Clear();
            CVarSetString(headerCvar, navigateMainEntry.c_str());
            const char* sidebarCvar = menuEntries.at(navigateMainEntry).sidebarCvar;
            CVarSetString(sidebarCvar, navigateSidebar.c_str());
            highlightWidget = true;
            sHighlightTtl = 90; /* ~1.5s at 60fps to find the widget */
        }
        navigateToWidget = false;
    }
    if (highlightWidget && --sHighlightTtl <= 0) {
        highlightWidget = false; /* target hidden or gone — stop looking */
    }

    raceDisableActive = CVarGetInteger(CVAR_SETTING("DisableChanges"), 0);

    /* UI scale — ScaleAllSizes only on change (it multiplies), FontGlobalScale
     * is absolute so it can be re-set every frame. Applied to the shared ImGui
     * context, so the scale also covers the floating tool windows. */
    float uiScale = CVarGetFloat("gSettings.UIScale", 1.0f);
    uiScale = std::clamp(uiScale, 0.75f, 2.5f);
    if (uiScale != mLastUiScale) {
        ImGui::GetStyle().ScaleAllSizes(uiScale / mLastUiScale);
        mLastUiScale = uiScale;
    }
    ImGui::GetIO().FontGlobalScale = uiScale;

    /* Reduced motion — this ImGui build has no widget animation system, so the
     * only perpetual motion in the menu is the text-input cursor blink. */
    ImGui::GetIO().ConfigInputTextCursorBlink = !CVarGetInteger("gSettings.ReducedMotion", 0);

    mFocusHelpName.clear();
    mFocusHelpText.clear();

    windowHeight = ImGui::GetMainViewport()->WorkSize.y;
    windowWidth = ImGui::GetMainViewport()->WorkSize.x;
    auto windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    bool popout = CVarGetInteger(CVAR_SETTING("Menu.Popout"), 0) && allowPopout;
    if (popout) {
        windowFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking;
    }
    if (popout != popped) {
        if (popout) {
            windowHeight = poppedSize.y;
            windowWidth = poppedSize.x;
            ImGui::SetNextWindowSize({ static_cast<float>(windowWidth), static_cast<float>(windowHeight) },
                                     ImGuiCond_Always);
            ImGui::SetNextWindowPos(poppedPos, ImGuiCond_Always);
        } else if (popped) {
            CVarSetFloat(CVAR_SETTING("Menu.PoppedWidth"), poppedSize.x);
            CVarSetFloat(CVAR_SETTING("Menu.PoppedHeight"), poppedSize.y);
            CVarSave();
        }
    }
    popped = popout;
    auto windowCond = ImGuiCond_Always;
    if (!popout) {
        ImGui::SetNextWindowSize({ static_cast<float>(windowWidth), static_cast<float>(windowHeight) }, windowCond);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), windowCond, { 0.5f, 0.5f });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    }

    /* §8 — base theme palette first (Dark is the stock palette: no pushes),
     * then the per-theme surface/focus colors on top. */
    const auto baseTheme = static_cast<MenuBaseTheme>(
        std::clamp(CVarGetInteger(CVAR_SETTING("Menu.BaseTheme"), 0), 0, 2));
    const int pushedColors = PushMenuBasePalette(baseTheme) + 3;

    ImGui::PushStyleColor(
        ImGuiCol_WindowBg,
        MenuWindowBgColor(baseTheme, CVarGetFloat(CVAR_SETTING("Menu.BackgroundOpacity"), 0.85f)));

    /* Stronger gamepad/keyboard focus ring than ImGui's stock 0.25-alpha
     * highlight — the focus must be findable at a glance. The accessibility
     * toggle pushes it to near-opaque. */
    ImVec4 navColor = UIWidgets::ColorValues.at(menuThemeIndex);
    navColor.w = (baseTheme == MenuBaseTheme::HighContrast ||
                  CVarGetInteger("gSettings.HighContrastFocus", 0))
                     ? 0.85f
                     : 0.45f;
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, navColor);

    /* Help/disabled text stays legible over translucent backdrops (A-03):
     * ImGui's default TextDisabled gray (0.50) sinks into bright game scenes
     * visible through a "Transparent" preset. Menu-scoped lift keeps the
     * secondary-text affordance while holding contrast on any backdrop —
     * theme-aware: dark gray for Light, near-white for High Contrast. */
    ImGui::PushStyleColor(ImGuiCol_TextDisabled, MenuTextDisabledColor(baseTheme));

    if (!ImGui::Begin("Main Menu", NULL, windowFlags)) {
        if (!popout) {
            ImGui::PopStyleVar();
        }
        freshOpen = true;
        ImGui::PopStyleColor(pushedColors);
        ImGui::End();
        return;
    }

    if (popped != popout) {
        if (!popout) {
            ImGui::PopStyleVar();
        }
        CVarSetInteger(CVAR_SETTING("Menu.Popout"), popped);
        CVarSetFloat(CVAR_SETTING("Menu.PoppedWidth"), poppedSize.x);
        CVarSetFloat(CVAR_SETTING("Menu.PoppedHeight"), poppedSize.y);
        CVarSetFloat(CVAR_SETTING("Menu.PoppedPos.x"), poppedSize.x);
        CVarSetFloat(CVAR_SETTING("Menu.PoppedPos.y"), poppedSize.y);
        CVarSave();
        ImGui::End();
        ImGui::PopStyleColor(pushedColors);
        return;
    }
    ImGuiContext& g = *GImGui;
    ImGuiWindow* window = g.CurrentWindow;
    ImGuiStyle& style = ImGui::GetStyle();
    windowHeight = window->WorkRect.GetHeight();
    windowWidth = window->WorkRect.GetWidth();

    ImGui::PushFont(standardLargest);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
    std::string headerIndex = CVarGetString(headerCvar, "Settings");
    ImVec2 pos = window->DC.CursorPos;
    float centerX = pos.x + windowWidth / 2 - (style.ItemSpacing.x * (menuEntries.size() + 1));
    std::vector<ImVec2> headerSizes;
    float headerWidth = 0.0f;
    bool headerSearch = !CVarGetInteger(CVAR_SETTING("Menu.SidebarSearch"), 0);
    for (auto& label : menuOrder) {
        ImVec2 size = ImGui::CalcTextSize(label.c_str());
        headerSizes.push_back(size);
        headerWidth += size.x + style.FramePadding.x * 2 + style.ItemSpacing.x;
    }

    // Full screen menu with widths below 1280, heights below 800.
    // 5% of screen width/height padding on both sides above those resolutions.
    // Menu width will never exceed a 16:9 aspect ratio.
    ImVec2 menuSize = { windowWidth, windowHeight };
    if (windowWidth > 1280) {
        menuSize.x = std::fminf(windowWidth * 0.9f, (windowHeight * 1.77f));
    }
    if (windowHeight > 800) {
        menuSize.y = windowHeight * 0.9f;
    }

    pos += window->WorkRect.GetSize() / 2 - menuSize / 2;
    ImGui::SetNextWindowPos(pos);
    const ImVec2 menuBlockPos = pos;
    ImGui::BeginChild("Menu Block", menuSize,
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

    std::unordered_map<std::string, SidebarEntry>* sidebar = nullptr;
    float headerHeight = headerSizes.at(0).y + style.FramePadding.y * 2;
    ImVec2 buttonSize = ImGui::CalcTextSize(ICON_FA_TIMES_CIRCLE) + style.FramePadding * 2;
    /* Reserve the pinned search field and trailing buttons: the search lives
     * outside the scrolling tab strip, so its width is a fixed affordance the
     * strip must leave room for — longer translated labels overflow into the
     * child's scrollbar instead of clipping the input. */
    const float searchW = headerSearch ? 200.0f : 0.0f;
    const float headerReserved = buttonSize.x * 3 + searchW + style.ItemSpacing.x * 4;
    bool scrollbar = false;
    if (headerWidth > menuSize.x - headerReserved) {
        headerHeight += style.ScrollbarSize;
        scrollbar = true;
    }
    ImVec2 headerSelSize = { menuSize.x - headerReserved, headerHeight };
    /* The child used to be constrained to the CONTENT width (headerWidth), so
     * on narrow windows AlwaysAutoResize grew it past the menu edge and the
     * Close/Reset/Quit buttons painted on top of the tab strip. Clamp it to
     * the space actually available — the HorizontalScrollbar flag inside
     * handles the overflow. */
    headerSelSize.x = ImMax(headerSelSize.x, 40.0f);
    ImGui::SetNextWindowSizeConstraints({ 0, headerHeight }, { headerSelSize.x, headerSelSize.y });
    if (scrollbar) {
        headerSelSize.y += style.ScrollbarSize;
    }
    bool autoFocus = CVarGetInteger(CVAR_SETTING("Menu.SearchAutofocus"), 0);
    /* No AutoResizeX: it resizes the frame to the CONTENT width in EndChild,
     * ignoring the constraint above, so tabs wider than the reserved space
     * would grow the child past the pinned search field and buttons. A fixed
     * width keeps overflow inside the child's horizontal scrollbar. */
    ImGui::BeginChild("Header Selection", headerSelSize,
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_HorizontalScrollbar);
    uint8_t curIndex = 0;
    for (auto& label : menuOrder) {
        if (curIndex != 0) {
            ImGui::SameLine();
        }
        auto& entry = menuEntries.at(label);
        std::string nextIndex = label;
        UIWidgets::PushStyleButton(menuThemeIndex);
        if (headerIndex != label) {
            ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
        }
        if (headerIndex == label && freshOpen) {
            // Keep the active tab visible when the strip is scrolled.
            ImGui::SetScrollHereX(0.5f);
        }
        if (ModernMenuHeaderEntry(entry.label)) {
            if (headerSearch) {
                menuSearch.Clear();
            }
            CVarSetString(headerCvar, label.c_str());
            CVarSave();
            nextIndex = label;
        }
        if (headerIndex != label) {
            ImGui::PopStyleColor();
        }
        UIWidgets::PopStyleButton();
        if (headerIndex == label) {
            sidebar = &entry.sidebars;
        }
        if (nextIndex != label) {
            headerIndex = nextIndex;
        }
        curIndex++;
    }
    ImGui::EndChild();
    /* Pinned search field — sits between the scrolling tab strip and the
     * trailing buttons so longer translated labels can never push it out
     * of view or under the last tab. */
    std::string menuSearchText = "";
    if (headerSearch) {
        ImGui::SameLine();
        if (autoFocus && freshOpen) {
            ImGui::SetKeyboardFocusHere();
        }
        auto color = UIWidgets::ColorValues.at(menuThemeIndex);
        color.w = 0.6f;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, color);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
        menuSearch.Draw("##search", 200.0f);
        menuSearchText = menuSearch.InputBuf;
        menuSearchText.erase(std::remove(menuSearchText.begin(), menuSearchText.end(), ' '), menuSearchText.end());
        if (menuSearchText.length() < 1) {
            /* Draw the placeholder straight into the draw list — a
             * cursor-seeking SameLine() here would disturb the absolute
             * positioning the trailing buttons rely on below. */
            const ImVec2 rmin = ImGui::GetItemRectMin();
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(rmin.x + style.FramePadding.x, rmin.y + style.FramePadding.y),
                ImGui::GetColorU32(UIWidgets::SemanticColors::Placeholder), TR("Search..."));
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    /* Trailing header actions. Quit/Reset are optional — subclasses that move
     * them into regular menu entries (e.g. a Support sidebar) keep only the
     * Close button here, so "leave the menu" and "quit the app" can't be
     * fat-fingered into each other. Android never shows Quit: mobile users
     * dismiss the app by swiping it away. */
    bool headerQuit = mShowHeaderQuit;
    bool headerReset = mShowHeaderReset;
#if defined(__ANDROID__)
    headerQuit = false;
#endif
    const int trailingButtons = 1 + (headerReset ? 1 : 0) + (headerQuit ? 1 : 0);
    ImGui::SameLine(menuSize.x - (buttonSize.x * trailingButtons) - (style.ItemSpacing.x * (trailingButtons - 1)));
    ImGui::PopStyleVar();
    if (headerQuit) {
        UIWidgets::ButtonOptions options3 = {};
        options3.color = UIWidgets::Colors::Red;
        options3.size = UIWidgets::Sizes::Inline;
        options3.tooltip = "Quit BattleShip";
        if (UIWidgets::Button(ICON_FA_POWER_OFF, options3)) {
            std::shared_ptr<Menu> menu =
                std::static_pointer_cast<Menu>(Ship::Context::GetInstance()->GetWindow()->GetGui()->GetMenu());
            if (!menu->IsMenuPopped()) {
                menu->ToggleVisibility();
            }
            Ship::Context::GetInstance()->GetWindow()->Close();
        }
        ImGui::SameLine();
    }
    if (headerReset) {
        UIWidgets::ButtonOptions options2 = {};
        options2.color = UIWidgets::Colors::Red;
        options2.size = UIWidgets::Sizes::Inline;
        options2.tooltip = "Restart game"
#ifdef __APPLE__
                           " (Command-R)"
#elif !defined(__SWITCH__) && !defined(__WIIU__)
                           " (Ctrl+R)"
#else
                           ""
#endif
            ;
        if (UIWidgets::Button(ICON_FA_UNDO, options2)) {
            std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
                Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"))
                ->Dispatch("reset");
        }
        ImGui::SameLine();
    }
    UIWidgets::ButtonOptions options = {};
    options.size = UIWidgets::Sizes::Inline;
    options.tooltip = "Close Menu (Esc)";
    if (UIWidgets::Button(ICON_FA_TIMES_CIRCLE, options)) {
        ToggleVisibility();

        // Update gamepad navigation after close based on if other menus are still visible
        auto mImGuiIo = &ImGui::GetIO();
        if (CVarGetInteger(CVAR_IMGUI_CONTROLLER_NAV, 0) &&
            Ship::Context::GetInstance()->GetWindow()->GetGui()->GetMenuOrMenubarVisible()) {
            mImGuiIo->ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
        } else {
            mImGuiIo->ConfigFlags &= ~(ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard);
        }
    }

    pos.y += headerHeight + style.ItemSpacing.y;
    pos.x = centerX - menuSize.x / 2 + (style.ItemSpacing.x * (menuEntries.size() + 1));
    window->DrawList->AddRectFilled(pos, pos + ImVec2{ menuSize.x, 4 }, ImGui::GetColorU32({ 255, 255, 255, 255 }),
                                    true, static_cast<ImDrawFlags>(style.WindowRounding));
    pos.y += style.ItemSpacing.y;
    float sectionHeight = menuSize.y - headerHeight - 4 - style.ItemSpacing.y * 2;
    float columnHeight = sectionHeight - style.ItemSpacing.y * 4;

    /* Compact layout: below ~560px of menu width a vertical 300px sidebar
     * leaves no room for content, so the section list collapses into a
     * horizontal scrolling strip above a full-width content area. */
    const bool compactLayout = menuSize.x < 560.0f;

    /* Footer reservation: the help strip + action-hint bar sit at the bottom
     * of the menu block, pinned by absolute cursor position later. Always
     * reserve when enabled so the layout doesn't jump as the help text
     * appears and disappears. In compact mode only a single hint line is
     * reserved — a 3-line help strip would eat half of a 240px window, and
     * persistent descriptions cover the same info inline. */
    const float footerLineH = ImGui::GetFontSize() + style.FramePadding.y * 2;
    float footerH = 0.0f;
    if (compactLayout) {
        if (mShowHelpBar || mShowActionBar) {
            footerH = footerLineH + style.ItemSpacing.y;
        }
    } else {
        if (mShowHelpBar) {
            footerH += footerLineH * 3 + style.ItemSpacing.y;
        }
        if (mShowActionBar) {
            footerH += footerLineH + style.ItemSpacing.y;
        }
    }
    columnHeight -= footerH;
    ImGui::SetNextWindowPos(pos + style.ItemSpacing * 2);

    // Increase sidebar width on larger screens to accommodate people scaling their menus.
    float sidebarWidth = 300 - style.ItemSpacing.x;
    if (menuSize.x > 1600) {
        sidebarWidth = menuSize.x * 0.15f;
    }

    const char* sidebarCvar = menuEntries.at(headerIndex).sidebarCvar;

    std::string sectionIndex = CVarGetString(sidebarCvar, "");
    if (!sidebar->contains(sectionIndex) || SidebarHiddenInSimple(sidebar->at(sectionIndex))) {
        sectionIndex = "";
        for (auto& l : menuEntries.at(headerIndex).sidebarOrder) {
            if (!SidebarHiddenInSimple(sidebar->at(l))) {
                sectionIndex = l;
                break;
            }
        }
        if (sectionIndex.empty()) {
            sectionIndex = menuEntries.at(headerIndex).sidebarOrder.at(0);
        }
    }
    const float sidebarStripH =
        compactLayout ? (headerSizes.at(0).y + style.FramePadding.y * 2 + style.ScrollbarSize) : 0.0f;
    if (compactLayout) {
        columnHeight -= sidebarStripH + style.ItemSpacing.y * 2;
    }
    columnHeight = ImMax(columnHeight, ImGui::GetFontSize() * 2.0f);

    float sectionCenterX = pos.x + (sidebarWidth / 2);
    float topY = pos.y;
    if (compactLayout) {
        ImGui::SetNextWindowPos(pos + style.ItemSpacing * 2);
        ImGui::BeginChild((menuEntries.at(headerIndex).label + " Section").c_str(),
                          { menuSize.x - style.ItemSpacing.x * 4, sidebarStripH }, ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_HorizontalScrollbar |
                              ImGuiWindowFlags_AlwaysHorizontalScrollbar);
        bool firstEntry = true;
        for (auto& sidebarLabel : menuEntries.at(headerIndex).sidebarOrder) {
            if (SidebarHiddenInSimple(sidebar->at(sidebarLabel))) {
                continue;
            }
            if (!firstEntry) {
                ImGui::SameLine();
            }
            firstEntry = false;
            std::string nextIndex = "";
            UIWidgets::PushStyleButton(menuThemeIndex);
            if (sectionIndex != sidebarLabel) {
                ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
            }
            /* Header-style entry: the sidebar variant centers itself on the
             * window width, which breaks in a horizontal strip. */
            if (ModernMenuHeaderEntry(sidebarLabel)) {
                if (headerSearch) {
                    menuSearch.Clear();
                }
                CVarSetString(sidebarCvar, sidebarLabel.c_str());
                CVarSave();
                nextIndex = sidebarLabel;
            }
            if (sectionIndex != sidebarLabel) {
                ImGui::PopStyleColor();
            }
            UIWidgets::PopStyleButton();
            /* Scroll the active section into view once it exists —
             * SetScrollHereX needs the item's real rect, so request it after
             * ItemAdd, and force max scroll for the last entry whose centered
             * target still clamps short of fully visible. */
            if (sectionIndex == sidebarLabel && scrollToActiveFrames > 0) {
                ImGui::SetScrollHereX(0.5f);
                if (&sidebarLabel == &menuEntries.at(headerIndex).sidebarOrder.back()) {
                    ImGui::SetScrollX(ImGui::GetScrollMaxX());
                }
            }
            if (nextIndex != "") {
                sectionIndex = nextIndex;
            }
        }
        ImGui::EndChild();
    } else {
        ImGui::SetNextWindowSizeConstraints({ sidebarWidth, 0 }, { sidebarWidth, columnHeight });
        ImGui::BeginChild((menuEntries.at(headerIndex).label + " Section").c_str(),
                          { sidebarWidth, columnHeight * 3 },
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        for (auto& sidebarLabel : menuEntries.at(headerIndex).sidebarOrder) {
            if (SidebarHiddenInSimple(sidebar->at(sidebarLabel))) {
                continue;
            }
            std::string nextIndex = "";
            UIWidgets::PushStyleButton(menuThemeIndex);
            if (sectionIndex != sidebarLabel) {
                ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
            }
            if (sectionIndex == sidebarLabel && scrollToActiveFrames > 0) {
                ImGui::SetScrollHereY(0.5f);
            }
            if (ModernMenuSidebarEntry(sidebarLabel)) {
                if (headerSearch) {
                    menuSearch.Clear();
                }
                CVarSetString(sidebarCvar, sidebarLabel.c_str());
                CVarSave();
                nextIndex = sidebarLabel;
            }
            if (sectionIndex != sidebarLabel) {
                ImGui::PopStyleColor();
            }
            UIWidgets::PopStyleButton();
            if (nextIndex != "") {
                sectionIndex = nextIndex;
            }
        }
        ImGui::EndChild();
    }
    ImGui::PopFont();

    if (standardSmall != nullptr) {
        ImGui::PushFont(standardSmall);
    }

    float sectionWidth;
    if (compactLayout) {
        pos = ImVec2{ menuBlockPos.x + style.ItemSpacing.x * 2,
                      topY + sidebarStripH + style.ItemSpacing.y * 3 };
        ImGui::SetNextWindowPos(pos);
        sectionWidth = menuSize.x - style.ItemSpacing.x * 4;
    } else {
        pos = ImVec2{ sectionCenterX + (sidebarWidth / 2), topY } + style.ItemSpacing * 2;
        window->DrawList->AddRectFilled(pos, pos + ImVec2{ 4, sectionHeight - style.FramePadding.y * 2 },
                                        ImGui::GetColorU32({ 255, 255, 255, 255 }), true, static_cast<ImDrawFlags>(style.WindowRounding));
        pos.x += 4 + style.ItemSpacing.x;
        ImGui::SetNextWindowPos(pos + style.ItemSpacing);
        sectionWidth = menuSize.x - sidebarWidth - 4 - style.ItemSpacing.x * 4;
    }
    std::string sectionMenuId = sectionIndex + " Settings";
    size_t columns = sidebar->at(sectionIndex).columnCount;
    size_t columnFuncs = sidebar->at(sectionIndex).columnWidgets.size();
    if (windowWidth < 800) {
        columns = 1;
    }
    float columnWidth = (sectionWidth - style.ItemSpacing.x * columns) / columns;
    bool useColumns = columns > 1;
    if (!useColumns || (headerSearch && menuSearchText.length() > 0)) {
        ImGui::SameLine();
        ImGui::SetNextWindowSizeConstraints({ sectionWidth, 0 }, { sectionWidth, columnHeight });
        ImGui::BeginChild(sectionMenuId.c_str(), { sectionWidth, windowHeight * 4 }, ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoTitleBar);
    }
    if (headerSearch && menuSearchText.length() > 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::Text("%s", TR("Search Results"));
        ImGui::SameLine();
        UIWidgets::ButtonOptions clearBtnOpts = {};
        clearBtnOpts.size = UIWidgets::Sizes::Inline;
        if (UIWidgets::Button(TR("Clear Search"), clearBtnOpts)) {
            menuSearch.Clear();
        }
        ImGui::BeginChild("searchSeparator", ImVec2(ImGui::GetContentRegionAvail().x / 2, 20),
                          ImGuiChildFlags_AlwaysAutoResize | ImGuiChildFlags_AutoResizeY);
        UIWidgets::Separator(true, true, 0, 10);
        ImGui::EndChild();
        uint32_t searchCount = DrawSearchResults(menuSearchText);
        if (searchCount == 0) {
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("No results found").x) / 2);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
            ImGui::TextColored(UIWidgets::SemanticColors::Placeholder, "%s", TR("No results found"));
        }
    } else {
        std::string menuLabel = menuEntries.at(headerIndex).label;
        if (MenuInit::GetUpdateFuncs().contains(menuLabel)) {
            if (MenuInit::GetUpdateFuncs()[menuLabel].contains(sectionIndex)) {
                for (auto& updateFunc : MenuInit::GetUpdateFuncs()[menuLabel][sectionIndex]) {
                    updateFunc();
                }
            }
        }
        for (size_t i = 0; i < columnFuncs; i++) {
            std::string sectionId = fmt::format(fmt::runtime(TR("{} Column {}")), sectionMenuId, i);
            if (useColumns) {
                ImGui::SetNextWindowSizeConstraints({ columnWidth, 0 }, { columnWidth, columnHeight });
                ImGui::BeginChild(sectionId.c_str(), { columnWidth, windowHeight * 4 }, ImGuiChildFlags_AutoResizeY,
                                  ImGuiWindowFlags_NoTitleBar);
            }
            // for (auto& entryName : sidebar->at(sectionIndex).sidebarOrder) {
            for (auto& entry : sidebar->at(sectionIndex).columnWidgets.at(i)) {
                if (entry.advanced && SimpleMode()) {
                    continue;
                }
                const bool isNavTarget = highlightWidget && entry.name == navigateWidgetName;
                if (isNavTarget) {
                    ImGui::SetScrollHereY(0.35f);
                }
                MenuDrawItem(entry, 90 / sidebar->at(sectionIndex).columnCount, menuThemeIndex);
                if (isNavTarget) {
                    /* Accent outline so the jumped-to widget is findable at
                     * a glance (GetItemRect covers whatever the item drew). */
                    const ImVec2 mn = ImGui::GetItemRectMin();
                    const ImVec2 mx = ImGui::GetItemRectMax();
                    ImGui::GetWindowDrawList()->AddRect(
                        ImVec2(mn.x - 4.0f, mn.y - 4.0f), ImVec2(mx.x + 4.0f, mx.y + 4.0f),
                        IM_COL32(102, 155, 245, 255), 4.0f, 0, 2.0f);
                    highlightWidget = false;
                    sHighlightTtl = 0;
                    navigateWidgetName.clear();
                }
            }
            //}
            if (useColumns) {
                ImGui::EndChild();
            }
            if (i < columns - 1) {
                ImGui::SameLine();
            }
        }
        if (ResetSectionCvars(sidebar->at(sectionIndex), false) > 0) {
            ImGui::Spacing();
            if (UIWidgets::Button(TR("Restore section defaults"),
                    UIWidgets::ButtonOptions()
                        .Tooltip("Reset every option in this section to its default value. "
                                 "Undo is available until you restore another section.")
                        .Size(UIWidgets::Sizes::Inline))) {
                SnapshotSectionCvars(sidebar->at(sectionIndex), sSectionUndo);
                sSectionUndo.sidebar = sectionIndex;
                const size_t restored = ResetSectionCvars(sidebar->at(sectionIndex), true);
                Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                ssb64::ToastInfo(
                    fmt::format(fmt::runtime(TR("Restored {} option{} to defaults")), restored, restored == 1 ? "" : "s"));
            }
            if (!sSectionUndo.entries.empty() && sSectionUndo.sidebar == sectionIndex) {
                ImGui::SameLine();
                if (UIWidgets::Button(TR("Undo restore"),
                        UIWidgets::ButtonOptions()
                            .Tooltip("Bring back the values this section had before the last restore.")
                            .Size(UIWidgets::Sizes::Inline))) {
                    ApplySectionUndo(sSectionUndo);
                    Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
                    ssb64::ToastInfo(TR("Section restore undone"));
                }
            }
        }
    }
    // Mirror the open-guard at line ~954. Earlier versions of this code
    // dropped the headerSearch qualifier here, which calls EndChild without
    // a matching BeginChild when useColumns=true && headerSearch=false &&
    // search-active — fires when the user enables "Search In Sidebar" then
    // types into a 2-column section's sidebar search.
    if (!useColumns || (headerSearch && menuSearchText.length() > 0)) {
        ImGui::EndChild();
    }

    /* §34: diff RequiresRestart CVars against the menu-open snapshot -
     * reverting a widget back to its baseline drops it from the count. */
    sPendingRestart = 0;
    for (const auto& [cvar, pe] : sRestartBaseline) {
        if (CVarGetInteger(cvar.c_str(), pe.baseline) != pe.baseline) {
            sPendingRestart++;
        }
    }

    /* Footer: pinned to the bottom of the menu block regardless of how tall
     * the section content ended up. Help strip first, action hints last. */
    if (mShowHelpBar || mShowActionBar) {
        // Opaque strip so the footer reads as its own bar instead of floating
        // text that can collide with sidebar rows on small screens.
        window->DrawList->AddRectFilled(
            ImVec2(menuBlockPos.x, menuBlockPos.y + menuSize.y - footerH),
            ImVec2(menuBlockPos.x + menuSize.x, menuBlockPos.y + menuSize.y),
            ImGui::GetColorU32(ImGuiCol_WindowBg), style.WindowRounding);
        ImGui::SetCursorScreenPos(
            ImVec2(menuBlockPos.x + style.ItemSpacing.x * 2, menuBlockPos.y + menuSize.y - footerH));
        if (mShowHelpBar && !mFocusHelpName.empty() && !compactLayout) {
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(menuThemeIndex));
            ImGui::Text(TR("%s"), TR(mFocusHelpName.c_str()));
            ImGui::PopStyleColor();
            if (!mFocusHelpText.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(UIWidgets::Colors::LightGray));
                ImGui::TextWrapped(TR("%s"), UIWidgets::WrappedText(TR(mFocusHelpText.c_str()), 120).c_str());
                ImGui::PopStyleColor();
            }
        }
        if (mShowActionBar) {
            const bool padNav = CVarGetInteger(CVAR_IMGUI_CONTROLLER_NAV, 0) != 0;
            const char* hints;
            if (padNav) {
                switch (DetectPadFamily()) {
                case PadFamily::PlayStation:
                    hints = compactLayout ? TR("[Cross] Select   [Circle] Back   [Square] Search   [Esc] Close")
                                          : TR("[Cross] Select    [Circle] Back    [Square] Search    [Esc] Close menu");
                    break;
                case PadFamily::Nintendo:
                    hints = compactLayout ? TR("[B] Select   [A] Back   [Y] Search   [Esc] Close")
                                          : TR("[B] Select    [A] Back    [Y] Search    [Esc] Close menu");
                    break;
                default:
                    hints = compactLayout ? TR("[A] Select   [B] Back   [X] Search   [Esc] Close")
                                          : TR("[A] Select    [B] Back    [X] Search    [Esc] Close menu");
                    break;
                }
            } else {
                hints = compactLayout ? TR("Enter Select   Esc Close menu")
                                      : TR("Enter Select    Esc Close menu    Type in Search box to find settings");
            }
            if (!compactLayout) {
                ImGui::Separator();
            }
            const float hintLineY = ImGui::GetCursorScreenPos().y;
            /* Online-match context: the menu never pauses netplay, so make
             * that explicit instead of letting the player assume otherwise.
             * Skipped in compact mode — a single footer line can't hold both
             * texts; the Online section shows its own in-match banner. */
            if (!compactLayout && port_netlobby_state() == PORT_NETLOBBY_IN_MATCH) {
                ImGui::SetCursorScreenPos(ImVec2(menuBlockPos.x + style.ItemSpacing.x * 2, hintLineY));
                ImGui::TextColored(UIWidgets::SemanticColors::Warning, "%s", TR("Match in progress - the game continues while this menu is open"));
            }
            /* §34 pending-changes chip: left-aligned, after the match banner
             * when both are up (compact mode keeps the single hint line). */
            if (!compactLayout && sPendingRestart > 0) {
                float chipX = menuBlockPos.x + style.ItemSpacing.x * 2;
                if (port_netlobby_state() == PORT_NETLOBBY_IN_MATCH) {
                    chipX += ImGui::CalcTextSize(TR("Match in progress - the game continues while this menu is open")).x +
                             style.ItemSpacing.x * 2;
                }
                char pendingText[96];
                std::snprintf(pendingText, sizeof(pendingText), TR("%d change(s) pending restart"), sPendingRestart);
                ImGui::SetCursorScreenPos(ImVec2(chipX, hintLineY));
                ImGui::TextColored(UIWidgets::SemanticColors::Warning, "%s", pendingText);
                float chipNextX = chipX + ImGui::CalcTextSize(pendingText).x + style.ItemSpacing.x * 2;
                ImGui::SetCursorScreenPos(ImVec2(chipNextX, hintLineY));
                if (ImGui::SmallButton(TR("Details"))) {
                    ImGui::OpenPopup("##pending_restart_details");
                }
                chipNextX += ImGui::GetItemRectSize().x + style.ItemSpacing.x * 2;
                if (ImGui::BeginPopup("##pending_restart_details")) {
                    ImGui::Text("%s", TR("Changed options requiring restart:"));
                    ImGui::Separator();
                    for (const auto& [cvar, pe] : sRestartBaseline) {
                        const int32_t cur = CVarGetInteger(cvar.c_str(), pe.baseline);
                        if (cur == pe.baseline) {
                            continue;
                        }
                        ImGui::PushID(cvar.c_str());
                        ImGui::Text("%s: %d -> %d", TR(pe.name.c_str()), pe.baseline, cur);
                        ImGui::SameLine();
                        if (ImGui::SmallButton(TR("Revert"))) {
                            CVarSetInteger(cvar.c_str(), pe.baseline);
                            CVarSave();
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndPopup();
                }
                ImGui::SetCursorScreenPos(ImVec2(chipNextX, hintLineY));
                if (ImGui::SmallButton(TR("Restart now"))) {
                    std::reinterpret_pointer_cast<Ship::ConsoleWindow>(
                        Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow("Console"))
                        ->Dispatch("reset");
                }
            }
            const float hintX =
                menuBlockPos.x + menuSize.x - ImGui::CalcTextSize(hints).x - style.ItemSpacing.x * 2;
            ImGui::SetCursorScreenPos(
                ImVec2(ImMax(hintX, menuBlockPos.x + style.ItemSpacing.x * 2), hintLineY));
            ImGui::TextDisabled(TR("%s"), hints);
        }
    }

    if (standardSmall != nullptr) {
        ImGui::PopFont();
    }

    if (!popout) {
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    if (popout) {
        poppedSize = ImGui::GetWindowSize();
        poppedPos = ImGui::GetWindowPos();
    }
    if (freshOpen) {
        freshOpen = false;
        scrollToActiveFrames = 4;
        /* §34 baseline: RequiresRestart CVars as they stood when the menu
         * opened - the footer diff counts how many are still pending. */
        sRestartBaseline.clear();
        sPendingRestart = 0;
        for (auto& [mainName, entry] : menuEntries) {
            for (auto& [sbName, sb] : entry.sidebars) {
                for (auto& col : sb.columnWidgets) {
                    for (auto& w : col) {
                        if (w.applyPolicy == ApplyPolicy::RequiresRestart && w.cVar != nullptr) {
                            sRestartBaseline[w.cVar] = { CVarGetInteger(w.cVar, 0), w.name };
                        }
                    }
                }
            }
        }
        if (port_diag_safe_mode_active()) {
            ssb64::ToastWarning(
                TR("Safe mode is on: mods and hi-res texture packs were skipped after a crash. Your settings were not changed."));
        }
    }
    if (scrollToActiveFrames > 0) {
        scrollToActiveFrames--;
    }
    ImGui::End();
    ImGui::PopStyleColor(pushedColors);
}
} // namespace Ship
