// Per-frame menu draw + element lifecycle — extracted from Menu.cpp.

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
#include "Menu_internal.h"
#include "crash_bundle.h"

namespace Ship {
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
struct PendingRestart { int32_t baseline = 0; std::string name; };
static std::unordered_map<std::string, PendingRestart> sRestartBaseline;
static int sPendingRestart = 0;

/* §34 progressive disclosure: Simple mode (gSettings.MenuMode=0) hides
 * advanced widgets and whole-advanced sidebars in normal browsing; search
 * still surfaces them so nothing becomes unreachable. */
bool SimpleMode() {
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
/* Per-frame environment sync: race-condition disable flag, UI scale (style
 * rescale is multiplicative so it only applies on change), reduced-motion
 * cursor blink, and the focus-help capture buffers. */
void Menu::SyncFrameEnvironment() {
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
}

/* Popout state + one-shot geometry: entering popout re-applies the saved
 * size/pos; leaving persists it back to CVars. `popped` is the last-frame
 * latch so the Set* calls only run on the transition frame. */
void Menu::ApplyPopoutTransition(ElementFrame& f, bool canPopout, ImGuiWindowFlags& windowFlags) {
    f.popout = CVarGetInteger(CVAR_SETTING("Menu.Popout"), 0) && canPopout;
    if (f.popout) {
        windowFlags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoDocking;
    }
    if (f.popout != popped) {
        if (f.popout) {
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
    popped = f.popout;
}

bool Menu::BeginElement(ElementFrame& f) {
    f.fontLarge = GetMenuFont("MenuStandard");
    f.fontSmall = GetMenuFont("MenuStandardSmall");
    if (f.fontLarge == nullptr) {
        f.fontLarge = ImGui::GetFont();
    }
    if (f.fontSmall == nullptr) {
        f.fontSmall = f.fontLarge;
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

    SyncFrameEnvironment();

    windowHeight = ImGui::GetMainViewport()->WorkSize.y;
    windowWidth = ImGui::GetMainViewport()->WorkSize.x;
    auto windowFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings;
    ApplyPopoutTransition(f, allowPopout, windowFlags);
    auto windowCond = ImGuiCond_Always;
    if (!f.popout) {
        ImGui::SetNextWindowSize({ static_cast<float>(windowWidth), static_cast<float>(windowHeight) }, windowCond);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), windowCond, { 0.5f, 0.5f });
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    }

    /* §8 — base theme palette first (Dark is the stock palette: no pushes),
     * then the per-theme surface/focus colors on top. */
    const auto baseTheme = static_cast<MenuBaseTheme>(
        std::clamp(CVarGetInteger(CVAR_SETTING("Menu.BaseTheme"), 0), 0, 2));
    f.pushedColors = PushMenuBasePalette(baseTheme) + 3;

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
        if (!f.popout) {
            ImGui::PopStyleVar();
        }
        freshOpen = true;
        ImGui::PopStyleColor(f.pushedColors);
        ImGui::End();
        return false;
    }

    if (popped != f.popout) {
        if (!f.popout) {
            ImGui::PopStyleVar();
        }
        CVarSetInteger(CVAR_SETTING("Menu.Popout"), popped);
        CVarSetFloat(CVAR_SETTING("Menu.PoppedWidth"), poppedSize.x);
        CVarSetFloat(CVAR_SETTING("Menu.PoppedHeight"), poppedSize.y);
        CVarSetFloat(CVAR_SETTING("Menu.PoppedPos.x"), poppedSize.x);
        CVarSetFloat(CVAR_SETTING("Menu.PoppedPos.y"), poppedSize.y);
        CVarSave();
        ImGui::End();
        ImGui::PopStyleColor(f.pushedColors);
        return false;
    }
    return true;
}

/* Trailing header actions: Quit / Reset (optional per subclass) and
 * the always-present Close button. */
void Menu::DrawHeaderActions(ElementFrame& f) {
bool headerQuit = mShowHeaderQuit;
bool headerReset = mShowHeaderReset;
#if defined(__ANDROID__)
headerQuit = false;
#endif
const int trailingButtons = 1 + (headerReset ? 1 : 0) + (headerQuit ? 1 : 0);
ImGui::SameLine(f.menuSize.x - (f.buttonSize.x * trailingButtons) - (f.style->ItemSpacing.x * (trailingButtons - 1)));
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

}


/* Scrollable tab strip across the top of the menu — one header entry per
 * registered top-level menu. Updates f.headerIndex / f.sidebar on click and
 * scrolls the active tab into view on the first frame after opening. */
void Menu::DrawHeaderTabStrip(ElementFrame& f, const char* headerCvar) {
    uint8_t curIndex = 0;
    for (auto& label : menuOrder) {
        if (curIndex != 0) {
            ImGui::SameLine();
        }
        auto& entry = menuEntries.at(label);
        std::string nextIndex = label;
        UIWidgets::PushStyleButton(menuThemeIndex);
        if (f.headerIndex != label) {
            ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
        }
        if (f.headerIndex == label && freshOpen) {
            // Keep the active tab visible when the strip is scrolled.
            ImGui::SetScrollHereX(0.5f);
        }
        if (ModernMenuHeaderEntry(entry.label)) {
            if (f.headerSearch) {
                menuSearch.Clear();
            }
            CVarSetString(headerCvar, label.c_str());
            CVarSave();
            nextIndex = label;
        }
        if (f.headerIndex != label) {
            ImGui::PopStyleColor();
        }
        UIWidgets::PopStyleButton();
        if (f.headerIndex == label) {
            f.sidebar = &entry.sidebars;
        }
        if (nextIndex != label) {
            f.headerIndex = nextIndex;
        }
        curIndex++;
    }
}

/* Pinned search field between the scrolling tab strip and the trailing
 * buttons. The "Search..." placeholder is painted straight into the draw
 * list — a cursor-seeking SameLine() would disturb the absolute positioning
 * the trailing buttons rely on. */
void Menu::DrawHeaderSearchField(ElementFrame& f, bool autoFocus) {
    f.menuSearchText = "";
    if (!f.headerSearch) {
        return;
    }
    ImGui::SameLine();
    if (autoFocus && freshOpen) {
        ImGui::SetKeyboardFocusHere();
    }
    auto color = UIWidgets::ColorValues.at(menuThemeIndex);
    color.w = 0.6f;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, color);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 3.0f);
    menuSearch.Draw("##search", 200.0f);
    f.menuSearchText = menuSearch.InputBuf;
    f.menuSearchText.erase(std::remove(f.menuSearchText.begin(), f.menuSearchText.end(), ' '), f.menuSearchText.end());
    if (f.menuSearchText.length() < 1) {
        const ImVec2 rmin = ImGui::GetItemRectMin();
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(rmin.x + f.style->FramePadding.x, rmin.y + f.style->FramePadding.y),
            ImGui::GetColorU32(UIWidgets::SemanticColors::Placeholder), TR("Search..."));
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

void Menu::DrawElementHeader(ElementFrame& f) {
    const char* headerCvar = CVAR_SETTING("Menu.ActiveHeader");
    ImGui::PushFont(f.fontLarge);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
    f.headerIndex = CVarGetString(headerCvar, "Settings");
    f.pos = f.window->DC.CursorPos;
    float centerX = f.pos.x + windowWidth / 2 - (f.style->ItemSpacing.x * (menuEntries.size() + 1));
    std::vector<ImVec2> headerSizes;
    float headerWidth = 0.0f;
    f.headerSearch = !CVarGetInteger(CVAR_SETTING("Menu.SidebarSearch"), 0);
    for (auto& label : menuOrder) {
        ImVec2 size = ImGui::CalcTextSize(label.c_str());
        headerSizes.push_back(size);
        headerWidth += size.x + f.style->FramePadding.x * 2 + f.style->ItemSpacing.x;
    }

    // Full screen menu with widths below 1280, heights below 800.
    // 5% of screen width/height padding on both sides above those resolutions.
    // Menu width will never exceed a 16:9 aspect ratio.
    f.menuSize = { windowWidth, windowHeight };
    if (windowWidth > 1280) {
        f.menuSize.x = std::fminf(windowWidth * 0.9f, (windowHeight * 1.77f));
    }
    if (windowHeight > 800) {
        f.menuSize.y = windowHeight * 0.9f;
    }

    f.pos += f.window->WorkRect.GetSize() / 2 - f.menuSize / 2;
    ImGui::SetNextWindowPos(f.pos);
    f.menuBlockPos = f.pos;
    ImGui::BeginChild("Menu Block", f.menuSize,
                      ImGuiChildFlags_AutoResizeX | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoScrollbar);

    f.sidebar = nullptr;
    f.headerHeight = headerSizes.at(0).y + f.style->FramePadding.y * 2;
    f.headerRowH = f.headerHeight;
    f.buttonSize = ImGui::CalcTextSize(ICON_FA_TIMES_CIRCLE) + f.style->FramePadding * 2;
    /* Reserve the pinned search field and trailing buttons: the search lives
     * outside the scrolling tab strip, so its width is a fixed affordance the
     * strip must leave room for — longer translated labels overflow into the
     * child's scrollbar instead of clipping the input. */
    const float searchW = f.headerSearch ? 200.0f : 0.0f;
    const float headerReserved = f.buttonSize.x * 3 + searchW + f.style->ItemSpacing.x * 4;
    bool scrollbar = false;
    if (headerWidth > f.menuSize.x - headerReserved) {
        f.headerHeight += f.style->ScrollbarSize;
        scrollbar = true;
    }
    ImVec2 headerSelSize = { f.menuSize.x - headerReserved, f.headerHeight };
    /* The child used to be constrained to the CONTENT width (headerWidth), so
     * on narrow windows AlwaysAutoResize grew it past the menu edge and the
     * Close/Reset/Quit buttons painted on top of the tab strip. Clamp it to
     * the space actually available — the HorizontalScrollbar flag inside
     * handles the overflow. */
    headerSelSize.x = ImMax(headerSelSize.x, 40.0f);
    ImGui::SetNextWindowSizeConstraints({ 0, f.headerHeight }, { headerSelSize.x, headerSelSize.y });
    if (scrollbar) {
        headerSelSize.y += f.style->ScrollbarSize;
    }
    bool autoFocus = CVarGetInteger(CVAR_SETTING("Menu.SearchAutofocus"), 0);
    /* No AutoResizeX: it resizes the frame to the CONTENT width in EndChild,
     * ignoring the constraint above, so tabs wider than the reserved space
     * would grow the child past the pinned search field and buttons. A fixed
     * width keeps overflow inside the child's horizontal scrollbar. */
    ImGui::BeginChild("Header Selection", headerSelSize,
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_HorizontalScrollbar);
    DrawHeaderTabStrip(f, headerCvar);
    ImGui::EndChild();
    /* Pinned search field — sits between the scrolling tab strip and the
     * trailing buttons so longer translated labels can never push it out
     * of view or under the last tab. */
    DrawHeaderSearchField(f, autoFocus);
    /* Trailing header actions. Quit/Reset are optional — subclasses that move
     * them into regular menu entries (e.g. a Support f.sidebar) keep only the
     * Close button here, so "leave the menu" and "quit the app" can't be
     * fat-fingered into each other. Android never shows Quit: mobile users
     * dismiss the app by swiping it away. */
    DrawHeaderActions(f);
    f.pos.y += f.headerHeight + f.style->ItemSpacing.y;
    f.pos.x = centerX - f.menuSize.x / 2 + (f.style->ItemSpacing.x * (menuEntries.size() + 1));
    f.window->DrawList->AddRectFilled(f.pos, f.pos + ImVec2{ f.menuSize.x, 4 }, ImGui::GetColorU32({ 255, 255, 255, 255 }),
                                    true, static_cast<ImDrawFlags>(f.style->WindowRounding));
    f.pos.y += f.style->ItemSpacing.y;
    f.sectionHeight = f.menuSize.y - f.headerHeight - 4 - f.style->ItemSpacing.y * 2;
    f.columnHeight = f.sectionHeight - f.style->ItemSpacing.y * 4;
}

/* Effective section for this header: the persisted CVar value when it still
 * points at a visible sidebar, else the first non-hidden entry, else the
 * declared first (matches the previous fallback chain exactly). */
std::string Menu::ResolveSectionIndex(ElementFrame& f) {
    std::string sectionIndex = CVarGetString(menuEntries.at(f.headerIndex).sidebarCvar, "");
    if (f.sidebar->contains(sectionIndex) && !SidebarHiddenInSimple(f.sidebar->at(sectionIndex))) {
        return sectionIndex;
    }
    for (auto& l : menuEntries.at(f.headerIndex).sidebarOrder) {
        if (!SidebarHiddenInSimple(f.sidebar->at(l))) {
            return l;
        }
    }
    return menuEntries.at(f.headerIndex).sidebarOrder.at(0);
}

void Menu::DrawCompactSidebarStrip(ElementFrame& f) {
    const char* sidebarCvar = menuEntries.at(f.headerIndex).sidebarCvar;
    const float sidebarStripH = f.headerRowH + f.style->ScrollbarSize;
    ImGui::SetNextWindowPos(f.pos + f.style->ItemSpacing * 2);
ImGui::BeginChild((menuEntries.at(f.headerIndex).label + " Section").c_str(),
                  { f.menuSize.x - f.style->ItemSpacing.x * 4, sidebarStripH }, ImGuiChildFlags_None,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_HorizontalScrollbar |
                      ImGuiWindowFlags_AlwaysHorizontalScrollbar);
bool firstEntry = true;
for (auto& sidebarLabel : menuEntries.at(f.headerIndex).sidebarOrder) {
    if (SidebarHiddenInSimple(f.sidebar->at(sidebarLabel))) {
        continue;
    }
    if (!firstEntry) {
        ImGui::SameLine();
    }
    firstEntry = false;
    std::string nextIndex = "";
    UIWidgets::PushStyleButton(menuThemeIndex);
    if (f.sectionIndex != sidebarLabel) {
        ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
    }
    /* Header-style entry: the f.sidebar variant centers itself on the
     * f.window width, which breaks in a horizontal strip. */
    if (ModernMenuHeaderEntry(sidebarLabel)) {
        if (f.headerSearch) {
            menuSearch.Clear();
        }
        CVarSetString(sidebarCvar, sidebarLabel.c_str());
        CVarSave();
        nextIndex = sidebarLabel;
    }
    if (f.sectionIndex != sidebarLabel) {
        ImGui::PopStyleColor();
    }
    UIWidgets::PopStyleButton();
    /* Scroll the active section into view once it exists —
     * SetScrollHereX needs the item's real rect, so request it after
     * ItemAdd, and force max scroll for the last entry whose centered
     * target still clamps short of fully visible. */
    if (f.sectionIndex == sidebarLabel && scrollToActiveFrames > 0) {
        ImGui::SetScrollHereX(0.5f);
        if (&sidebarLabel == &menuEntries.at(f.headerIndex).sidebarOrder.back()) {
            ImGui::SetScrollX(ImGui::GetScrollMaxX());
        }
    }
    if (nextIndex != "") {
        f.sectionIndex = nextIndex;
    }
}
ImGui::EndChild();
}


void Menu::DrawVerticalSidebarList(ElementFrame& f) {
    const char* sidebarCvar = menuEntries.at(f.headerIndex).sidebarCvar;
ImGui::SetNextWindowSizeConstraints({ f.sidebarWidth, 0 }, { f.sidebarWidth, f.columnHeight });
ImGui::BeginChild((menuEntries.at(f.headerIndex).label + " Section").c_str(),
                  { f.sidebarWidth, f.columnHeight * 3 },
                  ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysAutoResize,
                  ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
for (auto& sidebarLabel : menuEntries.at(f.headerIndex).sidebarOrder) {
    if (SidebarHiddenInSimple(f.sidebar->at(sidebarLabel))) {
        continue;
    }
    std::string nextIndex = "";
    UIWidgets::PushStyleButton(menuThemeIndex);
    if (f.sectionIndex != sidebarLabel) {
        ImGui::PushStyleColor(ImGuiCol_Button, { 0, 0, 0, 0 });
    }
    if (f.sectionIndex == sidebarLabel && scrollToActiveFrames > 0) {
        ImGui::SetScrollHereY(0.5f);
    }
    if (ModernMenuSidebarEntry(sidebarLabel)) {
        if (f.headerSearch) {
            menuSearch.Clear();
        }
        CVarSetString(sidebarCvar, sidebarLabel.c_str());
        CVarSave();
        nextIndex = sidebarLabel;
    }
    if (f.sectionIndex != sidebarLabel) {
        ImGui::PopStyleColor();
    }
    UIWidgets::PopStyleButton();
    if (nextIndex != "") {
        f.sectionIndex = nextIndex;
    }
}
ImGui::EndChild();
}


void Menu::DrawElementSections(ElementFrame& f) {
    /* Compact layout: below ~560px of menu width a vertical 300px f.sidebar
     * leaves no room for content, so the section list collapses into a
     * horizontal scrolling strip above a full-width content area. */
    f.compactLayout = f.menuSize.x < 560.0f;

    /* Footer reservation: the help strip + action-hint bar sit at the bottom
     * of the menu block, pinned by absolute cursor position later. Always
     * reserve when enabled so the layout doesn't jump as the help text
     * appears and disappears. In compact mode only a single hint line is
     * reserved — a 3-line help strip would eat half of a 240px f.window, and
     * persistent descriptions cover the same info inline. */
    const float footerLineH = ImGui::GetFontSize() + f.style->FramePadding.y * 2;
    f.footerH = 0.0f;
    if (f.compactLayout) {
        if (mShowHelpBar || mShowActionBar) {
            f.footerH = footerLineH + f.style->ItemSpacing.y;
        }
    } else {
        if (mShowHelpBar) {
            f.footerH += footerLineH * 3 + f.style->ItemSpacing.y;
        }
        if (mShowActionBar) {
            f.footerH += footerLineH + f.style->ItemSpacing.y;
        }
    }
    f.columnHeight -= f.footerH;
    ImGui::SetNextWindowPos(f.pos + f.style->ItemSpacing * 2);

    // Increase f.sidebar width on larger screens to accommodate people scaling their menus.
    f.sidebarWidth = 300 - f.style->ItemSpacing.x;
    if (f.menuSize.x > 1600) {
        f.sidebarWidth = f.menuSize.x * 0.15f;
    }

    f.sectionIndex = ResolveSectionIndex(f);
    const float sidebarStripH =
        f.compactLayout ? (f.headerRowH + f.style->ScrollbarSize) : 0.0f;
    if (f.compactLayout) {
        f.columnHeight -= sidebarStripH + f.style->ItemSpacing.y * 2;
    }
    f.columnHeight = ImMax(f.columnHeight, ImGui::GetFontSize() * 2.0f);

    float sectionCenterX = f.pos.x + (f.sidebarWidth / 2);
    float topY = f.pos.y;
    if (f.compactLayout) {
        DrawCompactSidebarStrip(f);
    } else {
        DrawVerticalSidebarList(f);
    }
    ImGui::PopFont();

    if (f.fontSmall != nullptr) {
        ImGui::PushFont(f.fontSmall);
    }

    
    if (f.compactLayout) {
        f.pos = ImVec2{ f.menuBlockPos.x + f.style->ItemSpacing.x * 2,
                      topY + sidebarStripH + f.style->ItemSpacing.y * 3 };
        ImGui::SetNextWindowPos(f.pos);
        f.sectionWidth = f.menuSize.x - f.style->ItemSpacing.x * 4;
    } else {
        f.pos = ImVec2{ sectionCenterX + (f.sidebarWidth / 2), topY } + f.style->ItemSpacing * 2;
        f.window->DrawList->AddRectFilled(f.pos, f.pos + ImVec2{ 4, f.sectionHeight - f.style->FramePadding.y * 2 },
                                        ImGui::GetColorU32({ 255, 255, 255, 255 }), true, static_cast<ImDrawFlags>(f.style->WindowRounding));
        f.pos.x += 4 + f.style->ItemSpacing.x;
        ImGui::SetNextWindowPos(f.pos + f.style->ItemSpacing);
        f.sectionWidth = f.menuSize.x - f.sidebarWidth - 4 - f.style->ItemSpacing.x * 4;
    }
    f.sectionMenuId = f.sectionIndex + " Settings";
    f.columns = f.sidebar->at(f.sectionIndex).columnCount;
    f.columnFuncs = f.sidebar->at(f.sectionIndex).columnWidgets.size();
    if (windowWidth < 800) {
        f.columns = 1;
    }
    f.useColumns = f.columns > 1;
}

/* Per-section widget columns. `highlightWidget` is the shared
 * navigate-to-target state: when a search result's "Go to" lands here the
 * target row scrolls into view and gets an accent outline for one frame. */
/* Search-results pane: header row + separator + the three result producers
 * (widgets, extras, action entries) + an empty-state placeholder. */
void Menu::DrawSearchPane(ElementFrame& f) {
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
    if (DrawSearchResults(f.menuSearchText) == 0) {
        ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize("No results found").x) / 2);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10.0f);
        ImGui::TextColored(UIWidgets::SemanticColors::Placeholder, "%s", TR("No results found"));
    }
}

