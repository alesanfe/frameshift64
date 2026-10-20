// SPDX-License-Identifier: MIT
//
// Portions of this file are derived from the Ship of Harkinian (SoH) project
//   Copyright (c) The Harbour Masters
//   https://github.com/HarbourMasters/Shipwright
// Licensed under the MIT License; see LICENSE at repository root.

#include "UIWidgets.hpp"
#include "PortI18n.h"
#include "Compat.h"
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui_internal.h>
#include <libultraship/libultraship.h>
#include <string>
#include <math.h>
#include <unordered_map>
#include <libultraship/libultra/types.h>
#include <spdlog/fmt/fmt.h>

#include "UIWidgets_internal.h"

namespace UIWidgets {

int InputTextResizeCallback(ImGuiInputTextCallbackData* data) {
    std::string* value = (std::string*)data->UserData;
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        value->resize(data->BufTextLen);
        data->Buf = (char*)value->c_str();
    }
    return 0;
}

namespace {

/* Label drawn above (Left) or right-aligned-above (Right) before the
 * input box, depending on the alignment/labelPosition combo. */
void DrawInputLabel(const char* label, const std::string& value, float width,
                    const InputOptions& options) {
    ImVec2 labelSize = ImGui::CalcTextSize(label, NULL, true);
    if (labelSize.x == 0) {
        return;
    }
    if (options.alignment == ComponentAlignments::Left) {
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::Text(label, *value.c_str());
        }
    } else if (options.alignment == ComponentAlignments::Right &&
               options.labelPosition == LabelPositions::Above) {
        ImGui::NewLine();
        ImGui::SameLine(width - ImGui::CalcTextSize(label).x);
        ImGui::Text(label, *value.c_str());
    }
}

/* Hover tooltip on the finished group — error text wins over the
 * disabled tooltip, which wins over the regular one. */
void InputHoverTooltip(const InputOptions& options) {
    if (options.hasError && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !Ship_IsCStringEmpty(options.errorText)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.errorText)).c_str());
    } else if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
               !Ship_IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.disabledTooltip)).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
               !Ship_IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.tooltip)).c_str());
    }
}

} // namespace

bool InputString(const char* label, std::string* value, const InputOptions& options) {
    bool dirty = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleInput(options.color);
    if (options.hasError) {
        ImGui::PushStyleColor(ImGuiCol_Border, ColorValues.at(Colors::Red));
    }
    float width = (options.size == ImVec2(0, 0)) ? ImGui::GetContentRegionAvail().x : options.size.x;
    DrawInputLabel(label, *value, width, options);
    ImGui::SetNextItemWidth(width);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_CallbackResize;
    if (options.secret) {
        flags |= ImGuiInputTextFlags_Password;
    }
    flags |= options.addedFlags;
    if (ImGui::InputText(label, (char*)value->c_str(), value->capacity() + 1, flags, InputTextResizeCallback, value)) {
        dirty = true;
    }
    if (value->empty() && !options.placeholder.empty()) {
        ImGui::SameLine(17.0f);
        ImGui::TextColored(SemanticColors::Placeholder, "%s", options.placeholder.c_str());
    }
    if (options.hasError) {
        ImGui::PopStyleColor();
    }
    PopStyleInput();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    InputHoverTooltip(options);
    ImGui::PopID();
    return dirty;
}

bool CVarInputString(const char* label, const char* cvarName, const InputOptions& options) {
    bool dirty = false;
    std::string value = CVarGetString(cvarName, options.defaultValue.c_str());
    if (InputString(label, &value, options)) {
        CVarSetString(cvarName, value.c_str());
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ShipInit::Init(cvarName);
        dirty = true;
    }
    return dirty;
}

bool InputInt(const char* label, int32_t* value, const InputOptions& options) {
    bool dirty = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleInput(options.color);
    float width = (options.size == ImVec2(0, 0)) ? ImGui::GetContentRegionAvail().x : options.size.x;
    if (options.alignment == ComponentAlignments::Left) {
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::Text(label, *value);
        }
    } else if (options.alignment == ComponentAlignments::Right) {
        if (options.labelPosition == LabelPositions::Above) {
            ImGui::NewLine();
            ImGui::SameLine(width - ImGui::CalcTextSize(label).x);
            ImGui::Text(label, *value);
        }
    }
    ImGui::SetNextItemWidth(width);
    if (ImGui::InputScalar(label, ImGuiDataType_S32, value, nullptr, nullptr, nullptr, options.addedFlags)) {
        dirty = true;
    }
    if ((ImGui::GetItemStatusFlags() & ImGuiItemStatusFlags_Edited) && !options.placeholder.empty()) {
        ImGui::SameLine(17.0f);
        ImGui::TextColored(SemanticColors::Placeholder, "%s", options.placeholder.c_str());
    }
    PopStyleInput();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ShowWidgetTooltip(options.disabled, options.disabledTooltip, options.tooltip);
    ImGui::PopID();
    return dirty;
}

