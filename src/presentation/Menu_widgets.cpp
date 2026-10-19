// Widget draw dispatch (MenuDrawItem family) — extracted from Menu.cpp.

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

namespace Ship {
static inline void FireWidgetCallback(WidgetInfo& widget) {
    if (widget.callback != nullptr) {
        widget.callback(widget);
    }
}

Menu::WidgetDrawResult Menu::DrawMiscWidget(WidgetInfo& widget, uint32_t /*width*/,
                                                    UIWidgets::Colors themeIndex) {
    switch (widget.type) {
            case WIDGET_AUDIO_BACKEND:
            case WIDGET_VIDEO_BACKEND: {
                if (!DrawBackendSelector(widget, themeIndex)) {
                    return WidgetDrawResult::NotMine;
                }
            } return WidgetDrawResult::Done;
            case WIDGET_SEPARATOR: {
                ImGui::Separator();
            } return WidgetDrawResult::Done;
            case WIDGET_SEPARATOR_TEXT: {
                auto options = std::static_pointer_cast<UIWidgets::TextOptions>(widget.options);
                if (options->color != UIWidgets::Colors::NoColor) {
                    ImGui::PushStyleColor(ImGuiCol_Text, UIWidgets::ColorValues.at(options->color));
                }
                ImGui::SeparatorText(TR(widget.name.c_str()));
                if (options->color != UIWidgets::Colors::NoColor) {
                    ImGui::PopStyleColor();
                }
            } return WidgetDrawResult::Done;
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
            } return WidgetDrawResult::Done;
            case WIDGET_BUTTON: {
                auto options = std::static_pointer_cast<UIWidgets::ButtonOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::Button(TR(widget.name.c_str()), *options)) {
                    FireWidgetCallback(widget);
                }
            } return WidgetDrawResult::Done;
            case WIDGET_CUSTOM: {
                if (widget.customFunction != nullptr) {
                    widget.customFunction(widget);
                }
            } return WidgetDrawResult::Done;
            case WIDGET_WINDOW_BUTTON: {
                DrawWindowButtonWidget(widget, themeIndex);
            } return WidgetDrawResult::Done;
            case WIDGET_SEARCH: {
                if (!DrawSearchWidget(themeIndex)) {
                    return WidgetDrawResult::Bail;
                }
            } return WidgetDrawResult::Done;
        default:
            return WidgetDrawResult::NotMine;
    }
}

/* Value-pointer and CVar-bound widget cases. Returns false when the
 * widget cannot be drawn (null value pointer) — the caller skips
 * the shared epilogue, matching the old bare `return`. */
/* Value-pointer widgets (the pointer may be null -> Bail, matching
 * the old early-return). */
Menu::WidgetDrawResult Menu::DrawPointerWidget(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex) {
    switch (widget.type) {
            case WIDGET_CHECKBOX: {
                bool* pointer = std::get<bool*>(widget.valuePointer);
                if (pointer == nullptr) {
                    SPDLOG_ERROR("Checkbox Widget requires a value pointer, currently nullptr");
                    assert(false);
                    return WidgetDrawResult::Bail;
                }
                auto options = std::static_pointer_cast<UIWidgets::CheckboxOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::Checkbox(UIWidgets::WrappedText(TR(widget.name.c_str()), width).c_str(), pointer,
                                        *options)) {
                    FireWidgetCallback(widget);
                }
            } return WidgetDrawResult::Done;
            case WIDGET_COMBOBOX: {
                int32_t* pointer = std::get<int32_t*>(widget.valuePointer);
                if (pointer == nullptr) {
                    SPDLOG_ERROR("Combobox Widget requires a value pointer, currently nullptr");
                    assert(false);
                    return WidgetDrawResult::Bail;
                }
                auto options = std::static_pointer_cast<UIWidgets::ComboboxOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::Combobox(TR(widget.name.c_str()), pointer, options->comboMap, *options)) {
                    FireWidgetCallback(widget);
                };
            } return WidgetDrawResult::Done;
            case WIDGET_SLIDER_INT: {
                int32_t* pointer = std::get<int32_t*>(widget.valuePointer);
                if (pointer == nullptr) {
                    SPDLOG_ERROR("int32 Slider Widget requires a value pointer, currently nullptr");
                    assert(false);
                    return WidgetDrawResult::Bail;
                }
                auto options = std::static_pointer_cast<UIWidgets::IntSliderOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::SliderInt(TR(widget.name.c_str()), pointer, *options)) {
                    FireWidgetCallback(widget);
                };
            } return WidgetDrawResult::Done;
            case WIDGET_SLIDER_FLOAT: {
                float* pointer = std::get<float*>(widget.valuePointer);

                if (pointer == nullptr) {
                    SPDLOG_ERROR("float Slider Widget requires a value pointer, currently nullptr");
                    assert(false);
                    return WidgetDrawResult::Bail;
                }
                auto options = std::static_pointer_cast<UIWidgets::FloatSliderOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::SliderFloat(TR(widget.name.c_str()), pointer, *options)) {
                    FireWidgetCallback(widget);
                }
            } return WidgetDrawResult::Done;
        default:
            return WidgetDrawResult::NotMine;
    }
}

