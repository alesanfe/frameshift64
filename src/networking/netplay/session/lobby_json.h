#pragma once

/* Wire-message JSON helpers for the netplay lobby — the Worker speaks real
 * JSON, so parse it with the real parser (nlohmann, already linked
 * project-wide). Self-contained so the unit test can compile this TU
 * directly without game state (same pattern as netpacket.c). */

#include <string>

#include <nlohmann/json.hpp>

namespace ssb64::netlobby {

/* Parse a wire message; returns a discarded value on malformed input. */
nlohmann::json json_parse(const std::string& text);

/* Read a field as its wire string form: strings pass through, numbers and
 * bools serialize to their JSON token ("4", "true") so readers that
 * atoi()/strcmp against them keep working, everything else (absent, null,
 * objects, arrays) reads as "". */
std::string json_str(const nlohmann::json& obj, const char* key);

/* Read an integer field; returns `def` when absent or non-numeric, and
 * tolerates string-encoded numbers the way the old substring scanner did. */
int json_int(const nlohmann::json& obj, const char* key, int def = -1);

} // namespace ssb64::netlobby
