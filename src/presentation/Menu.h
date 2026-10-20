/*
 * SPDX-License-Identifier: MIT
 *
 * Portions of this file are derived from the Ship of Harkinian (SoH) project
 *   Copyright (c) The Harbour Masters
 *   https://github.com/HarbourMasters/Shipwright
 * Licensed under the MIT License; see LICENSE at repository root.
 */

#ifndef MENU_H
#define MENU_H

#include <libultraship/libultraship.h>
#include <fast/backends/gfx_rendering_api.h>
#include "MenuTypes.h"

namespace Ship {
uint32_t GetVectorIndexOf(std::vector<std::string>& vector, std::string value);
class Menu : public GuiWindow {
  public:
    using GuiWindow::GuiWindow;

    Menu(const std::string& cVar, const std::string& name, uint8_t searchSidebarIndex_ = 0,
         UIWidgets::Colors menuThemeIndex_ = UIWidgets::Colors::LightBlue);

    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override;
    void Draw() override;
    void InsertSidebarSearch();
    void RemoveSidebarSearch();
    void UpdateWindowBackendObjects();
    bool IsMenuPopped();
    UIWidgets::Colors GetMenuThemeColor();

    void MenuDrawItem(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex);
    void AddMenuEntry(std::string entryName, const char* entryCvar);
    void AddSearchWidget(SearchWidget widget);
    void ClearAllEntries();
    std::unordered_map<uint32_t, disabledInfo>& GetDisabledMap();

  protected:
    /* Header-action flags — subclasses turn these off when they provide the
     * same action as a regular menu entry (e.g. a Quit row in a Support
     * sidebar) instead of a permanent header button. */
    bool mShowHeaderQuit = true;
    bool mShowHeaderReset = true;
    /* Bottom bar with device-appropriate action hints (gamepad buttons when
     * controller nav is enabled, keyboard otherwise). */
    bool mShowActionBar = false;
    /* Persistent help strip above the action bar showing the focused /
     * hovered widget's tooltip — tooltips alone don't work with gamepad or
     * touch, so the description needs a fixed home. */
    bool mShowHelpBar = false;
    std::string mFocusHelpName;
    std::string mFocusHelpText;

    ImVec2 mOriginalSize;
    std::string mName;
    uint32_t mWindowFlags;
    std::unordered_map<std::string, MainMenuEntry> menuEntries;
    std::vector<std::string> menuOrder;
    uint32_t DrawSearchResults(std::string& menuSearchText);
    void DrawSearchHit(WidgetInfo& info, const std::string& origin,
                       const std::string& entryName, const std::string& sidebarName);
    uint32_t DrawSearchResultWidgets(const std::string& menuSearchText);
    uint32_t DrawSearchResultExtras(const std::string& menuSearchText);
    uint32_t DrawSearchActionEntries(const std::string& menuSearchText);
    ImGuiTextFilter menuSearch;
    uint8_t searchSidebarIndex;
    UIWidgets::Colors defaultThemeIndex;
    std::shared_ptr<std::vector<Ship::WindowBackend>> availableWindowBackends;
    std::map<Ship::WindowBackend, const char*> availableWindowBackendsMap;
    Ship::WindowBackend configWindowBackend;

    std::unordered_map<uint32_t, disabledInfo> disabledMap;
    std::vector<disabledInfo> disabledVector;
    const SidebarEntry searchSidebarEntry = {
        .columnCount = 1,
        .columnWidgets = { { { .name = "Sidebar Search",
                               .type = WIDGET_SEARCH,
                               .options = std::make_shared<UIWidgets::WidgetOptions>(UIWidgets::WidgetOptions{}.Tooltip(
                                   "Searches all menus for the given text, including tooltips.")) } } }
    };

  private:
    bool allowPopout = true; // PortNote: should be set to false on small screen ports
    bool popped;
    ImVec2 poppedSize;
    ImVec2 poppedPos;
    float windowHeight;
    float windowWidth;
    float mLastUiScale = 1.0f;
    UIWidgets::Colors menuThemeIndex;

