#include "lobby_json.h"

#include <cstdlib>

namespace ssb64::netlobby {

nlohmann::json json_parse(const std::string& text) {
    return nlohmann::json::parse(text, nullptr, /*exceptions*/ false);
}

std::string json_str(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object()) {
        return std::string();
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return std::string();
    }
    if (it->is_string()) {
        return it->get_ref<const std::string&>();
    }
    /* Numbers/bools serialize to their wire token so readers that strcmp
     * against "true" or atoi() keep working on either representation. */
    if (it->is_boolean()) {
        return it->get<bool>() ? "true" : "false";
    }
    if (it->is_number()) {
        return it->dump();
    }
    return std::string();
}

int json_int(const nlohmann::json& obj, const char* key, int def) {
    if (!obj.is_object()) {
        return def;
    }
    const auto it = obj.find(key);
    if (it == obj.end()) {
        return def;
    }
    if (it->is_number_integer()) {
        return it->get<int>();
    }
    if (it->is_number()) {
        return static_cast<int>(it->get<double>());
    }
    if (it->is_string()) {
        return std::atoi(it->get_ref<const std::string&>().c_str());
    }
    return def;
}

} // namespace ssb64::netlobby
