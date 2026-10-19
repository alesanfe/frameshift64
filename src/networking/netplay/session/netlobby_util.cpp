/* See netlobby_util.h — state-free lobby helpers (compat token, wire-line
 * split, CVar rule reads) kept out of netlobby.cpp. Wire-message JSON
 * parsing lives in lobby_json.cpp. */

#include "netlobby_util.h"

#include "fnv1a.h"
#include "udp_transport.h"
#include "port_log.h"

#include <libultraship/bridge/consolevariablebridge.h>

#ifndef DISABLE_SCRIPTING
#include "ModRegistry.h"
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <ship/Context.h>

namespace ssb64::netlobby {

constexpr unsigned int kPeerTimeoutMs = 10000;

#ifdef SSB64_TEST_HOOKS
/* Test rigs may need a longer leash: several same-machine peers can stall
 * a process past the default 10s (rollback resims blocking the netloop on
 * starved CPU) without the peer actually being dead. SSB64_NETPLAY_DROP_MS
 * overrides this and the netpeer pacing-drop timeout together. */
unsigned int peer_timeout_ms() {
    if (const char* env = std::getenv("SSB64_NETPLAY_DROP_MS")) {
        int v = std::atoi(env);
        if (v > 0) {
            return static_cast<unsigned int>(v);
        }
    }
    return kPeerTimeoutMs;
}
#else
unsigned int peer_timeout_ms() { return kPeerTimeoutMs; }
#endif

bool is_spec_slot(int slot) { return slot >= 4 && slot <= 7; }

#ifdef BATTLESHIP_CURRENT_VERSION
const char* const kGameVersion = BATTLESHIP_CURRENT_VERSION;
#else
const char* const kGameVersion = "dev";
#endif

#ifdef BATTLESHIP_BUILD_SHA
const char* const kBuildSha = BATTLESHIP_BUILD_SHA;
#else
const char* const kBuildSha = "dev";
#endif

using ssb64::fnv1a64_accum;
using ssb64::kFnv1a64Basis;

/* FNV-1a over a whole file (streamed; 0 when unreadable). The buffer must
 * stay small: this runs on the game thread inside port_netlobby_update,
 * and Windows' default thread stack is only 1 MiB — a 1 MiB buffer here
 * overflows it (0xC00000FD in __chkstk). */
static uint64_t fnv1a_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return 0;
    }
    uint64_t h = ssb64::kFnv1a64Basis;
    char buf[64 << 10];
    while (f) {
        f.read(buf, sizeof(buf));
        const std::streamsize n = f.gcount();
        if (n > 0) {
            h = fnv1a64_accum(h, buf, static_cast<size_t>(n));
        }
    }
    return h;
}

/* Content fingerprint of one mounted mod payload. Archive files (.o2r/.otr/
 * .zip) hash their full bytes; folder mods hash each contained file's
 * relative path + byte size (cheap; catches content drift under an unchanged
 * name@version without hashing arbitrarily large trees). */
static uint64_t mod_content_hash(const std::filesystem::path& path) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(path, ec)) {
        return fnv1a_file(path);
    }
    if (!std::filesystem::is_directory(path, ec)) {
        return 0;
    }
    uint64_t h = ssb64::kFnv1a64Basis;
    std::filesystem::recursive_directory_iterator it(path, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        const std::string rel = it->path().lexically_relative(path).generic_string();
        h = fnv1a64_accum(h, rel);
        if (it->is_regular_file(ec)) {
            const uint64_t sz = it->file_size(ec);
            h = fnv1a64_accum(h, &sz, sizeof(sz));
        }
    }
    return h;
}

/* FNV-1a over the base asset archive (BattleShip.o2r / BattleShip-JP.o2r).
 * Two peers with the same build but differently-extracted or hand-edited
 * archives would desync silently; the archive bytes are the ground truth.
 * Computed once and cached — the file can't change under a running process. */