void Menu::DrawSectionColumns(ElementFrame& f, float columnWidth) {
    for (size_t i = 0; i < f.columnFuncs; i++) {
        std::string sectionId = fmt::format(fmt::runtime(TR("{} Column {}")), f.sectionMenuId, i);
        if (f.useColumns) {
            ImGui::SetNextWindowSizeConstraints({ columnWidth, 0 }, { columnWidth, f.columnHeight });
            ImGui::BeginChild(sectionId.c_str(), { columnWidth, windowHeight * 4 }, ImGuiChildFlags_AutoResizeY,
                              ImGuiWindowFlags_NoTitleBar);
        }
        for (auto& entry : f.sidebar->at(f.sectionIndex).columnWidgets.at(i)) {
            if (entry.advanced && SimpleMode()) {
                continue;
            }
            const bool isNavTarget = highlightWidget && entry.name == navigateWidgetName;
            if (isNavTarget) {
                ImGui::SetScrollHereY(0.35f);
            }
            MenuDrawItem(entry, 90 / f.sidebar->at(f.sectionIndex).columnCount, menuThemeIndex);
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
        if (f.useColumns) {
            ImGui::EndChild();
        }
        if (i < f.columns - 1) {
            ImGui::SameLine();
        }
    }
}

/* "Restore section defaults" + one-shot undo — only rendered when the
 * section actually overrides some CVar (probed by the dry-run reset). */
void Menu::DrawSectionRestoreRow(ElementFrame& f) {
    if (ResetSectionCvars(f.sidebar->at(f.sectionIndex), false) == 0) {
        return;
    }
    ImGui::Spacing();
    if (UIWidgets::Button(TR("Restore section defaults"),
            UIWidgets::ButtonOptions()
                .Tooltip("Reset every option in this section to its default value. "
                         "Undo is available until you restore another section.")
                .Size(UIWidgets::Sizes::Inline))) {
        SnapshotSectionCvars(f.sidebar->at(f.sectionIndex), sSectionUndo);
        sSectionUndo.sidebar = f.sectionIndex;
        const size_t restored = ResetSectionCvars(f.sidebar->at(f.sectionIndex), true);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ssb64::ToastInfo(
            fmt::format(fmt::runtime(TR("Restored {} option{} to defaults")), restored, restored == 1 ? "" : "s"));
    }
    if (!sSectionUndo.entries.empty() && sSectionUndo.sidebar == f.sectionIndex) {
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

void Menu::DrawElementContent(ElementFrame& f) {
    const float columnWidth = (f.sectionWidth - f.style->ItemSpacing.x * f.columns) / f.columns;
    if (!f.useColumns || (f.headerSearch && f.menuSearchText.length() > 0)) {
        ImGui::SameLine();
        ImGui::SetNextWindowSizeConstraints({ f.sectionWidth, 0 }, { f.sectionWidth, f.columnHeight });
        ImGui::BeginChild(f.sectionMenuId.c_str(), { f.sectionWidth, windowHeight * 4 }, ImGuiChildFlags_AutoResizeY,
                          ImGuiWindowFlags_NoTitleBar);
    }
    if (f.headerSearch && f.menuSearchText.length() > 0) {
        DrawSearchPane(f);
    } else {
        std::string menuLabel = menuEntries.at(f.headerIndex).label;
        if (MenuInit::GetUpdateFuncs().contains(menuLabel)) {
            if (MenuInit::GetUpdateFuncs()[menuLabel].contains(f.sectionIndex)) {
                for (auto& updateFunc : MenuInit::GetUpdateFuncs()[menuLabel][f.sectionIndex]) {
                    updateFunc();
                }
            }
        }
        DrawSectionColumns(f, columnWidth);
        DrawSectionRestoreRow(f);
    }
    // Mirror the open-guard at line ~954. Earlier versions of this code
    // dropped the f.headerSearch qualifier here, which calls EndChild without
    // a matching BeginChild when f.useColumns=true && f.headerSearch=false &&
    // search-active — fires when the user enables "Search In Sidebar" then
    // types into a 2-column section's f.sidebar search.
    if (!f.useColumns || (f.headerSearch && f.menuSearchText.length() > 0)) {
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
}

/* §34 pending-changes chip: warning count + Details popup (per-CVar revert)
 * + "Restart now". Returns the new right edge of the left footer cluster so
 * the caller can clamp the hint text against it. */
/* Action-bar hint strings, chosen by nav device: gamepad families get their
 * own glyph labels, keyboard/mouse gets Enter/Esc hints. `compactOut`
 * receives the short variant used when the long one won't fit. */
const char* Menu::FooterHints(bool padNav, bool compactLayout, const char** compactOut) {
    if (!padNav) {
        *compactOut = TR("Enter Select   Esc Close menu");
        return compactLayout ? *compactOut
                             : TR("Enter Select    Esc Close menu    Type in Search box to find settings");
    }
    switch (DetectPadFamily()) {
    case PadFamily::PlayStation:
        *compactOut = TR("[Cross] Select   [Circle] Back   [Square] Search   [Esc] Close");
        return compactLayout ? *compactOut
                             : TR("[Cross] Select    [Circle] Back    [Square] Search    [Esc] Close menu");
    case PadFamily::Nintendo:
        *compactOut = TR("[B] Select   [A] Back   [Y] Search   [Esc] Close");
        return compactLayout ? *compactOut
                             : TR("[B] Select    [A] Back    [Y] Search    [Esc] Close menu");
    default:
        *compactOut = TR("[A] Select   [B] Back   [X] Search   [Esc] Close");
        return compactLayout ? *compactOut
                             : TR("[A] Select    [B] Back    [X] Search    [Esc] Close menu");
    }
}

float Menu::DrawPendingRestartChip(ElementFrame& f, float leftClusterEndX, float hintLineY) {
    float chipX = leftClusterEndX;
    if (port_netlobby_state() == PORT_NETLOBBY_IN_MATCH) {
        chipX += f.style->ItemSpacing.x * 2;
    }
    char pendingText[96];
    std::snprintf(pendingText, sizeof(pendingText), TR("%d change(s) pending restart"), sPendingRestart);
    ImGui::SetCursorScreenPos(ImVec2(chipX, hintLineY));
    ImGui::TextColored(UIWidgets::SemanticColors::Warning, "%s", pendingText);
    float chipNextX = chipX + ImGui::CalcTextSize(pendingText).x + f.style->ItemSpacing.x * 2;
    ImGui::SetCursorScreenPos(ImVec2(chipNextX, hintLineY));
    if (ImGui::SmallButton(TR("Details"))) {
        ImGui::OpenPopup("##pending_restart_details");
    }
    chipNextX += ImGui::GetItemRectSize().x + f.style->ItemSpacing.x * 2;
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
    return ImGui::GetItemRectMax().x;
}

void Menu::DrawElementFooter(ElementFrame& f) {
    /* Footer: pinned to the bottom of the menu block regardless of how tall
     * the section content ended up. Help strip first, action hints last. */
    if (mShowHelpBar || mShowActionBar) {
        // Opaque strip so the footer reads as its own bar instead of floating
        // text that can collide with f.sidebar rows on small screens.
        f.window->DrawList->AddRectFilled(
            ImVec2(f.menuBlockPos.x, f.menuBlockPos.y + f.menuSize.y - f.footerH),
            ImVec2(f.menuBlockPos.x + f.menuSize.x, f.menuBlockPos.y + f.menuSize.y),
            ImGui::GetColorU32(ImGuiCol_WindowBg), f.style->WindowRounding);
        ImGui::SetCursorScreenPos(
            ImVec2(f.menuBlockPos.x + f.style->ItemSpacing.x * 2, f.menuBlockPos.y + f.menuSize.y - f.footerH));
        if (mShowHelpBar && !mFocusHelpName.empty() && !f.compactLayout) {
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
            const char* compactHints;
            const char* hints = FooterHints(padNav, f.compactLayout, &compactHints);
            if (!f.compactLayout) {
                ImGui::Separator();
            }
            const float hintLineY = ImGui::GetCursorScreenPos().y;
            /* Right edge of the left-aligned footer cluster (match banner
             * + pending chip). Long translations make `hints` wider than
             * the free strip and the old clamp would draw them over that
             * cluster — clamp against the real right edge instead. */
            float leftClusterEndX = f.menuBlockPos.x + f.style->ItemSpacing.x * 2;
            /* Online-match context: the menu never pauses netplay, so make
             * that explicit instead of letting the player assume otherwise.
             * Skipped in compact mode — a single footer line can't hold both
             * texts; the Online section shows its own in-match banner. */
            if (!f.compactLayout && port_netlobby_state() == PORT_NETLOBBY_IN_MATCH) {
                ImGui::SetCursorScreenPos(ImVec2(leftClusterEndX, hintLineY));
                ImGui::TextColored(UIWidgets::SemanticColors::Warning, "%s", TR("Match in progress - the game continues while this menu is open"));
                leftClusterEndX = ImGui::GetItemRectMax().x;
            }
            /* §34 pending-changes chip: left-aligned, after the match banner
             * when both are up (compact mode keeps the single hint line). */
            if (!f.compactLayout && sPendingRestart > 0) {
                leftClusterEndX = DrawPendingRestartChip(f, leftClusterEndX, hintLineY);
            }
            float hintX =
                f.menuBlockPos.x + f.menuSize.x - ImGui::CalcTextSize(hints).x - f.style->ItemSpacing.x * 2;
            if (hintX < leftClusterEndX + f.style->ItemSpacing.x && !f.compactLayout) {
                /* Long translation + occupied left cluster: switch to the
                 * compact wording before we'd collide. */
                hints = compactHints;
                hintX = f.menuBlockPos.x + f.menuSize.x - ImGui::CalcTextSize(hints).x -
                        f.style->ItemSpacing.x * 2;
            }
            ImGui::SetCursorScreenPos(
                ImVec2(ImMax(hintX, leftClusterEndX + f.style->ItemSpacing.x), hintLineY));
            ImGui::TextDisabled(TR("%s"), hints);
        }
    }
}

void Menu::DrawElement() {
    ElementFrame f;
    if (!BeginElement(f)) {
        return;
    }
    ImGuiContext& g = *GImGui;
    f.window = g.CurrentWindow;
    f.style = &ImGui::GetStyle();
    windowHeight = f.window->WorkRect.GetHeight();
    windowWidth = f.window->WorkRect.GetWidth();

    DrawElementHeader(f);
    DrawElementSections(f);
    DrawElementContent(f);
    DrawElementFooter(f);
    if (f.fontSmall != nullptr) {
        ImGui::PopFont();
    }

    if (!f.popout) {
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();
    if (f.popout) {
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
    ImGui::PopStyleColor(f.pushedColors);
}
} // namespace Ship
