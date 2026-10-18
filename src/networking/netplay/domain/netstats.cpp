/* See netstats.h. Storage: <save dir>/netstats.json — same directory the
 * SRAM image resolves to (SSB64_SAVE_PATH honored), so stats survive the
 * same way saves do. The file is ours alone, never shared with the engine. */

#include "netstats.h"
#include "atomic_write.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include <ship/Context.h>

#include <nlohmann/json.hpp>

extern "C" void port_log(const char *fmt, ...);

namespace {

constexpr int kStartRating = 1200;
constexpr int kEloK = 32;
constexpr int kProvisionalMatches = 20;
constexpr size_t kMaxHistory = 100;
constexpr size_t kMaxRecentRooms = 10;

struct MatchRow {
    int won = 0;                    /* 1 win, 0 loss, -1 draw */
    int opp_rating = -1;            /* declared by peer in PICK; -1 unknown */
    unsigned int duration_ticks = 0;
    unsigned long long ts = 0;
};

struct SeasonRow {
    std::string id;                 /* "YYYY-MM" */
    int rating = 0;
    int wins = 0;
    int losses = 0;
};

struct Stats {
    float rating = kStartRating;
    int wins = 0;
    int losses = 0;
    int draws = 0;
    int opp_rating = -1;            /* last peer's declared rating */
    std::vector<MatchRow> history;  /* newest first */
    std::vector<std::string> recent_rooms;
    std::string key;                /* leaderboard claim token (random hex) */
    std::string season;             /* active season "YYYY-MM" */
    std::vector<SeasonRow> seasons; /* archived past seasons, newest first */
    bool loaded = false;
    bool dirty = false;
};

Stats sStats;
std::mutex sStatsLock;

/* Resolve netstats.json next to the SRAM file — same precedence as
 * port_save.cpp (env override, then the app-directory convention). */
std::string stats_path()
{
    std::string save;
    if (const char *override = std::getenv("SSB64_SAVE_PATH")) {
        save = override;
    } else {
        try {
            save = Ship::Context::GetPathRelativeToAppDirectory("ssb64_save.bin");
        } catch (...) {
            save.clear();
        }
    }
    std::filesystem::path p = save.empty()
        ? std::filesystem::path("netstats.json")
        : std::filesystem::path(save).parent_path() / "netstats.json";
    return p.string();
}

/* Tolerant field readers over a parsed doc — older files wrote a few fields
 * quoted ("wins": "5"), and a hand-edited file shouldn't take the stats
 * down. Real JSON: escaped quotes, nested values and key names inside
 * strings are all handled correctly by the parser. */
double field_num(const nlohmann::json &o, const char *key, double fallback)
{
    if (!o.is_object()) {
        return fallback;
    }
    const auto it = o.find(key);
    if (it == o.end()) {
        return fallback;
    }
    if (it->is_number()) {
        return it->get<double>();
    }
    if (it->is_string()) {
        return std::strtod(it->get_ref<const std::string&>().c_str(), nullptr);
    }
    return fallback;
}

unsigned long long field_u64(const nlohmann::json &o, const char *key,
                             unsigned long long fallback)
{
    if (!o.is_object()) {
        return fallback;
    }
    const auto it = o.find(key);
    if (it == o.end()) {
        return fallback;
    }
    if (it->is_number_unsigned()) {
        return it->get<unsigned long long>();
    }
    if (it->is_number_integer()) {
        const long long v = it->get<long long>();
        return (v >= 0) ? static_cast<unsigned long long>(v) : fallback;
    }
    if (it->is_number_float()) {
        return static_cast<unsigned long long>(it->get<double>());
    }
    if (it->is_string()) {
        return std::strtoull(it->get_ref<const std::string&>().c_str(), nullptr, 10);
    }
    return fallback;
}

std::string field_str(const nlohmann::json &o, const char *key)
{
    if (!o.is_object()) {
        return std::string();
    }
    const auto it = o.find(key);
    return (it != o.end() && it->is_string())
        ? it->get_ref<const std::string&>() : std::string();
}

void load()
{
    std::ifstream f(stats_path());
    if (!f) {
        return;
    }
    const std::string text{std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>()};
    const nlohmann::json doc =
        nlohmann::json::parse(text, nullptr, /*exceptions*/ false);
    if (!doc.is_object()) {
        return;                 /* malformed/corrupt file — keep defaults */
    }
    sStats.rating = static_cast<float>(field_num(doc, "rating", kStartRating));
    sStats.wins = static_cast<int>(field_num(doc, "wins", 0));
    sStats.losses = static_cast<int>(field_num(doc, "losses", 0));
    sStats.draws = static_cast<int>(field_num(doc, "draws", 0));
    sStats.key = field_str(doc, "key");
    sStats.season = field_str(doc, "season");

    /* archived seasons: {"id":"2026-09","r":1450,"w":10,"l":8} */
    const auto seasons = doc.find("seasons");
    if (seasons != doc.end() && seasons->is_array()) {
        for (const nlohmann::json &row : *seasons) {
            if (!row.is_object() || sStats.seasons.size() >= 24) {
                break;
            }
            SeasonRow s;
            s.id = field_str(row, "id");
            s.rating = static_cast<int>(field_num(row, "r", 0));
            s.wins = static_cast<int>(field_num(row, "w", 0));
            s.losses = static_cast<int>(field_num(row, "l", 0));
            if (!s.id.empty()) {
                sStats.seasons.push_back(s);
            }
        }
    }

    /* history rows: {"w":1,"or":1234,"d":1234,"t":1720000000} */
    const auto history = doc.find("history");
    if (history != doc.end() && history->is_array()) {
        for (const nlohmann::json &row : *history) {
            if (!row.is_object() || sStats.history.size() >= kMaxHistory) {
                break;
            }
            MatchRow m;
            m.won = static_cast<int>(field_num(row, "w", 0));
            m.opp_rating = static_cast<int>(field_num(row, "or", -1));
            m.duration_ticks = static_cast<unsigned int>(field_u64(row, "d", 0));
            m.ts = field_u64(row, "t", 0);
            sStats.history.push_back(m);
        }
    }

    /* recent room codes: "recent":["ABC123",...] */
    const auto recent = doc.find("recent");
    if (recent != doc.end() && recent->is_array()) {
        for (const nlohmann::json &c : *recent) {
            if (sStats.recent_rooms.size() >= kMaxRecentRooms) {
                break;
            }
            if (c.is_string()) {
                sStats.recent_rooms.push_back(c.get_ref<const std::string&>());
            }
        }
    }
}

void ensure_loaded()
{
    if (!sStats.loaded) {
        sStats.loaded = true;
        load();
    }
}

void save()
{
    if (!sStats.dirty) {
        return;
    }
    const std::string path = stats_path();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    /* Serialize with the real JSON writer (proper string escaping) and
     * commit atomically — a mid-write crash strands a .tmp, never a torn
     * netstats.json. */
    nlohmann::json doc;
    doc["version"] = 1;
    doc["rating"] = std::lround(static_cast<double>(sStats.rating) * 100.0) / 100.0;
    doc["wins"] = sStats.wins;
    doc["losses"] = sStats.losses;
    doc["draws"] = sStats.draws;
    doc["key"] = sStats.key;
    doc["season"] = sStats.season;
    doc["seasons"] = nlohmann::json::array();
    for (const SeasonRow &s : sStats.seasons) {
        doc["seasons"].push_back(
            {{"id", s.id}, {"r", s.rating}, {"w", s.wins}, {"l", s.losses}});
    }
    doc["history"] = nlohmann::json::array();
    for (const MatchRow &m : sStats.history) {
        doc["history"].push_back({{"w", m.won},
                                  {"or", m.opp_rating},
                                  {"d", m.duration_ticks},
                                  {"t", m.ts}});
    }
    doc["recent"] = nlohmann::json::array();
    for (const std::string &c : sStats.recent_rooms) {
        doc["recent"].push_back(c);
    }
    if (!ssb64::WriteFileAtomic(path, doc.dump(2))) {
        port_log("SSB64 Stats: could not write %s\n", path.c_str());
        return;
    }
    sStats.dirty = false;
}

unsigned long long now_unix()
{
    return static_cast<unsigned long long>(
        std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
}

} // namespace

extern "C" int port_netstats_rating(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    return static_cast<int>(std::lround(sStats.rating));
}

extern "C" int port_netstats_wins(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    return sStats.wins;
}

extern "C" int port_netstats_losses(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    return sStats.losses;
}

extern "C" int port_netstats_draws(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    return sStats.draws;
}

extern "C" int port_netstats_provisional(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    const int n = sStats.wins + sStats.losses + sStats.draws;
    return (n < kProvisionalMatches) ? 1 : 0;
}

extern "C" int port_netstats_opponent_rating(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    return sStats.opp_rating;
}

extern "C" void port_netstats_set_opponent_rating(int rating)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    sStats.opp_rating = rating;
}

