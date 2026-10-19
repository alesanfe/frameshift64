// Shared between PortMenuSettings.cpp and PortMenuSettingsPanels.cpp —
// profile-name helpers used by sidebar widgets in both TUs. Internal to
// the presentation module.
#pragma once
#include <string>
#include <vector>

namespace ssb64 {
std::vector<std::string> PortProfileNames();
std::string PortProfileSelectedName();
}
