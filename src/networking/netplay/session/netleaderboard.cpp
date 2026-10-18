/* See netleaderboard.h. */

#include "netleaderboard.h"
#include "../domain/netstats.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <ship/Context.h>

#include <libultraship/bridge/consolevariablebridge.h>

#ifdef SSB64_HAVE_CURL
#include "http_curl.h"
#include <nlohmann/json.hpp>
#endif

namespace {

struct LbRow {
    std::string name;
    int rating = 0;
    int wins = 0;
    int losses = 0;
    int suspicious = 0;      /* server smurf-heuristic flag */
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
    std::thread([base] {
        std::string body, error;
        std::vector<LbRow> rows;
        int state = 2;
        if (ssb64::CurlGetToString(base + "/leaderboard", &body, &error,
                                   {}, 15)) {
            const nlohmann::json doc =
                nlohmann::json::parse(body, nullptr, /*exceptions*/ false);
            /* operator[]("entries") on a non-object doc is UB — check the
             * shape first (a hostile server could send anything). */
            if (doc.is_object() && doc["entries"].is_array()) {
                parse_lb_entries(doc["entries"], rows);
            } else {
                state = -1;
            }
        } else {
            state = -1;
        }
        std::lock_guard<std::mutex> lk(sLbLock);
        if (state == 2) {
            sRows = std::move(rows);
        }
        sState = state;
    }).detach();
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
                                      int *suspicious)
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
    return 1;
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
