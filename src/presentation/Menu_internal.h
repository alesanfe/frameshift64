// Shared between Menu.cpp, Menu_widgets.cpp and Menu_frame.cpp — file-
// scope helpers that used to live inside the single TU. Internal to the
// presentation module; not part of the Menu public API.
#pragma once
#include <string>
#include <vector>
#include <imgui.h>
#include "MenuTypes.h"   // SidebarEntry, WidgetType, Color_RGBA8

namespace Ship {

/* §5 — pad hints follow the connected controller's family, since button
 * letters sit at different physical positions (FaceDown = accept is A on
 * Xbox, B on Nintendo pads, Cross on PlayStation). */
enum class PadFamily { Xbox, PlayStation, Nintendo };
PadFamily DetectPadFamily();

enum class MenuBaseTheme { Dark = 0, Light = 1, HighContrast = 2 };
int    PushMenuBasePalette(MenuBaseTheme theme);
ImVec4 MenuWindowBgColor(MenuBaseTheme theme, float alpha);
ImVec4 MenuTextDisabledColor(MenuBaseTheme theme);

struct SectionUndoEntry {
    std::string cvar;
    WidgetType type = {};
    int32_t i = 0;
    float f = 0.0f;
    std::string s;
    Color_RGBA8 c = { 255, 255, 255, 255 };
};
struct SectionUndo {
    std::string sidebar;
    std::vector<SectionUndoEntry> entries;
};
extern SectionUndo sSectionUndo;
void   SnapshotSectionCvars(const SidebarEntry& section, SectionUndo& undo);
void   ApplySectionUndo(SectionUndo& undo);
size_t ResetSectionCvars(const SidebarEntry& section, bool apply);

extern int  sHighlightTtl;   // frames left to find the navigation target
extern bool raceDisableActive;

/* Navigation + disabled-widget state shared by the widget-draw and
 * frame-draw TUs. Definitions live in Menu.cpp. */
extern std::string disabledTempTooltip;
extern const char*  disabledTooltip;
extern bool         disabledValue;
extern bool         navigateToWidget;
extern std::string  navigateMainEntry;
extern std::string  navigateSidebar;
extern std::string  navigateWidgetName;
extern bool         highlightWidget;

bool SimpleMode();   // defined in Menu_frame.cpp

ImFont* GetMenuFont(const std::string& name);
bool ModernMenuSidebarEntry(std::string label);
bool ModernMenuHeaderEntry(std::string label);

} // namespace Ship