static uint64_t asset_archive_hash() {
    static uint64_t cached = 0;
    static bool computed = false;
    if (!computed) {
        computed = true;
        const std::filesystem::path p = Ship::Context::GetPathRelativeToAppDirectory(SSB64_O2R_NAME);
        cached = fnv1a_file(p);
        if (cached == 0) {
            port_log("SSB64 Lobby: WARNING could not hash %s — asset "
                     "compatibility check will be identity-only\n",
                     p.string().c_str());
        }
    }
    return cached;
}

std::string compat_token() {
#ifdef SSB64_TEST_HOOKS
    /* test hook — identity forgery must not exist in shipping builds */
    if (const char* force = std::getenv("SSB64_COMPAT_TOKEN")) {
        return force;
    }
#endif
    uint64_t mods = ssb64::kFnv1a64Basis; /* FNV-1a 64 */
#ifndef DISABLE_SCRIPTING
    for (const ssb64::mods::ModInfo& m : ssb64::mods::ModRegistry::Snapshot()) {
        /* Mods that declare "simulation": false in their manifest are
         * cosmetic-only by contract (textures/audio) and exempt from the
         * compatibility hash — they can't desync. The exemption trusts the
         * mod's own declaration: a liar that still shadows gameplay data
         * will desync, same as on console-era mods. */
        if (!m.simulation) {
            continue;
        }
        /* Every mounted mod counts, not just script-Loaded ones: a mounted
         * archive's files shadow game assets (movesets, physics, stage
         * data) even when no ModInit ran, so an asset-only mod would
         * otherwise desync silently. Key by state so the same archive
         * Loaded vs NotLoaded differs; when the manifest is invalid fall
         * back to the archive basename (the name is untrustworthy). The
         * trailing hash covers the payload bytes, so two archives sharing
         * name@version but differing in content are still rejected. */
        std::string entry = std::to_string(static_cast<int>(m.state)) + ":";
        entry += !m.name.empty() ? m.name + "@" + m.version
                                 : std::filesystem::path(m.archivePath).filename().string();
        char hashText[20];
        std::snprintf(hashText, sizeof(hashText), ":%016llx;",
                      static_cast<unsigned long long>(mod_content_hash(m.archivePath)));
        entry += hashText;
        mods = fnv1a64_accum(mods, entry);
    }
#endif
    char text[96];
    std::snprintf(text, sizeof(text), "%s:%016llx:%016llx", kBuildSha, static_cast<unsigned long long>(mods),
                  static_cast<unsigned long long>(asset_archive_hash()));
    return text;
}

std::string local_endpoint() {
    char text[64];
    return port_udp_local_endpoint(text, sizeof(text)) ? std::string(text) : std::string("-");
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v != nullptr && *v != '\0') ? v : nullptr;
}

std::vector<std::string> split(const char* text) {
    std::vector<std::string> parts;
    const char* at = text;
    while (*at != '\0') {
        while (*at == ' ') {
            at++;
        }
        const char* start = at;
        while (*at != '\0' && *at != ' ') {
            at++;
        }
        if (at > start) {
            parts.emplace_back(start, at - start);
        }
    }
    return parts;
}

int local_tap_jump_off() {
    return CVarGetInteger("gEnhancements.TapJumpDisabled.P1", 0) != 0;
}

/* The display name we advertise in PICK: the local profile name, sanitized
 * down to the wire alphabet (no spaces — the protocol is space-delimited). */
std::string local_player_name() {
    std::string name;
    if (const char* raw = CVarGetString("gProfiles.Name", "Player")) {
        name = raw;
    }
    std::string out;
    for (const char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (wire_name_char(u)) {
            out += c;
        }
    }
    if (out.empty()) {
        out = "Player";
    }
    out.resize(kWireNameMax);
    return out;
}

void host_rules(int* autoz, int* comp, int* neutral, int* hazards_off) {
    *autoz = CVarGetInteger("gEnhancements.CasualRules.AutoZCancel", 0) != 0;
    *comp = CVarGetInteger("gEnhancements.CompRuleset", 0) != 0;
    *neutral = CVarGetInteger("gEnhancements.NeutralSpawns", 0) != 0;
    *hazards_off = CVarGetInteger("gEnhancements.StageHazardsDisabled", 0) != 0;
}

} // namespace ssb64::netlobby
