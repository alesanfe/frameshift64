/* Template widget implementations — included from UIWidgets.hpp so
 * existing `#include "UIWidgets.hpp"` keeps working. Not meant to be
 * included directly. */
#ifndef UIWidgetsTemplates_hpp
#define UIWidgetsTemplates_hpp

#include <map>
#include <string>
#include <vector>
#include <stdint.h>
#include <imgui.h>
#include <libultraship/libultraship.h>
#include "Compat.h"
#include "PortI18n.h"

namespace UIWidgets {

template <typename T>
bool Combobox(std::string label, T* value, const std::map<T, const char*>& comboMap,
              const ComboboxOptions& options = {}) {
    bool dirty = false;
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    std::string trueLabel = label.substr(0, label.find("#"));
    ImGui::PushID(label.c_str());
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleCombobox(options.color);

    const char* longest = "";
    size_t length = 0;
    for (auto& [index, string] : comboMap) {
        const char* tr = TR(string);
        size_t len = strlen(tr);
        if (len > length) {
            longest = tr;
            length = len;
        }
    }
    float comboWidth = CalcComboWidth(longest, options.flags);

    ImGui::AlignTextToFramePadding();
    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Right) {
            ImGui::Text("%s", trueLabel.c_str());
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            } else if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
            } else if (options.labelPosition == LabelPositions::Far) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            }
        } else if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", trueLabel.c_str());
            }
        }
    }

    ImGui::SetNextItemWidth(comboWidth);
    /* The stored key can be absent from the map (empty dynamic maps like the
     * profiles picker, or a stale CVar) — at() throws std::out_of_range mid-
     * frame and the aborted frame never reaches EndFrame. */
    const auto previewIt = comboMap.find(*value);
    const char* previewLabel = previewIt != comboMap.end() ? previewIt->second : "";
    if (ImGui::BeginCombo(invisibleLabel, TR(previewLabel), options.flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
        for (const auto& pair : comboMap) {
            if (strlen(pair.second) > 1) {
                if (ImGui::Selectable(TR(pair.second), pair.first == *value)) {
                    *value = pair.first;
                    dirty = true;
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
                ImGui::Text("%s", trueLabel.c_str());
            } else if (options.labelPosition == LabelPositions::Far) {
                float width = ImGui::CalcTextSize(TR(previewLabel)).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
                ImGui::Text("%s", trueLabel.c_str());
            }
        }
    }
    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !Ship_IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.disabledTooltip)).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !Ship_IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.tooltip)).c_str());
    }
    ImGui::PopID();
    return dirty;
}

template <typename T = size_t>
bool Combobox(std::string label, T* value, const std::vector<const char*>& comboVector,
              const ComboboxOptions& options = {}) {
    bool dirty = false;
    /* A persisted CVar can hold an index past the end (config hand-edited or
     * the list shrank): clamp before at()/[] below or it throws out_of_range
     * inside the frame. */
    size_t currentValueIndex =
        (!comboVector.empty() && static_cast<size_t>(*value) < comboVector.size())
            ? static_cast<size_t>(*value)
            : 0;
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    std::string trueLabel = label.substr(0, label.find("#"));
    ImGui::PushID(label.c_str());
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleCombobox(options.color);

    const char* longest = "";
    size_t length = 0;
    for (auto& string : comboVector) {
        size_t len = strlen(string);
        if (len > length) {
            longest = string;
            length = len;
        }
    }
    float comboWidth = CalcComboWidth(longest, options.flags);

    ImGui::AlignTextToFramePadding();
    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Right) {
            ImGui::Text("%s", trueLabel.c_str());
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            } else if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
            } else if (options.labelPosition == LabelPositions::Far) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            }
        } else if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", trueLabel.c_str());
            }
        }
    }

    ImGui::SetNextItemWidth(comboWidth);
    const char* previewLabel = comboVector.empty() ? "" : comboVector.at(currentValueIndex);
    if (ImGui::BeginCombo(invisibleLabel, TR(previewLabel), options.flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
        for (size_t i = 0; i < comboVector.size(); ++i) {
            auto newValue = static_cast<T>(i);
            if (strlen(comboVector.at(i)) > 1) {
                if (ImGui::Selectable(TR(comboVector.at(i)), newValue == *value)) {
                    *value = newValue;
                    dirty = true;
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
                ImGui::Text("%s", trueLabel.c_str());
            } else if (options.labelPosition == LabelPositions::Far) {
                float width = ImGui::CalcTextSize(TR(comboVector.at(*value))).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
                ImGui::Text("%s", trueLabel.c_str());
            }
        }
    }

    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !Ship_IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.disabledTooltip)).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !Ship_IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.tooltip)).c_str());
    }
    ImGui::PopID();
    return dirty;
}

