// Shared between UIWidgets.cpp and UIWidgets_inputs.cpp — tooltip helper
// used by widgets in both TUs. Internal to the presentation module.
#pragma once
namespace UIWidgets {
void ShowWidgetTooltip(bool disabled, const char* disabledTooltip, const char* tooltip);
}
