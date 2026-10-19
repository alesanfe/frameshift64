/* See netleaderboard.h. Player card, shared replays and tournaments —
 * extracted from netleaderboard.cpp to keep each TU under the 1000-line
 * file-size cap; shares the JSON/thread helpers via
 * netleaderboard_internal.h. */

#include "netleaderboard.h"
#include "netleaderboard_internal.h"
#include "../domain/netstats.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port_save.h"   /* port_save_get_dir — download target dir */

#include <libultraship/bridge/consolevariablebridge.h>

#ifdef SSB64_HAVE_CURL
#include "base64.h"
#endif


/* ── Player card — GET /player?name=X ────────────────────────────────── */

struct PlayerCard {
    std::string name;
    int rank = 0, rating = 0, wins = 0, losses = 0;
    std::vector<PortLbMatchRow> matches;
};

PlayerCard& sCard = *new PlayerCard;
std::atomic<int>& sCardState = *new std::atomic<int>{0};

extern "C" int port_netleaderboard_player_state(void)
{
    return sCardState.load();
}

extern "C" void port_netleaderboard_fetch_player(const char *server,
                                                 const char *name)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || name == nullptr || name[0] == '\0' ||
        sCardState.exchange(1) == 1) {
        return;
    }
    const std::string qname = ssb64::UrlEncodeComponent(name);
    port_lb::lb_fetch(base + "/player?name=" + qname, sCardState, sCard,
        [](const nlohmann::json &doc, PlayerCard &card) {
            if (!doc.is_object() || !doc["name"].is_string()) {
                return false;
            }
            card.name = doc["name"].get<std::string>().substr(0, 96);
            card.rank = port_lb::json_int_or(doc, "rank", 0);
            card.rating = port_lb::json_int_or(doc, "rating", 0);
            card.wins = port_lb::json_int_or(doc, "wins", 0);
            card.losses = port_lb::json_int_or(doc, "losses", 0);
            const auto it = doc.find("matches");
            if (it != doc.end() && it->is_array()) {
                for (const auto &m : *it) {
                    if (card.matches.size() >= 200) {
                        break;
                    }
                    PortLbMatchRow r{};
                    if (port_lb::parse_match_row(m, r)) {
                        card.matches.push_back(r);
                    }
                }
            }
            return true;
        });
#else
    (void)server; (void)name;
#endif
}

extern "C" int port_netleaderboard_player_card(char *name, int capacity,
                                               int *rank, int *rating,
                                               int *wins, int *losses)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (sCard.name.empty()) {
        return 0;
    }
    if (name != nullptr && capacity > 0) {
        std::snprintf(name, static_cast<size_t>(capacity), "%s", sCard.name.c_str());
    }
    if (rank != nullptr) *rank = sCard.rank;
    if (rating != nullptr) *rating = sCard.rating;
    if (wins != nullptr) *wins = sCard.wins;
    if (losses != nullptr) *losses = sCard.losses;
    return 1;
}

extern "C" int port_netleaderboard_player_matches_count(void)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    return static_cast<int>(sCard.matches.size());
}

extern "C" int port_netleaderboard_player_match_at(int index,
                                                   PortLbMatchRow *out)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (out == nullptr || index < 0 ||
        index >= static_cast<int>(sCard.matches.size())) {
        return 0;
    }
    *out = sCard.matches[index];
    return 1;
}

/* ── Shared-replay index + download — GET /replays, GET /replay/<id> ──── */

struct SharedReplay {
    std::string id, by, stamp;
};

std::vector<SharedReplay>& sSharedReplays = *new std::vector<SharedReplay>;
std::atomic<int>& sReplaysState = *new std::atomic<int>{0};
std::atomic<int>& sReplayDlState = *new std::atomic<int>{0};
std::string& sReplayDlPath = *new std::string;

extern "C" int port_netleaderboard_replays_state(void)
{
    return sReplaysState.load();
}

