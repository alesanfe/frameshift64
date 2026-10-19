/* See netleaderboard.h. */

#include "netleaderboard.h"
#include "netleaderboard_internal.h"
#include "../domain/netstats.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "port_save.h"   /* port_save_get_dir — download target dir */

#include <ship/Context.h>

#include <libultraship/bridge/consolevariablebridge.h>

#ifdef SSB64_HAVE_CURL
#include "http_curl.h"
#include "netleaderboard_internal.h"
#include "base64.h"
#include <nlohmann/json.hpp>
#endif

namespace {

struct LbRow {
    std::string name;
    int rating = 0;
    int wins = 0;
    int losses = 0;
    int suspicious = 0;      /* server smurf-heuristic flag */
    int banned = 0;          /* server community-moderation flag */
};

/* Intentionally leaked: submit/fetch run on detached fire-and-forget
 * worker threads that can still be in flight while statics destruct at
 * process exit — heap-allocating the state means there is no destruction
 * order to race against. The shared lock lives in port_lb::SharedLock(). */
std::vector<LbRow>& sRows = *new std::vector<LbRow>;
std::atomic<int>& sState = *new std::atomic<int>{0};      /* idle */
std::string& sMyClaimed = *new std::string;               /* name we submitted under (for rank) */

/* Fills rows from a parsed "entries" array (server rows are
 * {name,rating,wins,losses}); caps at 1000 so a hostile server cannot grow
 * memory unbounded. */
void parse_lb_entries(const nlohmann::json &entries, std::vector<LbRow> &rows)
{
    for (const auto &e : entries) {
        if (rows.size() >= 1000) {
            break;      /* cap: don't let the server grow memory */
        }
        if (!e.is_object()) {
            continue;
        }
        LbRow r;
        const auto name = e.find("name");
        if (name != e.end() && name->is_string()) {
            r.name = name->get<std::string>().substr(0, 64);
        }
        const auto num = [&e](const char *k) {
            const auto it = e.find(k);
            return (it != e.end() && it->is_number()) ? it->get<int>() : 0;
        };
        r.rating = num("rating");
        r.wins = num("wins");
        r.losses = num("losses");
        const auto susp = e.find("suspicious");
        r.suspicious = (susp != e.end() && susp->is_boolean() && susp->get<bool>()) ? 1 : 0;
        const auto ban = e.find("banned");
        r.banned = (ban != e.end() && ban->is_boolean() && ban->get<bool>()) ? 1 : 0;
        rows.push_back(std::move(r));
    }
}

} // namespace

extern "C" int port_netleaderboard_state(void)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    return sState;
}

extern "C" void port_netleaderboard_fetch(const char *server)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lk(port_lb::SharedLock());
        if (sState == 1) {
            return;                 /* already in flight */
        }
        sState = 1;
    }
    port_lb::lb_fetch(base + "/leaderboard", sState, sRows,
        [](const nlohmann::json &doc, std::vector<LbRow> &rows) {
            /* operator[]("entries") on a non-object doc is UB — check the
             * shape first (a hostile server could send anything). */
            if (!doc.is_object() || !doc["entries"].is_array()) {
                return false;
            }
            parse_lb_entries(doc["entries"], rows);
            return true;
        });
#else
    (void)server;
#endif
}

extern "C" void port_netleaderboard_submit(const char *server)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty()) {
        return;
    }
    char key[40];
    port_netstats_player_key(key, sizeof(key));
    const char *name = CVarGetString("gProfiles.Name", "Player");
    const int rating = port_netstats_rating();
    const int wins = port_netstats_wins();
    const int losses = port_netstats_losses();

    std::string claimed = port_lb::sanitize_lb_name(name);
    if (claimed.empty()) {
        claimed = "Player";
    }
    /* Submit the sanitized form — the server keeps it verbatim, so
     * sMyClaimed then byte-matches the stored row for rank lookup. */
    nlohmann::json payload = {
        {"name", claimed},
        {"key", key},
        {"rating", rating},
        {"wins", wins},
        {"losses", losses},
    };
    const std::string body = payload.dump();
    std::thread([base, body, claimed] {
        std::string resp, error;
        if (ssb64::CurlPostToString(base + "/submit", body, "application/json",
                                    &resp, &error, 15)) {
            std::lock_guard<std::mutex> lk(port_lb::SharedLock());
            sMyClaimed = claimed;
        }
    }).detach();
#else
    (void)server;
#endif
}

/* FNV-1a-64 — deterministic across builds/platforms, which is all a
 * client-consistent match_id needs (both sides hash the same payload). */
static std::string fnv1a64_hex(const std::string &s)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf(out, sizeof(out), "%016llx", static_cast<unsigned long long>(h));
    return out;
}