bool CVarInputInt(const char* label, const char* cvarName, const InputOptions& options) {
    bool dirty = false;
    int32_t defaultValue = options.defaultValue.empty() ? 0 : std::atoi(options.defaultValue.c_str());
    int32_t value = CVarGetInteger(cvarName, defaultValue);
    if (InputInt(label, &value, options)) {
        CVarSetInteger(cvarName, value);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ShipInit::Init(cvarName);
        dirty = true;
    }
    return dirty;
}

bool CVarColorPicker(const char* label, const char* cvarName, Color_RGBA8 defaultColor, bool hasAlpha,
                     uint8_t modifiers, UIWidgets::Colors themeColor) {
    std::string valueCVar = std::string(cvarName) + ".Value";
    std::string rainbowCVar = std::string(cvarName) + ".Rainbow";
    std::string lockedCVar = std::string(cvarName) + ".Locked";
    Color_RGBA8 color = CVarGetColor(valueCVar.c_str(), defaultColor);
    ImVec4 colorVec = ImVec4(color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f);
    bool changed = false;
    bool showReset = modifiers & ColorPickerResetButton;
    bool showRandom = modifiers & ColorPickerRandomButton;
    bool showRainbow = modifiers & ColorPickerRainbowCheck;
    bool showLock = modifiers & ColorPickerLockCheck;
    bool locked = CVarGetInteger(lockedCVar.c_str(), 0);
    ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs;
    ImGui::BeginDisabled(locked);
    PushStyleCombobox(UIWidgets::Colors::DarkGray);
    if (hasAlpha) {
        changed = ImGui::ColorEdit4(label, (float*)&colorVec,
                                    flags | ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreview);
    } else {
        changed = ImGui::ColorEdit3(label, (float*)&colorVec, flags | ImGuiColorEditFlags_NoAlpha);
    }
    PopStyleCombobox();
    ImGui::AlignTextToFramePadding();
    if (showReset) {
        ImGui::SameLine();
        std::string uniqueTag = "Reset##" + std::string(label);
        if (UIWidgets::Button(uniqueTag.c_str(),
                              UIWidgets::ButtonOptions({ { .tooltip = "Resets this color to its default value" } })
                                  .Color(themeColor)
                                  .Size(UIWidgets::Sizes::Inline))) {
            CVarClearBlock(valueCVar.c_str());
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
    }
    if (showRandom) {
        ImGui::SameLine();
        std::string uniqueTag = "Random##" + std::string(label);
        if (UIWidgets::Button(uniqueTag.c_str(),
                              UIWidgets::ButtonOptions({ { .tooltip = "Generates a random color value to use" } })
                                  .Color(themeColor)
                                  .Size(UIWidgets::Sizes::Inline))) {
            colorVec = GetRandomValue();
            color.r = static_cast<uint8_t>(fmin(fmax(colorVec.x * 255, 0), 255));
            color.g = static_cast<uint8_t>(fmin(fmax(colorVec.y * 255, 0), 255));
            color.b = static_cast<uint8_t>(fmin(fmax(colorVec.z * 255, 0), 255));
            CVarSetColor(valueCVar.c_str(), color);
            CVarSetInteger(rainbowCVar.c_str(), 0); // On click disable rainbow mode.
            ShipInit::Init(rainbowCVar);
            Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        }
    }
    if (showRainbow) {
        ImGui::SameLine();
        std::string uniqueTag = "Rainbow##" + std::string(cvarName) + "Rainbow";

        UIWidgets::CVarCheckbox(
            uniqueTag.c_str(), rainbowCVar.c_str(),
            UIWidgets::CheckboxOptions(
                { { .tooltip = "Cycles through colors on a timer\nOverwrites previously chosen color" } })
                .Color(themeColor));
    }
    ImGui::EndDisabled();
    if (showLock) {
        ImGui::SameLine();
        std::string uniqueTag = "Lock##" + std::string(cvarName) + "Locked";

        UIWidgets::CVarCheckbox(
            uniqueTag.c_str(), lockedCVar.c_str(),
            UIWidgets::CheckboxOptions({ { .tooltip = "Prevents this color from being changed" } }).Color(themeColor));
    }
    if (changed) {
        color.r = (uint8_t)(colorVec.x * 255.0f);
        color.g = (uint8_t)(colorVec.y * 255.0f);
        color.b = (uint8_t)(colorVec.z * 255.0f);
        color.a = (uint8_t)(colorVec.w * 255.0f);
        CVarSetColor(valueCVar.c_str(), color);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ShipInit::Init(valueCVar);
        changed = true;
    }

    return changed;
}

bool RadioButton(const char* label, bool active, const RadioButtonsOptions& /*options*/) {
    ImGuiWindow* window = ImGui::GetCurrentWindow();
    if (window->SkipItems)
        return false;

    ImGuiContext& g = *GImGui;
    const ImGuiStyle& style = g.Style;
    const ImGuiID id = window->GetID(label);
    const ImVec2 label_size = ImGui::CalcTextSize(label, NULL, true);

    const float square_sz = ImGui::GetFrameHeight();
    const ImVec2 pos = window->DC.CursorPos;
    const ImRect check_bb(pos, pos + ImVec2(square_sz, square_sz));
    const ImRect total_bb(
        pos, pos + ImVec2(square_sz + (label_size.x > 0.0f ? style.ItemInnerSpacing.x + label_size.x : 0.0f),
                          label_size.y + style.FramePadding.y * 2.0f));
    ImGui::ItemSize(total_bb, style.FramePadding.y);
    if (!ImGui::ItemAdd(total_bb, id))
        return false;

    ImVec2 center = check_bb.GetCenter();
    center.x = IM_ROUND(center.x);
    center.y = IM_ROUND(center.y);
    const float radius = (square_sz - 1.0f) * 0.5f;

    bool hovered, held;
    bool pressed = ImGui::ButtonBehavior(total_bb, id, &hovered, &held);
    if (pressed)
        ImGui::MarkItemEdited(id);

    ImGui::RenderNavCursor(total_bb, id);
    const int num_segment = window->DrawList->_CalcCircleAutoSegmentCount(radius);
    window->DrawList->AddCircleFilled(center, radius,
                                      ImGui::GetColorU32((held && hovered) ? ImGuiCol_FrameBgActive
                                                         : hovered         ? ImGuiCol_FrameBgHovered
                                                                           : ImGuiCol_FrameBg),
                                      num_segment);
    if (active) {
        const float pad = ImMax(1.0f, IM_TRUNC(square_sz / 6.0f));
        window->DrawList->AddCircleFilled(center, radius - pad, ImGui::GetColorU32(ImGuiCol_CheckMark));
    }

    if (style.FrameBorderSize > 0.0f) {
        window->DrawList->AddCircle(center + ImVec2(1, 1), radius, ImGui::GetColorU32(ImGuiCol_BorderShadow),
                                    num_segment, style.FrameBorderSize);
        window->DrawList->AddCircle(center, radius, ImGui::GetColorU32(ImGuiCol_Border), num_segment,
                                    style.FrameBorderSize);
    }

    ImVec2 label_pos = ImVec2(check_bb.Max.x + style.ItemInnerSpacing.x, check_bb.Min.y + style.FramePadding.y);
    if (g.LogEnabled)
        ImGui::LogRenderedText(&label_pos, active ? "(x)" : "( )");
    if (label_size.x > 0.0f)
        RenderText(label_pos, label, ImGui::FindRenderedTextEnd(label), true);

    IMGUI_TEST_ENGINE_ITEM_INFO(id, label, g.LastItemData.StatusFlags);
    return pressed;
}

bool CVarRadioButton(const char* text, const char* cvarName, int32_t id, const RadioButtonsOptions& options) {
    std::string make_invisible = "##" + std::string(text) + std::string(cvarName);

    bool ret = false;
    int val = CVarGetInteger(cvarName, options.defaultIndex);
    PushStyleCheckbox(options.color);
    if (ImGui::RadioButton(make_invisible.c_str(), id == val)) {
        CVarSetInteger(cvarName, id);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ret = true;
    }
    ImGui::SameLine();
    ImGui::Text("%s", text);
    PopStyleCheckbox();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !Ship_IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.tooltip)).c_str());
    }

    return ret;
}