extern "C" void port_netleaderboard_fetch_replays(const char *server)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || sReplaysState.exchange(1) == 1) {
        return;
    }
    port_lb::lb_fetch(base + "/replays", sReplaysState, sSharedReplays,
        [](const nlohmann::json &doc, std::vector<SharedReplay> &rows) {
            if (!doc.is_object() || !doc["replays"].is_array()) {
                return false;
            }
            for (const auto &m : doc["replays"]) {
                if (rows.size() >= 500) {
                    break;
                }
                if (!m.is_object()) {
                    continue;
                }
                SharedReplay r;
                std::string v;
                if (port_lb::json_str(m, "id", v)) r.id = v.substr(0, 32);
                if (port_lb::json_str(m, "by", v)) r.by = v.substr(0, 96);
                char stamp[24];
                port_lb::json_str(m, "at", v);
                port_lb::hist_stamp(v, stamp, sizeof(stamp));
                r.stamp = stamp;
                if (!r.id.empty()) {
                    rows.push_back(std::move(r));
                }
            }
            return true;
        });
#else
    (void)server;
#endif
}

extern "C" int port_netleaderboard_replays_count(void)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    return static_cast<int>(sSharedReplays.size());
}

extern "C" int port_netleaderboard_replays_at(int index, char *id, int idCap,
                                              char *by, int byCap,
                                              char *stamp, int stCap)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (index < 0 || index >= static_cast<int>(sSharedReplays.size())) {
        return 0;
    }
    const SharedReplay &r = sSharedReplays[index];
    if (id != nullptr && idCap > 0) {
        std::snprintf(id, static_cast<size_t>(idCap), "%s", r.id.c_str());
    }
    if (by != nullptr && byCap > 0) {
        std::snprintf(by, static_cast<size_t>(byCap), "%s", r.by.c_str());
    }
    if (stamp != nullptr && stCap > 0) {
        std::snprintf(stamp, static_cast<size_t>(stCap), "%s", r.stamp.c_str());
    }
    return 1;
}

extern "C" int port_netleaderboard_replay_dl_state(void)
{
    return sReplayDlState.load();
}

extern "C" int port_netleaderboard_replay_dl_path(char *out, int capacity)
{
    if (out == nullptr || capacity <= 0) return 0;
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    std::snprintf(out, static_cast<size_t>(capacity), "%s", sReplayDlPath.c_str());
    return static_cast<int>(sReplayDlPath.size());
}

#ifdef SSB64_HAVE_CURL
/* Decodes {"data":b64} and persists under <save>/replays/shared-<id>.ssr —
 * 400 KB mirrors the upload cap the service enforces on /replay. Returns
 * the saved path, or "" on any failure. */
static std::string replay_download_save(const std::string &body,
                                        const std::string &safeId)
{
    const nlohmann::json doc =
        nlohmann::json::parse(body, nullptr, /*exceptions*/ false);
    std::string data;
    if (!doc.is_object() || !port_lb::json_str(doc, "data", data)) {
        return {};
    }
    std::vector<uint8_t> bytes;
    if (!ssb64::Base64Decode(data, bytes) || bytes.empty() ||
        bytes.size() > 400 * 1024) {
        return {};
    }
    const std::string dir = std::string(port_save_get_dir()) + "/replays";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::string path = dir + "/shared-" + safeId + ".ssr";
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good() ? path : std::string();
}
#endif

extern "C" void port_netleaderboard_replay_download(const char *server,
                                                    const char *id)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || id == nullptr || id[0] == '\0') {
        return;
    }
    /* The id is a server-minted slug (REPLAY_ID_RE: [0-9a-f]{10}) — keep
     * it to that alphabet so it can't smuggle a path traversal into the
     * filename. */
    for (const char *p = id; *p; ++p) {
        const char c = *p;
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return;
        }
    }
    const std::string safeId = id;
    if (sReplayDlState.exchange(1) == 1) {
        return;                     /* already in flight */
    }
    std::thread([base, safeId] {
        std::string body, error, saved;
        int state = -1;
        if (ssb64::CurlGetToString(base + "/replay/" + safeId, &body, &error,
                                   {}, 15)) {
            saved = replay_download_save(body, safeId);
            if (!saved.empty()) {
                state = 2;
            }
        }
        {
            std::lock_guard<std::mutex> lk(port_lb::SharedLock());
            sReplayDlPath = saved;
        }
        sReplayDlState = state;
    }).detach();
#else
    (void)server; (void)id;
#endif
}

