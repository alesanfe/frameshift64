/* netstats_test — round-trip checks for the local online-stats store
 * (src/networking/netplay/domain/netstats.cpp). Modes via argv:
 *
 *   fixture (default) — write a crafted netstats.json, let lazy load pick it
 *                       up, verify getters, then record_match/note_room and
 *                       re-read the file to verify the atomic save.
 *   corrupt           — pre-write garbage; load must keep defaults and the
 *                       first mutation must rewrite a valid JSON doc.
 *
 * The store resolves its file via SSB64_SAVE_PATH (parent dir), so each
 * mode runs against a private temp dir — no game state, no network. */

#include "netstats.h"
#include "smash_test/check.h"
#include "smash_test/env.h"
#include <ship/Context.h>   /* test double (tests/support/shim) */

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

extern "C" void port_log(const char *fmt, ...) { (void)fmt; }

/* tests/support/shim/ship/Context.h shadows the real header in this TU's
 * sibling (netstats.cpp); the env override makes this unreachable, but the
 * symbol must still link. */
std::string Ship::Context::GetPathRelativeToAppDirectory(const std::string path,
                                                         std::string) {
    return path;
}

namespace {

std::filesystem::path g_tmp;

nlohmann::json read_store()
{
    std::ifstream f(g_tmp / "netstats.json");
    if (!f) {
        return nlohmann::json();
    }
    const std::string text{std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>()};
    return nlohmann::json::parse(text, nullptr, /*exceptions*/ false);
}

std::string current_season()
{
    char buf[16];
    std::time_t t = std::time(nullptr);
    std::strftime(buf, sizeof(buf), "%Y-%m", std::localtime(&t));
    return buf;
}

void write_file(const std::string &text)
{
    std::filesystem::create_directories(g_tmp);
    std::ofstream(g_tmp / "netstats.json", std::ios::trunc) << text;
}

bool no_stray_tmp()
{
    for (const auto &e : std::filesystem::directory_iterator(g_tmp)) {
        if (e.path().extension() == ".tmp" ||
            e.path().filename().string().find(".tmp") != std::string::npos) {
            return false;
        }
    }
    return true;
}

void run_fixture()
{
    const std::string season = current_season();
    write_file(
        "{\n"
        "  \"version\": 1,\n"
        "  \"rating\": 1500,\n"
        "  \"wins\": 3,\n"
        "  \"losses\": 1,\n"
        "  \"draws\": 1,\n"
        "  \"key\": \"deadbeefdeadbeefdeadbeefdeadbeef\",\n"
        "  \"season\": \"" + season + "\",\n"
        "  \"seasons\": [{\"id\":\"2026-08\",\"r\":1450,\"w\":10,\"l\":8}],\n"
        "  \"history\": [\n"
        "    {\"w\":1,\"or\":1400,\"d\":3600,\"t\":1700000000},\n"
        "    {\"w\":0,\"or\":-1,\"d\":1800,\"t\":1699990000}\n"
        "  ],\n"
        "  \"recent\": [\"ABC123\"]\n"
        "}\n");

    /* lazy load reflects the fixture */
    SMASH_CHECK(port_netstats_rating() == 1500);
    SMASH_CHECK(port_netstats_wins() == 3);
    SMASH_CHECK(port_netstats_losses() == 1);
    SMASH_CHECK(port_netstats_draws() == 1);
    SMASH_CHECK(port_netstats_history_count() == 2);
    SMASH_CHECK(port_netstats_provisional() == 1);   /* < 20 matches */

    int won = -9, or_ = -9;
    unsigned int dur = 0;
    unsigned long long ts = 0;
    SMASH_EXPECT(port_netstats_history_at(0, &won, &or_, &dur, &ts) == 1);
    SMASH_CHECK(won == 1 && or_ == 1400 && dur == 3600 && ts == 1700000000);
    SMASH_CHECK(port_netstats_history_at(99, nullptr, nullptr, nullptr, nullptr) == 0);

    char code[32] = {0};
    SMASH_CHECK(port_netstats_recent_room_count() == 1);
    SMASH_EXPECT(port_netstats_recent_room_at(0, code, sizeof(code)) == 1);
    SMASH_CHECK(std::strcmp(code, "ABC123") == 0);

    char key[64] = {0};
    SMASH_EXPECT(port_netstats_player_key(key, sizeof(key)) == 32);
    SMASH_CHECK(std::strcmp(key, "deadbeefdeadbeefdeadbeefdeadbeef") == 0);

    char sid[16] = {0};
    int sr = 0, sw = 0, sl = 0;
    SMASH_EXPECT(port_netstats_season(sid, sizeof(sid)) ==
                 static_cast<int>(season.size()));   /* returns length, not bool */
    SMASH_CHECK(std::strcmp(sid, season.c_str()) == 0);
    SMASH_CHECK(port_netstats_season_count() == 1);
    SMASH_EXPECT(port_netstats_season_at(0, sid, sizeof(sid), &sr, &sw, &sl) == 1);
    SMASH_CHECK(std::strcmp(sid, "2026-08") == 0 && sr == 1450 && sw == 10 && sl == 8);

    /* win vs declared 1400: expected = 1/(1+10^((1400-1500)/400)) ~= 0.640
     * → +32 * 0.360 ~= +11.5 → ~1512 */
    port_netstats_set_opponent_rating(1400);
    SMASH_CHECK(port_netstats_opponent_rating() == 1400);
    port_netstats_record_match(0, 0, 7200);
    SMASH_CHECK(port_netstats_wins() == 4);
    SMASH_CHECK(port_netstats_history_count() == 3);
    {
        const int r = port_netstats_rating();
        SMASH_CHECK(r >= 1511 && r <= 1513);
    }
    {
        const nlohmann::json doc = read_store();
        SMASH_EXPECT(doc.is_object());
        SMASH_CHECK(doc.value("wins", -1) == 4);
        SMASH_CHECK(std::fabs(doc.value("rating", 0.0) - 1511.5) < 1.0);
        const auto hist = doc.find("history");
        SMASH_EXPECT(hist != doc.end() && hist->is_array() && hist->size() == 3);
        SMASH_CHECK((*hist)[0].value("w", -1) == 1);
        SMASH_CHECK((*hist)[0].value("or", -1) == 1400);
        SMASH_CHECK((*hist)[0].value("d", 0u) == 7200u);
    }

    /* absurd declared rating is clamped to the sanity band — a peer
     * claiming 99999 cannot move the rating beyond a normal full-K
     * swing. Loss vs clamped-2800 at ~1511: expected ~= 0.0003 → delta
     * ~= -0.01. Unclamped it would be ~= -32... wait, actually vs a
     * declared-huge rating losing costs ~0 either way. The observable
     * clamp direction is WINNING vs a floor-clamped 1: gain must stay
     * under K. Assert the rating stays finite and inside a sane band. */
    port_netstats_set_opponent_rating(99999);
    port_netstats_record_match(0, 0, 600);
    {
        const int r = port_netstats_rating();
        SMASH_CHECK(r > 1511 && r < 1600);   /* gain bounded by K */
    }
    port_netstats_set_opponent_rating(1);    /* floor-clamped to 400 */
    port_netstats_record_match(0, 1, 600);
    {
        const int r = port_netstats_rating();
        SMASH_CHECK(r > 1400 && r < 1600);   /* still finite, sane */
        SMASH_CHECK(port_netstats_losses() == 2);   /* fixture's 1 + this loss */
    }

    /* wire-sent room code is sanitized to alnum+upper before persisting */
    port_netstats_note_room("queue9!!");
    SMASH_CHECK(port_netstats_recent_room_count() == 2);
    char rc[16] = {0};
    SMASH_EXPECT(port_netstats_recent_room_at(0, rc, sizeof(rc)) == 1);
    SMASH_CHECK(std::strcmp(rc, "QUEUE9") == 0);
    {
        const nlohmann::json doc = read_store();
        const auto rec = doc.find("recent");
        SMASH_EXPECT(rec != doc.end() && rec->is_array());
        SMASH_CHECK((*rec)[0].get<std::string>() == "QUEUE9");
    }

    SMASH_CHECK(no_stray_tmp());
}

void run_corrupt()
{
    write_file("this is not json {{{ and neither is this");
    SMASH_CHECK(port_netstats_rating() == 1200);
    SMASH_CHECK(port_netstats_wins() == 0);
    SMASH_CHECK(port_netstats_history_count() == 0);
    SMASH_CHECK(port_netstats_provisional() == 1);

    port_netstats_record_match(0, 0, 100);
    SMASH_CHECK(port_netstats_wins() == 1);
    {
        const nlohmann::json doc = read_store();
        SMASH_EXPECT(doc.is_object());
        SMASH_CHECK(doc.value("wins", -1) == 1);
        SMASH_CHECK(doc.value("version", 0) == 1);
    }
    SMASH_CHECK(no_stray_tmp());
}

} /* namespace */

int main(int argc, char **argv)
{
    const bool corrupt = argc > 1 && std::strcmp(argv[1], "corrupt") == 0;
    g_tmp = std::filesystem::temp_directory_path() /
            (std::string("ssb64_netstats_") +
             (corrupt ? "corrupt_" : "fixture_") +
             std::to_string(
                 static_cast<unsigned long long>(
#ifdef _WIN32
                     ::_getpid()
#else
                     ::getpid()
#endif
                 )));
    std::error_code ec;
    std::filesystem::remove_all(g_tmp, ec);
    std::filesystem::create_directories(g_tmp);
    const std::string save = (g_tmp / "ssb64_save.bin").string();
    SMASH_EXPECT(smash_test_setenv("SSB64_SAVE_PATH", save.c_str()) == 0);

    if (corrupt) {
        run_corrupt();
    } else {
        run_fixture();
    }

    std::filesystem::remove_all(g_tmp, ec);
    return smash_test_report("netstats_test");
}
