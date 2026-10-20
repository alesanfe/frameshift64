/* §35 — named player profiles. See profiles.h for scope. */

#include "profiles.h"
#include "port_save.h"

#include <ship/Context.h>
#include <ship/config/ConsoleVariable.h>
#include <ship/controller/controldeck/ControlDeck.h>
#include <ship/controller/controldevice/controller/Controller.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace {

/* CVars under these prefixes ride inside a profile. */
constexpr const char* kPrefixes[] = { "gControllers.", "gEnhancements.", "gMods." };

bool NameValid(const char* name) {
    if (name == nullptr) return false;
    const size_t len = strlen(name);
    if (len == 0 || len > 32) return false;
    for (size_t i = 0; i < len; i++) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!std::isalnum(c) && c != ' ' && c != '_' && c != '-') return false;
    }
    return true;
}

bool InProfileScope(const std::string& key) {
    for (const char* p : kPrefixes) {
        if (key.compare(0, strlen(p), p) == 0) return true;
    }
    return false;
}

std::string ProfilePath(const char* name) {
    return std::string(port_save_get_dir()) + "/profiles/" + name + ".json";
}

} // namespace


/* Serialize every in-scope cvar into a sorted JSON array. Colors under
 * profile prefixes are rare; skip rather than guess the serialization. */
static nlohmann::json CollectProfileEntries(Ship::ConsoleVariable* cvars) {
    nlohmann::json entries = nlohmann::json::array();
    for (const std::string& key : cvars->GetVariableNames()) {
        if (!InProfileScope(key)) continue;
        auto var = cvars->Get(key.c_str());
        if (var == nullptr) continue;
        nlohmann::json e;
        e["k"] = key;
        switch (var->Type) {
            case Ship::ConsoleVariableType::Integer:
                e["t"] = "i"; e["v"] = var->Integer; break;
            case Ship::ConsoleVariableType::Float:
                e["t"] = "f"; e["v"] = var->Float; break;
            case Ship::ConsoleVariableType::String:
                e["t"] = "s"; e["v"] = var->String != nullptr ? var->String : ""; break;
            case Ship::ConsoleVariableType::Color:
            case Ship::ConsoleVariableType::Color24:
                continue;
        }
        entries.push_back(e);
    }
    std::sort(entries.begin(), entries.end(),
              [](const nlohmann::json& a, const nlohmann::json& b) {
                  return a["k"].get<std::string>() < b["k"].get<std::string>();
              });
    return entries;
}

extern "C" int port_profile_save(const char* name) {
    if (!NameValid(name)) return -1;
    auto cvars = Ship::Context::GetInstance()->GetConsoleVariables();
    if (cvars == nullptr) return -1;

    nlohmann::json entries = CollectProfileEntries(cvars.get());

    nlohmann::json doc;
    doc["version"] = 1;
    doc["cvars"] = std::move(entries);

    const std::string dir = std::string(port_save_get_dir()) + "/profiles";
    if (port_mkdir_p(dir.c_str()) != 0) return -1;
    std::ofstream out(ProfilePath(name), std::ios::trunc);
    if (!out) return -1;
    out << doc.dump(2) << "\n";
    return out.good() ? 0 : -1;
}

/* Apply each in-scope cvar entry from the profile document. */
static void ProfileApplyCvars(const nlohmann::json& doc,
                              const std::shared_ptr<Ship::ConsoleVariable>& cvars) {
    for (const auto& e : doc["cvars"]) {
        const std::string key = e.value("k", "");
        const std::string t = e.value("t", "");
        if (key.empty() || !InProfileScope(key)) continue; /* refuse out-of-scope keys */
        if (t == "i" && e["v"].is_number_integer()) {
            cvars->SetInteger(key.c_str(), e["v"].get<int32_t>());
        } else if (t == "f" && e["v"].is_number()) {
            cvars->SetFloat(key.c_str(), e["v"].get<float>());
        } else if (t == "s" && e["v"].is_string()) {
            cvars->SetString(key.c_str(), e["v"].get<std::string>().c_str());
        }
    }
}

/* Push the restored mappings into the live controllers. */
static void ProfileReloadControllers() {
    auto deck = Ship::Context::GetInstance()->GetControlDeck();
    if (deck == nullptr) {
        return;
    }
    for (uint8_t port = 0; port < 4; port++) {
        auto controller = deck->GetControllerByPort(port);
        if (controller != nullptr) controller->ReloadAllMappingsFromConfig();
    }
}

extern "C" int port_profile_load(const char* name) {
    if (!NameValid(name)) return -1;
    std::ifstream in(ProfilePath(name));
    if (!in) return -1;

    nlohmann::json doc;
    try {
        doc = nlohmann::json::parse(in);
    } catch (...) {
        return -1;
    }
    if (!doc.is_object() || doc.value("version", 0) != 1 || !doc["cvars"].is_array()) {
        return -1;
    }

    auto cvars = Ship::Context::GetInstance()->GetConsoleVariables();
    if (cvars == nullptr) return -1;

    ProfileApplyCvars(doc, cvars);
    cvars->Save();
    ProfileReloadControllers();
    return 0;
}


extern "C" int port_profile_delete(const char* name) {
    if (!NameValid(name)) return -1;
    return std::remove(ProfilePath(name).c_str()) == 0 ? 0 : -1;
}

extern "C" size_t port_profile_list(char* buf, size_t cap) {
    const std::string dir = std::string(port_save_get_dir()) + "/profiles";
    std::set<std::string> names;

    /* Tiny cross-platform readdir: use std::filesystem (C++17). */
    namespace fs = std::filesystem;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir, ec)) {
        if (!entry.is_regular_file()) continue;
        const std::string fname = entry.path().filename().string();
        if (fname.size() > 5 && fname.compare(fname.size() - 5, 5, ".json") == 0) {
            names.insert(fname.substr(0, fname.size() - 5));
        }
    }

    if (buf != nullptr && cap > 0) {
        std::string joined;
        for (const std::string& n : names) {
            if (!joined.empty()) joined += '\n';
            joined += n;
        }
        const size_t n = (std::min)(joined.size(), cap - 1); /* () defeats the windows.h min macro */
        memcpy(buf, joined.data(), n);
        buf[n] = '\0';
    }
    return names.size();
}
