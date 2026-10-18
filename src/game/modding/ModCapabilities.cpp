/*
 * ModCapabilities.cpp — see ModCapabilities.h.
 */
#include "ModCapabilities.h"
#include "port_log.h"

#include <libultraship/libultraship.h> // Ship::Context
#include <ship/resource/ResourceManager.h>
#include <ship/resource/archive/ArchiveManager.h>
#include <ship/resource/archive/Archive.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_set>

namespace ssb64::mods::ModCapabilities {
namespace {

std::mutex sMutex;
std::string sCurrentMod;                    /* manifest name inside ModInit */
std::unordered_set<std::string> sCurrentCaps;
bool sCurrentDeclared = false;              /* manifest carried "capabilities" */

/* Gated prefix groups — a symbol starting with one of these resolves only
 * when the mod declared the matching capability. Order irrelevant; the
 * first matching group wins. */
struct Gate {
    const char* capability;
    const char* const* prefixes;
    size_t count;
};

static const char* const kNetworkPrefixes[] = {
    "port_netlobby", "port_netstats", "port_netleaderboard", "port_netfriends",
    "port_netrollback", "port_ws_", "uplink_", "syNet", "netpeer",
};
static const char* const kPersistPrefixes[] = {
    "port_save", "port_profile", "port_persist",
};
static const char* const kDebugPrefixes[] = {
    "port_diag", "port_watchdog", "port_crash", "port_symbolize", "port_heap",
};
static const char* const kSystemPrefixes[] = {
    "port_update", "port_download", "port_exit", "port_first_run",
};

static const Gate kGates[] = {
    { "network", kNetworkPrefixes, sizeof(kNetworkPrefixes) / sizeof(kNetworkPrefixes[0]) },
    { "persist", kPersistPrefixes, sizeof(kPersistPrefixes) / sizeof(kPersistPrefixes[0]) },
    { "debug",   kDebugPrefixes,   sizeof(kDebugPrefixes)   / sizeof(kDebugPrefixes[0]) },
    { "system",  kSystemPrefixes,  sizeof(kSystemPrefixes)  / sizeof(kSystemPrefixes[0]) },
};

const Gate* GateFor(const char* symbolName) {
    const size_t len = std::strlen(symbolName);
    for (const Gate& g : kGates) {
        for (size_t i = 0; i < g.count; i++) {
            const size_t plen = std::strlen(g.prefixes[i]);
            if (len >= plen && std::memcmp(symbolName, g.prefixes[i], plen) == 0) {
                return &g;
            }
        }
    }
    return nullptr;
}

} // namespace

std::vector<std::string> Normalize(const std::vector<std::string>& declared) {
    static const std::unordered_set<std::string> kKnown = {
        "network", "persist", "debug", "system",
    };
    std::vector<std::string> out;
    for (const std::string& raw : declared) {
        std::string c = raw;
        std::transform(c.begin(), c.end(), c.begin(),
                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (kKnown.contains(c)) {
            out.push_back(std::move(c));
        } else {
            port_log("[mods] unknown capability \"%s\" — ignored\n", raw.c_str());
        }
    }
    return out;
}

void BeginMod(const char* modName) {
    std::lock_guard<std::mutex> lock(sMutex);
    sCurrentMod = modName ? modName : "";
    sCurrentCaps.clear();
    sCurrentDeclared = false;

    auto ctx = Ship::Context::GetInstance();
    if (!ctx || !ctx->GetResourceManager() || !ctx->GetResourceManager()->GetArchiveManager()) {
        return;
    }
    auto archives = ctx->GetResourceManager()->GetArchiveManager()->GetArchives();
    if (!archives) {
        return;
    }
    for (const auto& archive : *archives) {
        if (archive && archive->GetManifest().Name == sCurrentMod) {
            const auto& manifest = archive->GetManifest();
            const auto& declared = manifest.Capabilities;
            sCurrentDeclared = manifest.CapabilitiesDeclared;
            for (const std::string& c : Normalize(declared)) {
                sCurrentCaps.insert(c);
            }
            if (!sCurrentDeclared) {
                port_log("[mods] \"%s\" declares no capabilities — unrestricted "
                         "symbol access (declare \"capabilities\" in manifest.json)\n",
                         sCurrentMod.c_str());
            }
            return;
        }
    }
}

void EndMod() {
    std::lock_guard<std::mutex> lock(sMutex);
    sCurrentMod.clear();
    sCurrentCaps.clear();
    sCurrentDeclared = false;
}

bool IsSymbolAllowed(const char* symbolName) {
    if (symbolName == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(sMutex);
    /* No init window → no declared owner to gate. */
    if (sCurrentMod.empty() || !sCurrentDeclared) {
        return true;
    }
    const Gate* g = GateFor(symbolName);
    if (g == nullptr) {
        return true; /* ungated gameplay/engine prefix */
    }
    if (sCurrentCaps.contains(g->capability)) {
        return true;
    }
    port_log("[mods] DENIED: \"%s\" resolved gated symbol \"%s\" without "
             "the \"%s\" capability\n",
             sCurrentMod.c_str(), symbolName, g->capability);
    return false;
}

} // namespace ssb64::mods::ModCapabilities