extern "C" void port_netleaderboard_report_result(const char *server,
                                                  const char *code,
                                                  unsigned int session,
                                                  const char *opp_name,
                                                  int won,
                                                  unsigned int duration_ticks)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    const std::string opp = port_lb::sanitize_lb_name(opp_name);
    const std::string me = port_lb::sanitize_lb_name(CVarGetString("gProfiles.Name", "Player"));
    if (base.empty() || opp.empty() || me.empty() || code == nullptr ||
        session == 0) {
        return;
    }
    /* Canonical pairing (lexicographic, not winner-first) so the winner's
     * and loser's clients derive the same id. Session + duration bind a
     * rematch to a distinct match. */
    const std::string lo = (std::min)(me, opp), hi = (std::max)(me, opp);
    char salt[384];
    std::snprintf(salt, sizeof(salt), "%s|%u|%s|%s|%u", code, session,
                  lo.c_str(), hi.c_str(), duration_ticks);
    const std::string match_id = fnv1a64_hex(salt);

    char key[40];
    port_netstats_player_key(key, sizeof(key));
    nlohmann::json payload = {
        {"name", me},
        {"opp_name", opp},
        {"key", key},
        {"match_id", match_id},
        {"won", won},
        {"duration", duration_ticks},
    };
    const std::string body = payload.dump();
    std::thread([base, body] {
        std::string resp, error;
        ssb64::CurlPostToString(base + "/result", body, "application/json",
                                &resp, &error, 15);
        /* fire-and-forget: pending/verified/mismatch are all fine — the
         * server's answer doesn't change anything client-side. */
    }).detach();
#else
    (void)server; (void)code; (void)session; (void)opp_name;
    (void)won; (void)duration_ticks;
#endif
}

extern "C" int port_netleaderboard_count(void)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    return static_cast<int>(sRows.size());
}

extern "C" int port_netleaderboard_at(int index, char *out_name, int capacity,
                                      int *rating, int *wins, int *losses,
                                      int *suspicious, int *banned)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (index < 0 || index >= static_cast<int>(sRows.size())) {
        return 0;
    }
    const LbRow &r = sRows[index];
    if (out_name != nullptr && capacity > 0) {
        std::snprintf(out_name, static_cast<size_t>(capacity), "%s", r.name.c_str());
    }
    if (rating) *rating = r.rating;
    if (wins) *wins = r.wins;
    if (losses) *losses = r.losses;
    if (suspicious) *suspicious = r.suspicious;
    if (banned) *banned = r.banned;
    return 1;
}

/* Community moderation report — POSTs {name, key} where key is OUR claim
 * key; the server requires it to back an existing entry, so we can only
 * report once we've claimed a name. `name` arrives verbatim from the
 * board rows — it is already server-sanitized, and re-running the
 * client's ASCII-only filter would mangle Unicode names (server accepts
 * \p{L}\p{N}) into a different target. Fire-and-forget: the board
 * refreshes on the next fetch anyway. */
extern "C" void port_netleaderboard_report(const char *server, const char *name)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    const std::string target = (name != nullptr) ? name : "";
    if (base.empty() || target.empty()) {
        return;
    }
    char key[40];
    port_netstats_player_key(key, sizeof(key));
    const std::string body = nlohmann::json{
        {"name", target}, {"key", key}}.dump();
    std::thread([base, body] {
        std::string resp, error;
        ssb64::CurlPostToString(base + "/report", body, "application/json",
                                &resp, &error, 15);
    }).detach();
#else
    (void)server; (void)name;
#endif
}

/* Replay sharing — uploads <path> (a .ssr recorded by netreplay's
 * auto-recorder) to POST /replay and remembers the share link for the UI.
 * share_state: 0 idle, 1 in flight, 2 ready (link in share_link), -1 failed. */
std::atomic<int>& sShareState = *new std::atomic<int>{0};
std::string& sShareLink = *new std::string;

extern "C" int port_netleaderboard_share_state(void) { return sShareState.load(); }
extern "C" int port_netleaderboard_share_link(char *out, int capacity)
{
    if (out == nullptr || capacity <= 0) return 0;
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    std::snprintf(out, static_cast<size_t>(capacity), "%s", sShareLink.c_str());
    return static_cast<int>(sShareLink.size());
}

