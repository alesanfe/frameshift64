/* See netleaderboard.h. */

#include "netleaderboard.h"
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
 * order to race against. */
std::mutex& sLbLock = *new std::mutex;
std::vector<LbRow>& sRows = *new std::vector<LbRow>;
std::atomic<int>& sState = *new std::atomic<int>{0};      /* idle */
std::string& sMyClaimed = *new std::string;               /* name we submitted under (for rank) */

/* Mirror of sanitizeName() in services/netplay-lobby/src/domain/
 * leaderboard.mjs — the server strips anything outside its alphabet
 * (letters/digits + " _.-[]()", 24 chars, trimmed) before storing, so
 * we must compare ranks against the *sanitized* name, not the raw
 * profile string (a name with e.g. "?" or emojis would never match a
 * row and "Your rank" would stay hidden). ASCII-only isalnum is the
 * pragmatic subset of the server's \p{L}\p{N} class. */
bool lb_name_char(unsigned char u)
{
    return std::isalnum(u) != 0 || std::strchr(" _.-[]()", static_cast<char>(u)) != nullptr;
}

std::string sanitize_lb_name(const char *raw)
{
    std::string out;
    for (const char *p = raw != nullptr ? raw : ""; *p != '\0' && out.size() < 24; ++p) {
        const unsigned char u = static_cast<unsigned char>(*p);
        if (lb_name_char(u)) {
            out += static_cast<char>(u);
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}


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
    std::lock_guard<std::mutex> lk(sLbLock);
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
        std::lock_guard<std::mutex> lk(sLbLock);
        if (sState == 1) {
            return;                 /* already in flight */
        }
        sState = 1;
    }
    lb_fetch(base + "/leaderboard", sState, sRows,
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

    std::string claimed = sanitize_lb_name(name);
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
            std::lock_guard<std::mutex> lk(sLbLock);
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
    const std::string opp = sanitize_lb_name(opp_name);
    const std::string me = sanitize_lb_name(CVarGetString("gProfiles.Name", "Player"));
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
    std::lock_guard<std::mutex> lk(sLbLock);
    return static_cast<int>(sRows.size());
}

extern "C" int port_netleaderboard_at(int index, char *out_name, int capacity,
                                      int *rating, int *wins, int *losses,
                                      int *suspicious, int *banned)
{
    std::lock_guard<std::mutex> lk(sLbLock);
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
    std::lock_guard<std::mutex> lk(sLbLock);
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
    std::lock_guard<std::mutex> lk(sLbLock);
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
    const std::string by = sanitize_lb_name(CVarGetString("gProfiles.Name", "Player"));
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
    std::lock_guard<std::mutex> lk(sLbLock);
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
static void hist_stamp(const std::string &iso, char *out, size_t cap)
{
    out[0] = '\0';
    if (iso.size() >= 16 && iso[4] == '-' && iso[7] == '-' &&
        (iso[10] == 'T' || iso[10] == ' ')) {
        std::snprintf(out, cap, "%.5s %.5s",
                      iso.c_str() + 5, iso.c_str() + 11);
    }
}


/* Shared JSON member readers — server payloads are treated as hostile,
 * so every read is shape-checked before touching it. */
static bool json_str(const nlohmann::json &j, const char *k, std::string &out)
{
    const auto it = j.find(k);
    if (it == j.end() || !it->is_string()) {
        return false;
    }
    out = it->get<std::string>();
    return true;
}

static void json_str_to(const nlohmann::json &j, const char *k,
                        char *dst, size_t cap, size_t max_len)
{
    std::string v;
    if (json_str(j, k, v)) {
        std::snprintf(dst, cap, "%s", v.substr(0, max_len).c_str());
    }
}

static void json_str_copy(const nlohmann::json &j, const char *k,
                          std::string &dst, size_t cap)
{
    std::string v;
    if (json_str(j, k, v)) {
        dst = v.substr(0, cap);
    }
}

static int json_int_or(const nlohmann::json &j, const char *k, int dflt)
{
    const auto it = j.find(k);
    return (it != j.end() && it->is_number()) ? it->get<int>() : dflt;
}

/* One verified match row — /history entries and /player.matches share
 * this exact wire shape. */
static bool parse_match_row(const nlohmann::json &m, PortLbMatchRow &r)
{
    if (!m.is_object()) {
        return false;
    }
    json_str_to(m, "a", r.a, sizeof(r.a), 96);
    json_str_to(m, "b", r.b, sizeof(r.b), 96);
    json_str_to(m, "w", r.w, sizeof(r.w), 96);
    json_str_to(m, "l", r.l, sizeof(r.l), 96);
    r.draw = (m.contains("draw") && m["draw"].is_boolean() &&
              m["draw"].get<bool>()) ? 1 : 0;
    std::string at;
    json_str(m, "at", at);
    hist_stamp(at, r.stamp, sizeof(r.stamp));
    return true;
}

/* Detached-GET skeleton shared by every read-only fetcher: worker
 * thread -> GET -> shape-checked parse -> lock -> store -> state.
 * `parse` fills `out` and returns false to mark the fetch failed. The
 * slots are leaked on purpose (see sLbLock), so references captured by
 * the detached thread stay valid. */
template <typename T, typename Parse>
static void lb_fetch(const std::string &url, std::atomic<int> &state,
                     T &slot, Parse &&parse)
{
    std::thread([url, &state, &slot, parse] {
        std::string body, error;
        T result;
        int st = 2;
        const nlohmann::json doc = ssb64::CurlGetToString(
            url, &body, &error, {}, 15)
                ? nlohmann::json::parse(body, nullptr, /*exceptions*/ false)
                : nlohmann::json();
        if (doc.is_discarded() || !parse(doc, result)) {
            st = -1;
        }
        std::lock_guard<std::mutex> lk(sLbLock);
        if (st == 2) {
            slot = std::move(result);
        }
        state = st;
    }).detach();
}

extern "C" void port_netleaderboard_fetch_history(const char *server)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || sHistState.exchange(1) == 1) {
        return;                     /* no server configured / in flight */
    }
    lb_fetch(base + "/history", sHistState, sHist,
        [](const nlohmann::json &doc, std::vector<PortLbMatchRow> &rows) {
            if (!doc.is_object() || !doc["matches"].is_array()) {
                return false;
            }
            for (const auto &m : doc["matches"]) {
                if (rows.size() >= 200) {
                    break;          /* cap: hostile server memory bound */
                }
                PortLbMatchRow r{};
                if (parse_match_row(m, r)) {
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
    std::lock_guard<std::mutex> lk(sLbLock);
    return static_cast<int>(sHist.size());
}

extern "C" int port_netleaderboard_history_at(int index, PortLbMatchRow *out)
{
    std::lock_guard<std::mutex> lk(sLbLock);
    if (out == nullptr || index < 0 || index >= static_cast<int>(sHist.size())) {
        return 0;
    }
    *out = sHist[index];
    return 1;
}

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
    lb_fetch(base + "/player?name=" + qname, sCardState, sCard,
        [](const nlohmann::json &doc, PlayerCard &card) {
            if (!doc.is_object() || !doc["name"].is_string()) {
                return false;
            }
            card.name = doc["name"].get<std::string>().substr(0, 96);
            card.rank = json_int_or(doc, "rank", 0);
            card.rating = json_int_or(doc, "rating", 0);
            card.wins = json_int_or(doc, "wins", 0);
            card.losses = json_int_or(doc, "losses", 0);
            const auto it = doc.find("matches");
            if (it != doc.end() && it->is_array()) {
                for (const auto &m : *it) {
                    if (card.matches.size() >= 200) {
                        break;
                    }
                    PortLbMatchRow r{};
                    if (parse_match_row(m, r)) {
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
    std::lock_guard<std::mutex> lk(sLbLock);
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
    std::lock_guard<std::mutex> lk(sLbLock);
    return static_cast<int>(sCard.matches.size());
}

extern "C" int port_netleaderboard_player_match_at(int index,
                                                   PortLbMatchRow *out)
{
    std::lock_guard<std::mutex> lk(sLbLock);
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
    lb_fetch(base + "/replays", sReplaysState, sSharedReplays,
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
                if (json_str(m, "id", v)) r.id = v.substr(0, 32);
                if (json_str(m, "by", v)) r.by = v.substr(0, 96);
                char stamp[24];
                json_str(m, "at", v);
                hist_stamp(v, stamp, sizeof(stamp));
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
    std::lock_guard<std::mutex> lk(sLbLock);
    return static_cast<int>(sSharedReplays.size());
}

extern "C" int port_netleaderboard_replays_at(int index, char *id, int idCap,
                                              char *by, int byCap,
                                              char *stamp, int stCap)
{
    std::lock_guard<std::mutex> lk(sLbLock);
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
    std::lock_guard<std::mutex> lk(sLbLock);
    std::snprintf(out, static_cast<size_t>(capacity), "%s", sReplayDlPath.c_str());
    return static_cast<int>(sReplayDlPath.size());
}

extern "C" void port_netleaderboard_replay_download(const char *server,
                                                    const char *id)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || id == nullptr || id[0] == '\0') {
        return;
    }
    /* The id is a server-minted slug — keep it to the service's alphabet
     * ([0-9a-z]) so it can't smuggle a path traversal into the filename. */
    for (const char *p = id; *p; ++p) {
        if (!std::isalnum(static_cast<unsigned char>(*p))) {
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
            const nlohmann::json doc =
                nlohmann::json::parse(body, nullptr, /*exceptions*/ false);
            const auto data = doc.is_object() ? doc.find("data") : doc.end();
            if (doc.is_object() && data != doc.end() && data->is_string()) {
                std::vector<uint8_t> bytes;
                if (ssb64::Base64Decode(data->get<std::string>(), bytes) &&
                    !bytes.empty() && bytes.size() <= 400 * 1024) {
                    const std::string dir =
                        std::string(port_save_get_dir()) + "/replays";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    const std::string path =
                        dir + "/shared-" + safeId + ".ssr";
                    std::ofstream out(path, std::ios::binary | std::ios::trunc);
                    out.write(reinterpret_cast<const char *>(bytes.data()),
                              static_cast<std::streamsize>(bytes.size()));
                    if (out.good()) {
                        saved = path;
                        state = 2;
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(sLbLock);
            sReplayDlPath = saved;
        }
        sReplayDlState = state;
    }).detach();
#else
    (void)server; (void)id;
#endif
}

/* ── Tournaments — GET /tournament, POST join/start/create ────────────── */

struct BracketPairing {
    std::string a, b, winner;
};

struct BracketView {
    std::string id, name, owner, status, winner;
    int size = 0;
    std::vector<std::string> players;
    std::vector<std::vector<BracketPairing>> rounds;
};

std::vector<BracketView>& sBrackets = *new std::vector<BracketView>;
std::atomic<int>& sBracketsState = *new std::atomic<int>{0};

extern "C" int port_netleaderboard_tournament_state(void)
{
    return sBracketsState.load();
}

/* Parses the GET /tournament {brackets:[view]} payload — shared by the
 * plain fetch and the post-mutation refresh so both shapes stay aligned. */
static bool bracket_parse(const nlohmann::json &doc,
                          std::vector<BracketView> &out)
{
    const auto arr = doc.is_object() ? doc.find("brackets") : doc.end();
    if (!doc.is_object() || arr == doc.end() || !arr->is_array()) {
        return false;
    }
    for (const auto &jv : *arr) {
        if (out.size() >= 64 || !jv.is_object()) {
            break;                  /* host-input bound */
        }
        BracketView v;
        json_str_copy(jv, "id", v.id, 15);
        json_str_copy(jv, "name", v.name, 63);
        json_str_copy(jv, "owner", v.owner, 127);
        json_str_copy(jv, "status", v.status, 11);
        json_str_copy(jv, "winner", v.winner, 127);
        v.size = json_int_or(jv, "size", 0);
        const auto pl = jv.find("players");
        if (pl != jv.end() && pl->is_array()) {
            for (const auto &p : *pl) {
                if (v.players.size() >= 64) break;
                if (p.is_string()) {
                    v.players.push_back(p.get<std::string>().substr(0, 96));
                }
            }
        }
        const auto rd = jv.find("rounds");
        if (rd != jv.end() && rd->is_array()) {
            for (const auto &rj : *rd) {
                if (v.rounds.size() >= 8) break;
                std::vector<BracketPairing> round;
                if (rj.is_array()) {
                    for (const auto &pj : rj) {
                        if (round.size() >= 32 || !pj.is_object()) break;
                        BracketPairing pr;
                        json_str_copy(pj, "a", pr.a, 96);
                        json_str_copy(pj, "b", pr.b, 96);
                        json_str_copy(pj, "winner", pr.winner, 96);
                        round.push_back(std::move(pr));
                    }
                }
                v.rounds.push_back(std::move(round));
            }
        }
        if (!v.id.empty()) {
            out.push_back(std::move(v));
        }
    }
    return true;
}

extern "C" void port_netleaderboard_fetch_tournaments(const char *server)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty() || sBracketsState.exchange(1) == 1) {
        return;
    }
    lb_fetch(base + "/tournament", sBracketsState, sBrackets, bracket_parse);
#else
    (void)server;
#endif
}

extern "C" int port_netleaderboard_tournament_count(void)
{
    std::lock_guard<std::mutex> lk(sLbLock);
    return static_cast<int>(sBrackets.size());
}

extern "C" int port_netleaderboard_tournament_at(int index, PortLbBracket *out)
{
    std::lock_guard<std::mutex> lk(sLbLock);
    if (out == nullptr || index < 0 ||
        index >= static_cast<int>(sBrackets.size())) {
        return 0;
    }
    const BracketView &v = sBrackets[index];
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->id, sizeof(out->id), "%s", v.id.c_str());
    std::snprintf(out->name, sizeof(out->name), "%s", v.name.c_str());
    std::snprintf(out->owner, sizeof(out->owner), "%s", v.owner.c_str());
    std::snprintf(out->status, sizeof(out->status), "%s", v.status.c_str());
    std::snprintf(out->winner, sizeof(out->winner), "%s", v.winner.c_str());
    out->size = v.size;
    out->joined = static_cast<int>(v.players.size());
    const char *me = CVarGetString("gProfiles.Name", "Player");
    const std::string mine = sanitize_lb_name(me);
    out->owner_mine = (v.owner == mine);
    out->mine = out->owner_mine ||
                std::find(v.players.begin(), v.players.end(), mine)
                    != v.players.end();
    return 1;
}

extern "C" int port_netleaderboard_tournament_rounds(int index)
{
    std::lock_guard<std::mutex> lk(sLbLock);
    if (index < 0 || index >= static_cast<int>(sBrackets.size())) {
        return 0;
    }
    return static_cast<int>(sBrackets[index].rounds.size());
}

extern "C" int port_netleaderboard_tournament_pairing(int index, int round,
                                                      int slot,
                                                      PortLbMatchRow *out)
{
    std::lock_guard<std::mutex> lk(sLbLock);
    if (out == nullptr || index < 0 || round < 0 || slot < 0 ||
        index >= static_cast<int>(sBrackets.size())) {
        return 0;
    }
    const BracketView &v = sBrackets[index];
    if (round >= static_cast<int>(v.rounds.size()) ||
        slot >= static_cast<int>(v.rounds[round].size())) {
        return 0;
    }
    const BracketPairing &p = v.rounds[round][slot];
    std::memset(out, 0, sizeof(*out));
    std::snprintf(out->a, sizeof(out->a), "%s", p.a.c_str());
    std::snprintf(out->b, sizeof(out->b), "%s", p.b.c_str());
    std::snprintf(out->w, sizeof(out->w), "%s", p.winner.c_str());
    return 1;
}

extern "C" int port_netleaderboard_tournament_player_at(int index, int p,
                                                        char *out,
                                                        int capacity)
{
    std::lock_guard<std::mutex> lk(sLbLock);
    if (out == nullptr || capacity <= 0 || index < 0 || p < 0 ||
        index >= static_cast<int>(sBrackets.size()) ||
        p >= static_cast<int>(sBrackets[index].players.size())) {
        return 0;
    }
    std::snprintf(out, static_cast<size_t>(capacity), "%s",
                  sBrackets[index].players[p].c_str());
    return 1;
}

/* Shared POST-and-refresh: join/start/create mutate server state, then a
 * GET /tournament on the same worker thread lands the fresh view in
 * sBrackets (state stays through the fetch so the UI keeps polling). */
static void tournament_post(const char *server, const std::string &path,
                            nlohmann::json payload)
{
#ifdef SSB64_HAVE_CURL
    const std::string base = ssb64::HttpBaseFromWsServer(server);
    if (base.empty()) {
        return;
    }
    char key[40];
    port_netstats_player_key(key, sizeof(key));
    payload["key"] = key;
    const std::string body = payload.dump();
    if (sBracketsState.exchange(1) == 1) {
        return;                     /* mutation or fetch already running */
    }
    std::thread([base, path, body] {
        std::string resp, error;
        if (!ssb64::CurlPostToString(base + path, body, "application/json",
                                     &resp, &error, 15)) {
            sBracketsState = -1;
            return;
        }
        std::vector<BracketView> rows;
        if (ssb64::CurlGetToString(base + "/tournament", &resp, &error,
                                   {}, 15) &&
            bracket_parse(nlohmann::json::parse(resp, nullptr, false), rows)) {
            std::lock_guard<std::mutex> lk(sLbLock);
            sBrackets = std::move(rows);
            sBracketsState = 2;
        } else {
            sBracketsState = -1;
        }
    }).detach();
#else
    (void)server; (void)path; (void)payload;
#endif
}

extern "C" void port_netleaderboard_tournament_join(const char *server,
                                                    const char *id)
{
    if (id == nullptr || id[0] == '\0') return;
    const std::string name =
        sanitize_lb_name(CVarGetString("gProfiles.Name", "Player"));
    if (name.empty()) return;
    tournament_post(server, "/tournament/join",
                    {{"id", id}, {"name", name}});
}

extern "C" void port_netleaderboard_tournament_start(const char *server,
                                                     const char *id)
{
    if (id == nullptr || id[0] == '\0') return;
    tournament_post(server, "/tournament/start", {{"id", id}});
}

extern "C" void port_netleaderboard_tournament_create(const char *server,
                                                      const char *name,
                                                      int size)
{
    const std::string by =
        sanitize_lb_name(CVarGetString("gProfiles.Name", "Player"));
    const std::string title =
        sanitize_lb_name((name != nullptr && name[0] != '\0') ? name : "Bracket");
    if (by.empty()) return;
    tournament_post(server, "/tournament",
                    {{"name", title}, {"by", by}, {"size", size}});
}