    /* DrawElement phases — per-frame outputs handed from the setup half
     * (BeginElement) to the render half. */
    struct ElementFrame {
        ImFont* fontLarge = nullptr;
        ImFont* fontSmall = nullptr;
        bool popout = false;
        int pushedColors = 0;
        ImGuiWindow* window = nullptr;
        ImGuiStyle* style = nullptr;
        std::string headerIndex;
        ImVec2 pos{};
        ImVec2 menuSize{};
        ImVec2 menuBlockPos{};
        float headerHeight = 0.0f;
        float headerRowH = 0.0f;
        ImVec2 buttonSize{};
        bool headerSearch = false;
        std::unordered_map<std::string, SidebarEntry>* sidebar = nullptr;
        bool compactLayout = false;
        float footerH = 0.0f;
        float columnHeight = 0.0f;
        float sectionHeight = 0.0f;
        float sidebarWidth = 0.0f;
        std::string sectionIndex;
        std::string menuSearchText;
        std::string sectionMenuId;
        size_t columns = 0;
        size_t columnFuncs = 0;
        bool useColumns = false;
        float sectionWidth = 0.0f;
    };
    /* Fonts, pending widget navigation, UI scale, popout geometry and the
     * theme palette, ending at ImGui::Begin. Returns false when the frame
     * ends early (window collapsed, or the popout toggle persisted and the
     * window needs a rebuild) — pushed style state is balanced inside. */
    void SyncFrameEnvironment();
    void ApplyPopoutTransition(ElementFrame& f, bool canPopout, ImGuiWindowFlags& windowFlags);
    void ResolveMenuFonts(ElementFrame& f);
    void UpdatePendingNavigation();
    void PushMenuThemeColors(ElementFrame& f);
    void PersistPopoutGeometry(ElementFrame& f);
    bool BeginElement(ElementFrame& f);
    void DrawElementHeader(ElementFrame& f);
    void DrawHeaderActions(ElementFrame& f);
    void DrawHeaderTabStrip(ElementFrame& f, const char* headerCvar);
    void DrawHeaderSearchField(ElementFrame& f, bool autoFocus);
    void DrawElementSections(ElementFrame& f);
    std::string ResolveSectionIndex(ElementFrame& f);
    void DrawCompactSidebarStrip(ElementFrame& f);
    void DrawVerticalSidebarList(ElementFrame& f);
    void DrawElementContent(ElementFrame& f);
    void DrawSearchPane(ElementFrame& f);
    void DrawSectionColumns(ElementFrame& f, float columnWidth);
    void DrawSectionRestoreRow(ElementFrame& f);
    void DrawElementFooter(ElementFrame& f);
    void DrawFooterHelpText();
    void DrawFooterActionBar(ElementFrame& f);
    float DrawPendingRestartChip(ElementFrame& f, float leftClusterEndX, float hintLineY);
    const char* FooterHints(bool padNav, bool compactLayout, const char** compactOut);

    /* MenuDrawItem phases. Bail aborts before the shared epilogue (the old
     * bare `return` paths), NotMine forwards the dispatch to DrawWidgetBody. */
    enum class WidgetDrawResult { NotMine, Done, Bail };
    WidgetDrawResult DrawMiscWidget(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex);
    bool DrawBackendSelector(WidgetInfo& widget, UIWidgets::Colors themeIndex);
    void DrawWindowButtonWidget(WidgetInfo& widget, UIWidgets::Colors themeIndex);
    bool DrawWidgetBody(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex);
    WidgetDrawResult DrawPointerWidget(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex);
    WidgetDrawResult DrawCVarWidget(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex);
    void DrawDisabledRecoveryAction(WidgetInfo& widget);
    void DrawItemEpilogue(WidgetInfo& widget);
    bool DrawSearchWidget(UIWidgets::Colors themeIndex);
};
} // namespace Ship

#endif // MENU_H