std::map<std::string, int32_t> buttonMap = {
    { "A", BTN_A },
    { "B", BTN_B },
    { "Z", BTN_Z },
    { "START", BTN_START },
    { "D-Up", BTN_DUP },
    { "D-Down", BTN_DDOWN },
    { "D-Left", BTN_DLEFT },
    { "D-Right", BTN_DRIGHT },
    { "L", BTN_L },
    { "R", BTN_R },
    { "C-Up", BTN_CUP },
    { "C-Down", BTN_CDOWN },
    { "C-Left", BTN_CLEFT },
    { "C-Right", BTN_CRIGHT },
    { "Modifier 1", BTN_CUSTOM_MODIFIER1 },
    { "Modifier 2", BTN_CUSTOM_MODIFIER2 },
};

bool BtnSelector(const char* label, int32_t* value, const BtnSelectorOptions& options) {
    bool dirty = false;
    ImGui::PushID(label);
    ImGui::BeginGroup();
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%s", label);
    ImGui::BeginDisabled(false);
    PushStyleCombobox(options.color);
    ImGui::BeginChild("ButtonCombo", ImVec2(0, ImGui::GetFrameHeightWithSpacing() + 14.0f), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    int32_t currentValue = *value;
    int index = 0;
    for (const auto& [buttonName, buttonMask] : buttonMap) {
        if (currentValue & buttonMask) {
            ImGui::PushID(buttonName.c_str());
            if (index++ > 0) {
                ImGui::Text("+");
                ImGui::SameLine();
            }
            if (UIWidgets::Button(buttonName.c_str(), UIWidgets::ButtonOptions()
                                                          .Tooltip("Remove this button from the combination")
                                                          .Color(UIWidgets::Colors::Gray)
                                                          .Size(UIWidgets::Sizes::Inline))) {
                currentValue &= ~buttonMask;
                dirty = true;
            }
            ImGui::PopID();
            ImGui::SameLine();
        }
    }
    if (UIWidgets::Button("+", UIWidgets::ButtonOptions({ { .tooltip = TR("Add a button to the combination") } })
                                   .Size(UIWidgets::Sizes::Inline)
                                   .Color(options.color))) {
        ImGui::OpenPopup("Add Button");
    }
    if (ImGui::BeginPopup("Add Button")) {
        UIWidgets::PushStyleMenuItem();
        for (const auto& [buttonName, buttonMask] : buttonMap) {
            if (!(currentValue & buttonMask)) {
                if (ImGui::MenuItem(buttonName.c_str())) {
                    currentValue |= buttonMask;
                    dirty = true;
                }
            }
        }
        UIWidgets::PopStyleMenuItem();
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    if (UIWidgets::Button(ICON_FA_UNDO,
                          UIWidgets::ButtonOptions().Size(UIWidgets::Sizes::Inline).Color(options.color))) {
        currentValue = options.defaultValue;
        dirty = true;
    }
    ImGui::EndChild();
    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    ImGui::PopID();
    if (dirty) {
        *value = currentValue;
    }
    return dirty;
}

bool CVarBtnSelector(const char* label, const char* cvarName, const BtnSelectorOptions& options) {
    bool dirty = false;

    int32_t value = CVarGetInteger(cvarName, options.defaultValue);
    if (BtnSelector(label, &value, options)) {
        CVarSetInteger(cvarName, value);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ShipInit::Init(cvarName);
        dirty = true;
    }

    return dirty;
}
} // namespace UIWidgets

ImVec4 GetRandomValue(uint64_t* state) {
    ImVec4 NewColor;
    NewColor.x = (float)ShipUtils::RandomDouble(state);
    NewColor.y = (float)ShipUtils::RandomDouble(state);
    NewColor.z = (float)ShipUtils::RandomDouble(state);
    return NewColor;
}

Color_RGBA8 RGBA8FromVec(ImVec4 vec) {
    Color_RGBA8 color = { static_cast<uint8_t>(vec.x * 255), static_cast<uint8_t>(vec.y * 255),
                          static_cast<uint8_t>(vec.z * 255), static_cast<uint8_t>(vec.w * 255) };
    return color;
}

ImVec4 VecFromRGBA8(Color_RGBA8 color) {
    ImVec4 vec = { color.r / 255.0f, color.g / 255.0f, color.b / 255.0f, color.a / 255.0f };
    return vec;
}