#ifdef SSB64_HAVE_CURL
static int share_replay_upload(const std::string &base, const std::string &file,
                               const std::string &by)
{
    std::ifstream in(file, std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    if (!in || raw.empty() || raw.size() > 256 * 1024) {
        return -1;
    }
    const std::string body = nlohmann::json{
        {"data", ssb64::Base64Encode(raw.data(), raw.size())},
        {"by", by}}.dump();
    std::string resp, error;
    if (!ssb64::CurlPostToString(base + "/replay", body,
                                "application/json", &resp, &error, 20)) {
        return -1;
    }
    const nlohmann::json doc = nlohmann::json::parse(resp, nullptr, false);
    if (!doc.is_object() || !doc["id"].is_string()) {
        return -1;
    }
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    sShareLink = base + "/replay/" + doc["id"].get<std::string>();
    return 2;
}
#endif

extern "C" void port_netleaderboard_share_replay(const char *server,
                                                 const char *path)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || path == nullptr) {
        return;
    }
    if (sShareState.exchange(1) == 1) {
        return;                     /* already in flight — stay busy */
    }
    const std::string by = port_lb::sanitize_lb_name(CVarGetString("gProfiles.Name", "Player"));
    const std::string file(path);
    std::thread([base, file, by] {
        sShareState = share_replay_upload(base, file, by);
    }).detach();
#else
    (void)server; (void)path;
#endif
}

extern "C" int port_netleaderboard_my_rank(void)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (sMyClaimed.empty()) {
        return 0;
    }
    for (size_t i = 0; i < sRows.size(); i++) {
        if (sRows[i].name == sMyClaimed) {
            return static_cast<int>(i) + 1;
        }
    }
    return 0;
}

/* ── Match history — GET /history ─────────────────────────────────────── */

std::vector<PortLbMatchRow>& sHist = *new std::vector<PortLbMatchRow>;
std::atomic<int>& sHistState = *new std::atomic<int>{0};

extern "C" int port_netleaderboard_history_state(void)
{
    return sHistState.load();
}

/* "2026-10-19T12:34:56.789Z" -> "10-19 12:34" (UTC, trimmed for the row). */
extern "C" void port_netleaderboard_fetch_history(const char *server)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || sHistState.exchange(1) == 1) {
        return;                     /* no server configured / in flight */
    }
    port_lb::lb_fetch(base + "/history", sHistState, sHist,
        [](const nlohmann::json &doc, std::vector<PortLbMatchRow> &rows) {
            if (!doc.is_object() || !doc["matches"].is_array()) {
                return false;
            }
            for (const auto &m : doc["matches"]) {
                if (rows.size() >= 200) {
                    break;          /* cap: hostile server memory bound */
                }
                PortLbMatchRow r{};
                if (port_lb::parse_match_row(m, r)) {
                    rows.push_back(r);
                }
            }
            return true;
        });
#else
    (void)server;
#endif
}

extern "C" int port_netleaderboard_history_count(void)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    return static_cast<int>(sHist.size());
}

extern "C" int port_netleaderboard_history_at(int index, PortLbMatchRow *out)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (out == nullptr || index < 0 || index >= static_cast<int>(sHist.size())) {
        return 0;
    }
    *out = sHist[index];
    return 1;
}
/* ── Service stats (GET /stats) ──────────────────────────────────────── */

namespace {
struct LbStats {
    int players = -1;   /* live WS presence — self-hosted adapter only */
    int entries = 0;
    int matches = 0;
    int replays = 0;
    int builds = 0;
};
}
static LbStats& sStats = *new LbStats;
static std::atomic<int>& sStatsState = *new std::atomic<int>{0};

extern "C" void port_netleaderboard_fetch_stats(const char *server)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || sStatsState.exchange(1) == 1) {
        return;
    }
    port_lb::lb_fetch(base + "/stats", sStatsState, sStats,
        [](const nlohmann::json &doc, LbStats &out) {
            if (!doc.is_object()) {
                return false;
            }
            /* "players" is self-hosted only — absent keeps the -1
             * sentinel so the UI can hide the live-presence field. */
            out.players = port_lb::json_int_or(doc, "players", -1);
            out.entries = port_lb::json_int_or(doc, "leaderboard_entries", 0);
            out.matches = port_lb::json_int_or(doc, "verified_matches", 0);
            out.replays = port_lb::json_int_or(doc, "shared_replays", 0);
            out.builds  = port_lb::json_int_or(doc, "distinct_builds", 0);
            return true;
        });
#else
    (void)server;
#endif
}

extern "C" int port_netleaderboard_stats(int *players, int *entries,
                                         int *matches, int *replays,
                                         int *builds)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (sStatsState.load() != 2) {
        return 0;
    }
    if (players) *players = sStats.players;
    if (entries) *entries = sStats.entries;
    if (matches) *matches = sStats.matches;
    if (replays) *replays = sStats.replays;
    if (builds)  *builds  = sStats.builds;
    return 1;
}
