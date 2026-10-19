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

#include "Menu_internal.h"
#include "crash_bundle.h"

namespace Ship {
/* §5 — pad hints follow the connected controller's family, since button
 * letters sit at different physical positions (FaceDown = accept is A on
 * Xbox, B on Nintendo pads, Cross on PlayStation). SDL's name for the first
 * recognized controller is enough — hot-plug changes apply next frame. */
PadFamily DetectPadFamily() {
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

int PushMenuBasePalette(MenuBaseTheme theme) {
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

ImVec4 MenuWindowBgColor(MenuBaseTheme theme, float alpha) {
    if (theme == MenuBaseTheme::Light) {
        return ImVec4(0.97f, 0.97f, 0.98f, alpha);
    }
    if (theme == MenuBaseTheme::HighContrast) {
        return ImVec4(0.0f, 0.0f, 0.0f, (std::max)(alpha, 0.95f));
    }
    return ImVec4(0.0f, 0.0f, 0.0f, alpha);
}

ImVec4 MenuTextDisabledColor(MenuBaseTheme theme) {
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
SectionUndo sSectionUndo;

void SnapshotSectionCvars(const SidebarEntry& section, SectionUndo& undo) {
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

void ApplySectionUndo(SectionUndo& undo) {
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
size_t ResetSectionCvars(const SidebarEntry& section, bool apply) {
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

std::string disabledTempTooltip;
const char* disabledTooltip;
bool disabledValue = false;

bool navigateToWidget = false;
std::string navigateMainEntry = "";
std::string navigateSidebar = "";
std::string navigateWidgetName = "";
bool highlightWidget = false;
int sHighlightTtl = 0; /* frames left to find the navigation target */

bool operator==(Color_RGB8 const& l, Color_RGB8 const& r) noexcept {
    return l.r == r.r && l.g == r.g && l.b == r.b;
}

bool operator==(Color_RGBA8 const& l, Color_RGBA8 const& r) noexcept {
    return l.r == r.r && l.g == r.g && l.b == r.b && l.a == r.a;
}

bool operator<(Color_RGB8 const& l, Color_RGB8 const& r) noexcept {
    return std::tie(l.r, l.g, l.b) < std::tie(r.r, r.g, r.b);
}

bool operator<(Color_RGBA8 const& l, Color_RGBA8 const& r) noexcept {
    return std::tie(l.r, l.g, l.b, l.a) < std::tie(r.r, r.g, r.b, r.a);
}

bool operator>(Color_RGB8 const& l, Color_RGB8 const& r) noexcept {
    return std::tie(l.r, l.g, l.b) > std::tie(r.r, r.g, r.b);
}

bool operator>(Color_RGBA8 const& l, Color_RGBA8 const& r) noexcept {
    return std::tie(l.r, l.g, l.b, l.a) > std::tie(r.r, r.g, r.b, r.a);
}

uint32_t GetVectorIndexOf(std::vector<std::string>& vector, std::string value) {
    return (uint32_t)std::distance(vector.begin(), std::find(vector.begin(), vector.end(), value));
}

bool raceDisableActive = false;

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


/* Shared rendering for a single search hit: caption showing where the widget
 * lives plus a "Go to" shortcut wired to the navigateToWidget machinery. */
void Menu::DrawSearchHit(WidgetInfo& info, const std::string& origin,
                         const std::string& entryName, const std::string& sidebarName) {
    ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(UIWidgets::Colors::Gray));
    std::string caption = origin;
    if (info.advanced && SimpleMode()) {
        caption += TR(" - hidden in Simple mode");
    }
    ImGui::Text(TR("%s"), caption.c_str());
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushID(info.stableId.empty() ? info.name.c_str() : info.stableId.c_str());
    if (ImGui::SmallButton(TR("Go to"))) {
        navigateToWidget = true;
        navigateMainEntry = entryName;
        navigateSidebar = sidebarName;
        navigateWidgetName = info.name;
    }
    ImGui::PopID();
}

/* Haystack matching shared by all search producers: lowercase, spaces
 * stripped — the query is normalized the same way by DrawSearchResults. */
static bool SearchMatches(std::string haystack, const std::string& needle) {
    std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::tolower);
    haystack.erase(std::remove(haystack.begin(), haystack.end(), ' '), haystack.end());
    return haystack.find(needle) != std::string::npos;
}

uint32_t Menu::DrawSearchResultWidgets(const std::string& menuSearchText) {
    uint32_t count = 0;
    for (auto& menuLabel : menuOrder) {
        auto& menuEntry = menuEntries.at(menuLabel);
        for (auto& sidebarLabel : menuEntry.sidebarOrder) {
            auto& sidebar = menuEntry.sidebars[sidebarLabel];
            for (size_t i = 0; i < sidebar.columnWidgets.size(); i++) {
                for (auto& info : sidebar.columnWidgets.at(i)) {
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
                    if (!SearchMatches(widgetStr, menuSearchText)) {
                        continue;
                    }
                    UIWidgets::ComponentAlignments backupAlignment{};
                    UIWidgets::LabelPositions backupLabelPos{};
                    const bool isCombo =
                        info.type == WIDGET_COMBOBOX || info.type == WIDGET_CVAR_COMBOBOX;
                    if (isCombo) {
                        auto options = std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options);
                        backupAlignment = options->alignment;
                        backupLabelPos = options->labelPosition;
                        options->alignment = UIWidgets::ComponentAlignments::Left;
                        options->labelPosition = UIWidgets::LabelPositions::Above;
                    }
                    MenuDrawItem(info, 400, menuThemeIndex);
                    DrawSearchHit(info, fmt::format("  ({} -> {})", menuEntry.label, sidebarLabel),
                                  menuEntry.label, sidebarLabel);
                    count++;
                    if (isCombo) {
                        auto options = std::static_pointer_cast<UIWidgets::ComboboxOptions>(info.options);
                        options->alignment = backupAlignment;
                        options->labelPosition = backupLabelPos;
                    }
                }
            }
        }
    }
    return count;
}

uint32_t Menu::DrawSearchResultExtras(const std::string& menuSearchText) {
    uint32_t count = 0;
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
        if (!SearchMatches(widgetStr, menuSearchText)) {
            continue;
        }
        MenuDrawItem(entry.info, 400, menuThemeIndex);
        DrawSearchHit(entry.info,
                      fmt::format("  ({} -> {}, {})", entry.menuName, entry.sidebarName, entry.location),
                      entry.menuName, entry.sidebarName);
        count++;
    }
    return count;
}

/* Action entries — the search box doubles as a command palette for
 * the few global actions players hunt for most. */
uint32_t Menu::DrawSearchActionEntries(const std::string& menuSearchText) {
    struct SearchAction {
        const char* name = nullptr;
        const char* terms = nullptr;
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
    uint32_t count = 0;
    for (const auto& action : kSearchActions) {
        if (!SearchMatches(std::string(action.name) + action.terms, menuSearchText)) {
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
        count++;
    }
    return count;
}

uint32_t Menu::DrawSearchResults(std::string& menuSearchText) {
    std::transform(menuSearchText.begin(), menuSearchText.end(), menuSearchText.begin(), ::tolower);
    menuSearchText.erase(std::remove(menuSearchText.begin(), menuSearchText.end(), ' '), menuSearchText.end());
    ImGui::SetNextWindowSizeConstraints({ ImGui::GetContentRegionAvail().x / 2, 0 },
                                        { ImGui::GetContentRegionAvail().x / 2, ImGui::GetContentRegionAvail().y });
    uint32_t searchCount = 0;
    if (ImGui::BeginChild("Search Results Col 1", { ImGui::GetContentRegionAvail().x / 2, 0 },
                          ImGuiChildFlags_AutoResizeY, ImGuiWindowFlags_NoTitleBar)) {
        searchCount += DrawSearchResultWidgets(menuSearchText);
        searchCount += DrawSearchResultExtras(menuSearchText);
        searchCount += DrawSearchActionEntries(menuSearchText);
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

/* Non-value widget cases (backend pickers, separators, buttons,
 * custom/search). Done = drawn (or the only path out of the case);
 * Bail = abort MenuDrawItem before the shared epilogue (matches the
 * old bare `return` paths); NotMine = dispatch on to the value
 * widget switch. */
/* WIDGET_SEARCH: clear button + autofocus handling + live results.
 * Returns true while the query is non-empty. */
bool Menu::DrawSearchWidget(UIWidgets::Colors themeIndex) {
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
        return false;
    }
    DrawSearchResults(menuSearchText);
    return true;
}


/* Audio/video backend selectors — both comboboxes bound to live backend
 * state rather than a widget value pointer. */
/* Window-button widget: button toggles the target window; with embedWindow
 * the window's contents render inline instead. Missing window names are a
 * data error, logged rather than crashing the menu frame. */
void Menu::DrawWindowButtonWidget(WidgetInfo& widget, UIWidgets::Colors themeIndex) {
    if (widget.windowName == nullptr || widget.windowName[0] == '\0') {
        std::string msg =
            fmt::format(fmt::runtime(TR("Error drawing window contents for {}: windowName not defined")), widget.name);
        SPDLOG_ERROR(msg.c_str());
        return;
    }
    auto window = Ship::Context::GetInstance()->GetWindow()->GetGui()->GetGuiWindow(widget.windowName);
    if (!window) {
        std::string msg =
            fmt::format(fmt::runtime(TR("Error drawing window contents: windowName {} does not exist")), widget.windowName);
        SPDLOG_ERROR(msg.c_str());
        return;
    }
    auto options = std::static_pointer_cast<UIWidgets::WindowButtonOptions>(widget.options);
    options->color = themeIndex;
    if (options->showButton) {
        UIWidgets::WindowButton(TR(widget.name.c_str()), widget.cVar, window, *options);
    }
    if (!window->IsVisible() && options->embedWindow) {
        window->DrawElement();
    }
}


bool Menu::DrawBackendSelector(WidgetInfo& widget, UIWidgets::Colors themeIndex) {
    if (widget.type == WIDGET_AUDIO_BACKEND) {
        auto currentAudioBackend = Ship::Context::GetInstance()->GetAudio()->GetCurrentAudioBackend();
        UIWidgets::ComboboxOptions options = {};
        options.color = themeIndex;
        options.tooltip = "Sets the audio API used by the game. Requires a relaunch to take effect.";
        options.disabled = Ship::Context::GetInstance()->GetAudio()->GetAvailableAudioBackends()->size() <= 1;
        options.disabledTooltip = "Only one audio API is available on this platform.";
        if (UIWidgets::Combobox("Audio API", &currentAudioBackend, audioBackendsMap, options)) {
            Ship::Context::GetInstance()->GetAudio()->SetCurrentAudioBackend(currentAudioBackend);
        }
        return true;
    }
    UIWidgets::ComboboxOptions options = {};
    options.color = themeIndex;
    options.tooltip = "Sets the renderer API used by the game.";
    options.disabled = availableWindowBackends->size() <= 1;
    options.disabledTooltip = "Only one renderer API is available on this platform.";
    if (UIWidgets::Combobox("Renderer API", &configWindowBackend, availableWindowBackendsMap, options)) {
        Ship::Context::GetInstance()->GetConfig()->SetInt("Window.Backend.Id", (int32_t)(configWindowBackend));
        Ship::Context::GetInstance()->GetConfig()->SetString("Window.Backend.Name",
                                                             windowBackendsMap.at(configWindowBackend));
        Ship::Context::GetInstance()->GetConfig()->Save();
        UpdateWindowBackendObjects();
    }
    return true;
}

/* Widgets fire their change-callback only when the widget reports a
 * committed edit; keeping it as one-liner keeps each case body linear. */
} // namespace Ship