/* "YYYY-MM" — monthly seasons. A new season archives the previous one's
 * record and soft-resets rating halfway to the start value, the standard
 * Elo season decay. */
static std::string current_season()
{
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%04d-%02d", tm.tm_year + 1900, tm.tm_mon + 1);
    return buf;
}

static void season_rollover()
{
    const std::string cur = current_season();
    if (sStats.season == cur) {
        return;
    }
    if (!sStats.season.empty()) {
        SeasonRow s;
        s.id = sStats.season;
        s.rating = static_cast<int>(std::lround(sStats.rating));
        s.wins = sStats.wins;
        s.losses = sStats.losses;
        sStats.seasons.insert(sStats.seasons.begin(), s);
        if (sStats.seasons.size() > 24) {
            sStats.seasons.resize(24);
        }
        sStats.rating = kStartRating + (sStats.rating - kStartRating) * 0.5f;
        sStats.wins = sStats.losses = sStats.draws = 0;
        port_log("SSB64 Stats: new season %s — rating soft-reset to %d\n",
                 cur.c_str(), static_cast<int>(std::lround(sStats.rating)));
    }
    sStats.season = cur;
}

/* Random 32-hex claim token for the lobby leaderboard — generated once,
 * persisted; it is a write-key, not an identity proof. */
