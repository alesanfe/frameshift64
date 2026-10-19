// Copyright (c) 2025 SSB64 Tools.
// SPDX-License-Identifier: MIT
//
// netfriends.json schema:
//   {"friends":[{"name":"...","added":1729...,"blocked":false}, ...]}

#include "netfriends.h"
#include "port_log.h"
#include "atomic_write.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "port_save.h"

#include <nlohmann/json.hpp>


namespace ssb64::netfriends {

namespace {

struct Friend {
    std::string name;
    std::int64_t added = 0;
    bool blocked = false;
};

std::vector<Friend> sFriends;
std::mutex sFriendsMutex;
bool sLoaded = false;

/* Same directory convention as netstats.json / the SRAM image — port_save
 * owns the resolution (env override → app-dir → SDL pref → cwd). */
std::filesystem::path FriendsPath() {
    return std::filesystem::path(port_save_get_dir()) / "netfriends.json";
}

int NameCompare(const std::string& a, const char* b) {
    const size_t n = (std::min)(a.size(), std::strlen(b));
    for (size_t i = 0; i < n; i++) {
        const unsigned char ca = static_cast<unsigned char>(std::tolower(a[i]));
        const unsigned char cb = static_cast<unsigned char>(std::tolower(b[i]));
        if (ca != cb) {
            return ca < cb ? -1 : 1;
        }
    }
    if (a.size() == std::strlen(b)) {
        return 0;
    }
    return a.size() < std::strlen(b) ? -1 : 1;
}

Friend* Find(const char* name) {
    for (auto& f : sFriends) {
        if (NameCompare(f.name, name) == 0) {
            return &f;
        }
    }
    return nullptr;
}

}  // namespace

/* Parses one "friends" entry; tolerates added as string-or-number and
 * blocked as bool-or-int (older files serialized them loosely). Returns
 * false for non-object entries — the caller skips them. */
bool ParseFriendEntry(const nlohmann::json& e, Friend& f)
{
    if (!e.is_object()) {
        return false;
    }
    const auto name = e.find("name");
    if (name != e.end() && name->is_string()) {
        f.name = name->get_ref<const std::string&>();
    }
    const auto added = e.find("added");
    if (added != e.end()) {
        if (added->is_number()) {
            f.added = added->get<std::int64_t>();
        } else if (added->is_string()) {
            f.added = std::strtoll(added->get_ref<const std::string&>().c_str(),
                                   nullptr, 10);
        }
    }
    const auto blocked = e.find("blocked");
    if (blocked != e.end()) {
        if (blocked->is_boolean()) {
            f.blocked = blocked->get<bool>();
        } else if (blocked->is_number()) {
            f.blocked = blocked->get<int>() != 0;
        }
    }
    return true;
}

void EnsureLoaded() {
    if (sLoaded) {
        return;
    }
    sLoaded = true;
    std::ifstream in(FriendsPath());
    if (!in.is_open()) {
        return;
    }
    const std::string body{std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>()};
    const nlohmann::json doc =
        nlohmann::json::parse(body, nullptr, /*exceptions*/ false);
    const auto friends = doc.find("friends");
    if (!doc.is_object() || friends == doc.end() || !friends->is_array()) {
        return;
    }
    for (const nlohmann::json& e : *friends) {
        if (sFriends.size() >= kMaxNetFriends) {
            break;
        }
        Friend f;
        if (!ParseFriendEntry(e, f)) {
            break;      /* malformed entry stops the load, as before */
        }
        if (!f.name.empty()) {
            sFriends.push_back(std::move(f));
        }
    }
    std::sort(sFriends.begin(), sFriends.end(),
              [](const Friend& a, const Friend& b) { return NameCompare(a.name, b.name.c_str()) < 0; });
}

bool Save() {
    const std::filesystem::path path = FriendsPath();
    /* Serialize with the real JSON writer (proper escaping) then commit
     * atomically — a mid-write crash strands a .tmp, never truncates the
     * live friends file. */
    nlohmann::json doc;
    doc["friends"] = nlohmann::json::array();
    for (const Friend& f : sFriends) {
        doc["friends"].push_back(
            {{"name", f.name}, {"added", f.added}, {"blocked", f.blocked}});
    }
    if (!WriteFileAtomic(path, doc.dump(2))) {
        port_log("netfriends: cannot write %s\n", path.string().c_str());
        return false;
    }
    return true;
}

}  // namespace ssb64::netfriends

extern "C" {

int port_netfriends_count(void) {
    std::lock_guard<std::mutex> lock(ssb64::netfriends::sFriendsMutex);
    ssb64::netfriends::EnsureLoaded();
    return static_cast<int>(ssb64::netfriends::sFriends.size());
}

bool port_netfriends_get(int index, PortNetFriend* out) {
    if (!out) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ssb64::netfriends::sFriendsMutex);
    ssb64::netfriends::EnsureLoaded();
    if (index < 0 || index >= static_cast<int>(ssb64::netfriends::sFriends.size())) {
        return false;
    }
    const auto& f = ssb64::netfriends::sFriends[index];
    std::snprintf(out->name.data(), out->name.size(), "%s", f.name.c_str());
    out->added_unix = f.added;
    out->blocked = f.blocked;
    return true;
}

bool port_netfriends_add(const char* name) {
    if (!name || !*name) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ssb64::netfriends::sFriendsMutex);
    ssb64::netfriends::EnsureLoaded();
    if (ssb64::netfriends::Find(name) ||
        ssb64::netfriends::sFriends.size() >= kMaxNetFriends) {
        return ssb64::netfriends::Find(name) != nullptr;
    }
    ssb64::netfriends::Friend f;
    f.name = name;
    f.added = std::chrono::duration_cast<std::chrono::seconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count();
    ssb64::netfriends::sFriends.push_back(std::move(f));
    std::sort(ssb64::netfriends::sFriends.begin(), ssb64::netfriends::sFriends.end(),
              [](const auto& a, const auto& b) {
                  return ssb64::netfriends::NameCompare(a.name, b.name.c_str()) < 0;
              });
    return ssb64::netfriends::Save();
}

bool port_netfriends_remove(const char* name) {
    if (!name || !*name) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ssb64::netfriends::sFriendsMutex);
    ssb64::netfriends::EnsureLoaded();
    auto& v = ssb64::netfriends::sFriends;
    const auto it = std::remove_if(v.begin(), v.end(), [&](const auto& f) {
        return ssb64::netfriends::NameCompare(f.name, name) == 0;
    });
    if (it == v.end()) {
        return false;
    }
    v.erase(it, v.end());
    return ssb64::netfriends::Save();
}

bool port_netfriends_set_blocked(const char* name, bool blocked) {
    if (!name || !*name) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ssb64::netfriends::sFriendsMutex);
    ssb64::netfriends::EnsureLoaded();
    ssb64::netfriends::Friend* f = ssb64::netfriends::Find(name);
    if (!f) {
        return false;
    }
    f->blocked = blocked;
    return ssb64::netfriends::Save();
}

bool port_netfriends_is_blocked(const char* name) {
    if (!name || !*name) {
        return false;
    }
    std::lock_guard<std::mutex> lock(ssb64::netfriends::sFriendsMutex);
    ssb64::netfriends::EnsureLoaded();
    const ssb64::netfriends::Friend* f = ssb64::netfriends::Find(name);
    return f && f->blocked;
}

}
