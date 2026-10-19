#pragma once

/* Internal helpers for netlobby.cpp — pure/state-free utilities lifted out
 * of the lobby TU so the session state machine stays readable. Everything
 * here is C++-internal; the public C ABI stays in netlobby.h. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ssb64::netlobby {

/* Peers are dropped after this much silence; test builds may override via
 * SSB64_NETPLAY_DROP_MS. */
unsigned int peer_timeout_ms();

/* Spectators occupy room slots 4..7: receive-only relays that watch the
 * match live without joining pacing or publishing inputs. */
bool is_spec_slot(int slot);

extern const char* const kGameVersion;
extern const char* const kBuildSha;

/* Compatibility token exchanged in PICK: "<build_sha>:<mod_hash>:<asset_hash>"
 * — see the implementation for why every participant must match. */
std::string compat_token();

/* Local UDP endpoint as "ip:port" for the endpoint exchange ("-" on failure). */
std::string local_endpoint();

/* getenv that treats empty as absent. */
const char* env(const char* name);

/* Split an "SSBL VERB args..." line on spaces. */
std::vector<std::string> split(const char* text);

/* Local CVar reads packed into the PICK/STARTM wire messages. */
int local_tap_jump_off();
void host_rules(int* autoz, int* comp, int* neutral, int* hazards_off);
std::string local_player_name();

/* Wire name alphabet ([alnum._-], spaceless — the protocol is space-
 * delimited) and the display-name cap. Both the sender (local_player_name)
 * and the receiver (PICK parse) must agree, so the check lives here.
 * Matches std::isalnum under the C locale without the locale lookup. */
inline bool wire_name_char(unsigned char u) {
    return (u >= '0' && u <= '9') || (u >= 'A' && u <= 'Z') ||
           (u >= 'a' && u <= 'z') || u == '_' || u == '-' || u == '.';
}
inline constexpr std::size_t kWireNameMax = 15;

} // namespace ssb64::netlobby