static void ensure_key()
{
    if (!sStats.key.empty()) {
        return;
    }
    std::random_device rd;
    std::mt19937_64 gen((static_cast<unsigned long long>(rd()) << 32) ^ rd());
    char key[33];
    for (int i = 0; i < 32; i += 16) {
        std::snprintf(key + i, 17, "%016llx",
                      static_cast<unsigned long long>(gen()));
    }
    key[32] = '\0';
    sStats.key = key;
    sStats.dirty = true;
}

extern "C" void port_netstats_record_match(int my_slot, int winner_slot,
                                           unsigned int duration_ticks)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    ensure_key();
    season_rollover();

    MatchRow m;
    m.won = (winner_slot < 0) ? -1 : (winner_slot == my_slot) ? 1 : 0;
    m.opp_rating = sStats.opp_rating;
    m.duration_ticks = duration_ticks;
    m.ts = now_unix();
    sStats.history.insert(sStats.history.begin(), m);
    if (sStats.history.size() > kMaxHistory) {
        sStats.history.resize(kMaxHistory);
    }
    if (m.won > 0) {
        sStats.wins++;
    } else if (m.won == 0) {
        sStats.losses++;
    } else {
        sStats.draws++;
    }

    /* Elo vs the peer's declared rating; unknown opponents count at the
     * starting rating but with half weight (provisional). The declared
     * value is peer-controlled, so the formula sees only a sanity band —
     * a sandbagger claiming rating=1 can't make your loss cost ~full K. */
    const float opp = (m.opp_rating > 0)
        ? std::clamp(static_cast<float>(m.opp_rating), 400.0f, 2800.0f)
        : static_cast<float>(kStartRating);
    const float expected = 1.0f / (1.0f + std::pow(10.0f, (opp - sStats.rating) / 400.0f));
    const float score = (m.won > 0) ? 1.0f : (m.won < 0) ? 0.5f : 0.0f;
    const float k = (m.opp_rating > 0) ? kEloK : kEloK / 2.0f;
    sStats.rating += k * (score - expected);

    sStats.dirty = true;
    save();
}

extern "C" int port_netstats_history_count(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    return static_cast<int>(sStats.history.size());
}

extern "C" int port_netstats_history_at(int index, int *won, int *opp_rating,
                                        unsigned int *duration_ticks,
                                        unsigned long long *unix_ts)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    if (index < 0 || index >= static_cast<int>(sStats.history.size())) {
        return 0;
    }
    const MatchRow &m = sStats.history[index];
    if (won) *won = m.won;
    if (opp_rating) *opp_rating = m.opp_rating;
    if (duration_ticks) *duration_ticks = m.duration_ticks;
    if (unix_ts) *unix_ts = m.ts;
    return 1;
}