/* CVar-bound widgets - always draw; callback runs on change. */
Menu::WidgetDrawResult Menu::DrawCVarWidget(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex) {
    switch (widget.type) {
            case WIDGET_CVAR_CHECKBOX: {
                auto options = std::static_pointer_cast<UIWidgets::CheckboxOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarCheckbox(UIWidgets::WrappedText(TR(widget.name.c_str()), width).c_str(), widget.cVar,
                                            *options)) {
                    FireWidgetCallback(widget);
                };
            } return WidgetDrawResult::Done;
            case WIDGET_CVAR_COMBOBOX: {
                auto options = std::static_pointer_cast<UIWidgets::ComboboxOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarCombobox(TR(widget.name.c_str()), widget.cVar, options->comboMap, *options)) {
                    FireWidgetCallback(widget);
                }
            } return WidgetDrawResult::Done;
            case WIDGET_CVAR_SLIDER_INT: {
                auto options = std::static_pointer_cast<UIWidgets::IntSliderOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarSliderInt(TR(widget.name.c_str()), widget.cVar, *options)) {
                    FireWidgetCallback(widget);
                };
            } return WidgetDrawResult::Done;
            case WIDGET_CVAR_SLIDER_FLOAT: {
                auto options = std::static_pointer_cast<UIWidgets::FloatSliderOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarSliderFloat(TR(widget.name.c_str()), widget.cVar, *options)) {
                    FireWidgetCallback(widget);
                }
            } return WidgetDrawResult::Done;
            case WIDGET_CVAR_BTN_SELECTOR: {
                auto options = std::static_pointer_cast<UIWidgets::BtnSelectorOptions>(widget.options);
                options->color = themeIndex;
                if (UIWidgets::CVarBtnSelector(TR(widget.name.c_str()), widget.cVar, *options)) {
                    FireWidgetCallback(widget);
                }
            } return WidgetDrawResult::Done;
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
            } return WidgetDrawResult::Done;
        default:
            return WidgetDrawResult::NotMine;
    }
}

bool Menu::DrawWidgetBody(WidgetInfo& widget, uint32_t width, UIWidgets::Colors themeIndex) {
    const auto r = DrawPointerWidget(widget, width, themeIndex);
    if (r != WidgetDrawResult::NotMine) {
        return r == WidgetDrawResult::Done;
    }
    return DrawCVarWidget(widget, width, themeIndex) != WidgetDrawResult::Bail;
}

/* Shared widget epilogue: focus-help capture, narration announce,
 * trailing tag, disabled-recovery action, persistent description,
 * postFunc hook. */
/* Recovery action for a disabled control: the widget-level action wins,
 * otherwise the first activeDisables reason that carries one. */
void Menu::DrawDisabledRecoveryAction(WidgetInfo& widget) {
    if (!widget.options->disabled) {
        return;
    }
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
        if (ImGui::SmallButton(TR(actionLabel))) {
            action();
        }
    }
}

void Menu::DrawItemEpilogue(WidgetInfo& widget) {
    /* Focus help capture — the widget just drawn is the "last item", so
     * reading hover/focus here maps to this control. The focused widget's
     * name + tooltip feed the persistent help bar at the bottom of the
     * menu (gamepad/touch users never see hover tooltips). */
    if (mShowHelpBar &&
        (ImGui::IsItemFocused() || ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))) {
        mFocusHelpName = widget.name;
        const char* tip = TR(widget.options->tooltip);
        if (widget.options->disabled && !Ship_IsCStringEmpty(widget.options->disabledTooltip)) {
            tip = TR(widget.options->disabledTooltip);
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
    DrawDisabledRecoveryAction(widget);
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
                disabledTempTooltip += std::string("\n- ") + TR(disabledMap.at(option).reason);
            }
            widget.options->disabledTooltip = disabledTempTooltip.c_str();
        }
    }
    if (widget.raceDisable && raceDisableActive) {
        widget.options->disabled = true;
        disabledTempTooltip += std::string("\n- ") + TR("Race Lockout Active");
        widget.options->disabledTooltip = disabledTempTooltip.c_str();
    }

    if (widget.sameLine) {
        ImGui::SameLine();
    }

    try {
        const auto misc = DrawMiscWidget(widget, width, themeIndex);
        if (misc == WidgetDrawResult::Bail) {
            return;
        }
        if (misc == WidgetDrawResult::NotMine && !DrawWidgetBody(widget, width, themeIndex)) {
            return;
        }
        DrawItemEpilogue(widget);
    } catch (const std::bad_variant_access& e) {
        SPDLOG_ERROR("Failed to draw menu item \"{}\" due to: {}", widget.name, e.what());
        assert(false);
    }
}

} // namespace Ship
