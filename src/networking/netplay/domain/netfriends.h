// Copyright (c) 2025 SSB64 Tools.
// SPDX-License-Identifier: MIT
//
// Persistent friends/block list for netplay (JSON, next to the save file).
// No central identity service exists — entries key on the display name the
// peer advertises, so this is a personal address-book, not an account system.
// Blocking is enforced client-side: connecting peers whose advertised name
// matches a blocked entry surface a warning so the host can drop them.
#pragma once

#include <array>
#include <cstdint>

struct PortNetFriend {
    std::array<char, 32> name{};
    std::int64_t added_unix = 0;
    bool blocked = false;
};

constexpr int kMaxNetFriends = 64;

#ifdef __cplusplus
extern "C" {
#endif

int  port_netfriends_count(void);
bool port_netfriends_get(int index, PortNetFriend* out);           // sorted by name
bool port_netfriends_add(const char* name);
bool port_netfriends_remove(const char* name);
bool port_netfriends_set_blocked(const char* name, bool blocked);
bool port_netfriends_is_blocked(const char* name);                 // case-insensitive
bool port_netfriends_load(void);
bool port_netfriends_save(void);

#ifdef __cplusplus
}
#endif