extern "C" int port_netstats_recent_room_count(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    return static_cast<int>(sStats.recent_rooms.size());
}

extern "C" int port_netstats_recent_room_at(int index, char *out_code, int capacity)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    if (index < 0 || index >= static_cast<int>(sStats.recent_rooms.size()) ||
        out_code == nullptr || capacity <= 0) {
        return 0;
    }
    std::snprintf(out_code, static_cast<size_t>(capacity), "%s",
                  sStats.recent_rooms[index].c_str());
    return 1;
}

extern "C" void port_netstats_note_room(const char *code)
{
    if (code == nullptr || *code == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    /* The code arrives server-issued over the wire; it is written back out
     * as a raw quoted JSON string in save(), so keep it alphanumeric —
     * anything else would corrupt netstats.json (and can never be a real
     * room code anyway). */
    std::string c;
    for (const char *p = code; *p != '\0' && c.size() < 16; p++) {
        const unsigned char u = static_cast<unsigned char>(*p);
        if (std::isalnum(u)) {
            c += static_cast<char>(std::toupper(u));
        }
    }
    if (c.empty()) {
        return;
    }
    auto it = std::find(sStats.recent_rooms.begin(), sStats.recent_rooms.end(), c);
    if (it != sStats.recent_rooms.end()) {
        sStats.recent_rooms.erase(it);
    }
    sStats.recent_rooms.insert(sStats.recent_rooms.begin(), c);
    if (sStats.recent_rooms.size() > kMaxRecentRooms) {
        sStats.recent_rooms.resize(kMaxRecentRooms);
    }
    sStats.dirty = true;
    save();
}

extern "C" int port_netstats_player_key(char *out, int capacity)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    ensure_key();
    if (out == nullptr || capacity <= 0) {
        return 0;
    }
    std::snprintf(out, static_cast<size_t>(capacity), "%s", sStats.key.c_str());
    return static_cast<int>(sStats.key.size());
}

extern "C" int port_netstats_season(char *out, int capacity)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    const std::string &s = sStats.season.empty() ? current_season() : sStats.season;
    if (out == nullptr || capacity <= 0) {
        return 0;
    }
    std::snprintf(out, static_cast<size_t>(capacity), "%s", s.c_str());
    return static_cast<int>(s.size());
}

extern "C" int port_netstats_season_count(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    return static_cast<int>(sStats.seasons.size());
}

extern "C" int port_netstats_season_at(int index, char *out_id, int capacity,
                                       int *rating, int *wins, int *losses)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    if (index < 0 || index >= static_cast<int>(sStats.seasons.size())) {
        return 0;
    }
    const SeasonRow &s = sStats.seasons[index];
    if (out_id != nullptr && capacity > 0) {
        std::snprintf(out_id, static_cast<size_t>(capacity), "%s", s.id.c_str());
    }
    if (rating) *rating = s.rating;
    if (wins) *wins = s.wins;
    if (losses) *losses = s.losses;
    return 1;
}

extern "C" int port_netstats_rank_tier(void)
{
    std::lock_guard<std::mutex> lk(sStatsLock);
    ensure_loaded();
    if (sStats.history.size() < kProvisionalMatches) {
        return 0;                                    /* Unranked */
    }
    const int r = sStats.rating;
    if (r >= 2200) return 7;                         /* Grandmaster */
    if (r >= 2000) return 6;                         /* Master */
    if (r >= 1800) return 5;                         /* Diamond */
    if (r >= 1600) return 4;                         /* Platinum */
    if (r >= 1400) return 3;                         /* Gold */
    if (r >= 1200) return 2;                         /* Silver */
    return 1;                                        /* Bronze */
}

extern "C" int port_netstats_rank_tier_name(char *out, int capacity)
{
    static const char *kNames[] = {
        "Unranked", "Bronze", "Silver", "Gold",
        "Platinum", "Diamond", "Master", "Grandmaster",
    };
    const int tier = port_netstats_rank_tier();
    const char *name = kNames[tier];
    if (out == nullptr || capacity <= 0) {
        return 0;
    }
    const int len = static_cast<int>(std::strlen(name));
    std::snprintf(out, static_cast<size_t>(capacity), "%s", name);
    return len;
}
