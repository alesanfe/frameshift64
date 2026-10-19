/* Tournament bracket client — split from netleaderboard.cpp for the
 * file-size cap. Wire contract: services/netplay-lobby/src/domain/
 * bracket.mjs (GET /tournament {brackets:[view]}, POST join/start/create).
 * Pairings settle server-side only through verified /result corroboration;
 * the client never decides a winner itself. */

#include "netleaderboard.h"
#include "netleaderboard_internal.h"
#include "../domain/netstats.h"          /* port_netstats_player_key */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>

#ifdef SSB64_HAVE_CURL
#include "http_curl.h"
#include <nlohmann/json.hpp>
#endif

using port_lb::sanitize_lb_name;
#ifdef SSB64_HAVE_CURL
using port_lb::json_str_copy;
using port_lb::json_int_or;
using port_lb::lb_fetch;
#endif

namespace {

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

} // namespace

extern "C" int port_netleaderboard_tournament_state(void)
{
    return sBracketsState.load();
}

#ifdef SSB64_HAVE_CURL
namespace {

void bracket_players_parse(const nlohmann::json &jv, BracketView &v)
{
    const auto pl = jv.find("players");
    if (pl == jv.end() || !pl->is_array()) {
        return;
    }
    for (const auto &p : *pl) {
        if (v.players.size() >= 64) break;
        if (p.is_string()) {
            v.players.push_back(p.get<std::string>().substr(0, 96));
        }
    }
}

void bracket_rounds_parse(const nlohmann::json &jv, BracketView &v)
{
    const auto rd = jv.find("rounds");
    if (rd == jv.end() || !rd->is_array()) {
        return;
    }
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

/* Parses the GET /tournament {brackets:[view]} payload — shared by the
 * plain fetch and the post-mutation refresh so both shapes stay aligned. */
bool bracket_parse(const nlohmann::json &doc, std::vector<BracketView> &out)
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
        bracket_players_parse(jv, v);
        bracket_rounds_parse(jv, v);
        if (!v.id.empty()) {
            out.push_back(std::move(v));
        }
    }
    return true;
}

} // namespace
#endif /* SSB64_HAVE_CURL */

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
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    return static_cast<int>(sBrackets.size());
}

extern "C" int port_netleaderboard_tournament_at(int index, PortLbBracket *out)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
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
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (index < 0 || index >= static_cast<int>(sBrackets.size())) {
        return 0;
    }
    return static_cast<int>(sBrackets[index].rounds.size());
}

extern "C" int port_netleaderboard_tournament_pairing(int index, int round,
                                                      int slot,
                                                      PortLbMatchRow *out)
{
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
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
    std::lock_guard<std::mutex> lk(port_lb::SharedLock());
    if (out == nullptr || capacity <= 0 || index < 0 || p < 0 ||
        index >= static_cast<int>(sBrackets.size()) ||
        p >= static_cast<int>(sBrackets[index].players.size())) {
        return 0;
    }
    std::snprintf(out, static_cast<size_t>(capacity), "%s",
                  sBrackets[index].players[p].c_str());
    return 1;
}

#ifdef SSB64_HAVE_CURL
namespace {

/* Shared POST-and-refresh: join/start/create mutate server state, then a
 * GET /tournament on the same worker thread lands the fresh view in
 * sBrackets (state stays through the fetch so the UI keeps polling). */
void tournament_post(const char *server, const std::string &path,
                     nlohmann::json payload)
{
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
            std::lock_guard<std::mutex> lk(port_lb::SharedLock());
            sBrackets = std::move(rows);
            sBracketsState = 2;
        } else {
            sBracketsState = -1;
        }
    }).detach();
}

} // namespace
#endif /* SSB64_HAVE_CURL */

extern "C" void port_netleaderboard_tournament_join(const char *server,
                                                    const char *id)
{
#ifdef SSB64_HAVE_CURL
    if (id == nullptr || id[0] == '\0') return;
    const std::string name =
        sanitize_lb_name(CVarGetString("gProfiles.Name", "Player"));
    if (name.empty()) return;
    tournament_post(server, "/tournament/join",
                    {{"id", id}, {"name", name}});
#else
    (void)server; (void)id;
#endif
}

extern "C" void port_netleaderboard_tournament_start(const char *server,
                                                     const char *id)
{
#ifdef SSB64_HAVE_CURL
    if (id == nullptr || id[0] == '\0') return;
    tournament_post(server, "/tournament/start", {{"id", id}});
#else
    (void)server; (void)id;
#endif
}

extern "C" void port_netleaderboard_tournament_create(const char *server,
                                                      const char *name,
                                                      int size)
{
#ifdef SSB64_HAVE_CURL
    const std::string by =
        sanitize_lb_name(CVarGetString("gProfiles.Name", "Player"));
    const std::string title =
        sanitize_lb_name((name != nullptr && name[0] != '\0') ? name : "Bracket");
    if (by.empty()) return;
    tournament_post(server, "/tournament",
                    {{"name", title}, {"by", by}, {"size", size}});
#else
    (void)server; (void)name; (void)size;
#endif
}