template <typename T = size_t>
bool Combobox(std::string label, T* value, const std::vector<std::string>& comboVector,
              const ComboboxOptions& options = {}) {
    bool dirty = false;
    size_t currentValueIndex =
        (!comboVector.empty() && static_cast<size_t>(*value) < comboVector.size())
            ? static_cast<size_t>(*value)
            : 0;
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    std::string trueLabel = label.substr(0, label.find("#"));
    ImGui::PushID(label.c_str());
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleCombobox(options.color);

    const char* longest = "";
    size_t length = 0;
    for (auto& string : comboVector) {
        size_t len = string.length();
        if (len > length) {
            longest = string.c_str();
            length = len;
        }
    }
    float comboWidth = CalcComboWidth(longest, options.flags);

    ImGui::AlignTextToFramePadding();
    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Right) {
            ImGui::Text("%s", trueLabel.c_str());
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            } else if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
            } else if (options.labelPosition == LabelPositions::Far) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            }
        } else if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", trueLabel.c_str());
            }
        }
    }

    ImGui::SetNextItemWidth(comboWidth);
    const char* previewLabel = comboVector.empty() ? "" : comboVector.at(currentValueIndex).c_str();
    if (ImGui::BeginCombo(invisibleLabel, TR(previewLabel), options.flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
        for (size_t i = 0; i < comboVector.size(); ++i) {
            auto newValue = static_cast<T>(i);
            if (comboVector.at(i).length() > 1) {
                if (ImGui::Selectable(TR(comboVector.at(i).c_str()), newValue == *value)) {
                    *value = newValue;
                    dirty = true;
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
                ImGui::Text("%s", trueLabel.c_str());
            } else if (options.labelPosition == LabelPositions::Far) {
                float width =
                    ImGui::CalcTextSize(TR(comboVector.at(*value).c_str())).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
                ImGui::Text("%s", trueLabel.c_str());
            }
        }
    }

    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !Ship_IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.disabledTooltip)).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !Ship_IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.tooltip)).c_str());
    }
    ImGui::PopID();
    return dirty;
}

template <typename T = size_t, size_t N>
bool Combobox(std::string label, T* value, const char* (&comboArray)[N], const ComboboxOptions& options = {}) {
    bool dirty = false;
    size_t currentValueIndex = static_cast<size_t>(*value);
    if (currentValueIndex >= N) {
        currentValueIndex = 0;
    }
    std::string invisibleLabelStr = "##" + std::string(label);
    const char* invisibleLabel = invisibleLabelStr.c_str();
    std::string trueLabel = label.substr(0, label.find("#"));
    ImGui::PushID(label.c_str());
    ImGui::BeginGroup();
    ImGui::BeginDisabled(options.disabled);
    PushStyleCombobox(options.color);

    const char* longest = "";   // N == 0 or all-empty strings: stay defined
    size_t length = 0;
    for (size_t i = 0; i < N; i++) {
        size_t len = strlen(comboArray[i]);
        if (len > length) {
            longest = comboArray[i];
            length = len;
        }
    }
    float comboWidth = CalcComboWidth(longest, options.flags);

    ImGui::AlignTextToFramePadding();
    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Right) {
            ImGui::Text("%s", trueLabel.c_str());
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::NewLine();
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            } else if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
            } else if (options.labelPosition == LabelPositions::Far) {
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - comboWidth);
            }
        } else if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Above) {
                ImGui::Text("%s", trueLabel.c_str());
            }
        }
    }

    ImGui::SetNextItemWidth(comboWidth);
    if (ImGui::BeginCombo(invisibleLabel, TR(comboArray[currentValueIndex]), options.flags)) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10.0f, 10.0f));
        for (size_t i = 0; i < N; ++i) {
            auto newValue = static_cast<T>(i);
            if (strlen(comboArray[i]) > 1) {
                if (ImGui::Selectable(TR(comboArray[i]), newValue == *value)) {
                    *value = newValue;
                    dirty = true;
                }
            }
        }
        ImGui::PopStyleVar();
        ImGui::EndCombo();
    }

    if (options.labelPosition != LabelPositions::None) {
        if (options.alignment == ComponentAlignments::Left) {
            if (options.labelPosition == LabelPositions::Near) {
                ImGui::SameLine();
                ImGui::Text("%s", trueLabel.c_str());
            } else if (options.labelPosition == LabelPositions::Far) {
                float width = ImGui::CalcTextSize(TR(comboArray[*value])).x + ImGui::GetStyle().FramePadding.x * 2;
                ImGui::SameLine(ImGui::GetContentRegionAvail().x - width);
                ImGui::Text("%s", trueLabel.c_str());
            }
        }
    }
    PopStyleCombobox();
    ImGui::EndDisabled();
    ImGui::EndGroup();
    if (options.disabled && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) &&
        !Ship_IsCStringEmpty(options.disabledTooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.disabledTooltip)).c_str());
    } else if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !Ship_IsCStringEmpty(options.tooltip)) {
        ImGui::SetTooltip("%s", WrappedText(TR(options.tooltip)).c_str());
    }
    ImGui::PopID();
    return dirty;
}

template <typename T = int32_t>
bool CVarCombobox(const char* label, const char* cvarName, const std::map<T, const char*>& comboMap,
                  const ComboboxOptions& options = {}) {
    bool dirty = false;
    int32_t value = CVarGetInteger(cvarName, options.defaultIndex);
    if (Combobox<T>(label, &value, comboMap, options)) {
        CVarSetInteger(cvarName, value);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ShipInit::Init(cvarName);
        dirty = true;
    }
    return dirty;
}

template <typename T = int32_t>
bool CVarCombobox(const char* label, const char* cvarName, const std::vector<const char*>& comboVector,
                  const ComboboxOptions& options = {}) {
    bool dirty = false;
    int32_t value = CVarGetInteger(cvarName, options.defaultIndex);
    if (Combobox<T>(label, &value, comboVector, options)) {
        CVarSetInteger(cvarName, value);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ShipInit::Init(cvarName);
        dirty = true;
    }
    return dirty;
}

template <typename T = int32_t, size_t N>
bool CVarCombobox(const char* label, const char* cvarName, const char* (&comboArray)[N],
                  const ComboboxOptions& options = {}) {
    bool dirty = false;
    int32_t value = CVarGetInteger(cvarName, options.defaultIndex);
    if (Combobox<T>(label, &value, comboArray, options)) {
        CVarSetInteger(cvarName, value);
        Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
        ShipInit::Init(cvarName);
        dirty = true;
    }
    return dirty;
}
} // namespace UIWidgets

#endif /* UIWidgetsTemplates_hpp */
