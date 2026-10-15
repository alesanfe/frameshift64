/*
 * SPDX-License-Identifier: MIT
 *
 * Portions of this file are derived from the Ship of Harkinian (SoH) project
 *   Copyright (c) The Harbour Masters
 *   https://github.com/HarbourMasters/Shipwright
 * Licensed under the MIT License; see LICENSE at repository root.
 */

#pragma once

#include "Menu.h"
#include "MenuTypes.h"

#include <chrono>
#include <map>

namespace ssb64 {

class PortMenu : public Ship::Menu {
  public:
    PortMenu();
    ~PortMenu() override = default;

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override;
    void Draw() override;

  protected:
    void AddSidebarEntry(std::string sectionName, std::string sidbarName, uint32_t columnCount);
    WidgetInfo& AddWidget(WidgetPath& pathInfo, std::string widgetName, WidgetType widgetType);
    void AddMenuElements();
    void AddMenuPlay();
    void AddMenuSettings();
    void AddMenuCustomization();
    void AddMenuAssets();
    void AddMenuSupport();
    void AddMenuAbout();
    void AddMenuOnline();
    /* Clears and rebuilds every menu entry — used when a toggle changes which
     * sidebars exist (e.g. enabling developer tools). */
    void RebuildMenu();

  private:
    bool mShowReextractMessage = false;
    bool mShowQuitConfirm = false;
    bool mMenuElementsInitialized = false;
    /* Display-change auto-revert ("Keep these settings?" countdown modal).
     * Armed when the user toggles fullscreen; on timeout the change is
     * reverted so a black/incompatible display mode can't strand them —
     * the timeout also fires with the menu closed (Draw runs per-frame). */
    bool mDisplayRevertArmed = false;
    std::chrono::steady_clock::time_point mDisplayRevertDeadline{};
};

} // namespace ssb64
