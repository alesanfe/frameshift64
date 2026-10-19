/* netfriends_test — round-trip checks for the friends/block list store
 * (src/networking/netplay/domain/netfriends.cpp). Modes via argv:
 *
 *   fixture (default) — crafted netfriends.json (incl. an escaped-quote
 *                       name the old hand-rolled reader would have split)
 *                       → lazy load → getters → mutate → re-read the file.
 *   corrupt           — garbage file → defaults; first add rewrites valid
 *                       JSON.
 *
 * Same SSB64_SAVE_PATH seam as netstats — private temp dir per mode. */

#include "netfriends.h"
#include "port_save.h"
#include "smash_test/check.h"
#include "smash_test/env.h"

#include <cstdlib>
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

/* Test double for port_save_get_dir (netfriends.cpp no longer resolves
 * the save dir itself). Honors the SSB64_SAVE_PATH seam; "." fallback. */
extern "C" const char *port_save_get_dir(void) {
    static std::string dir;
    if (const char *o = std::getenv("SSB64_SAVE_PATH")) {
        dir = std::filesystem::path(o).parent_path().string();
    } else {
        dir = ".";
    }
    return dir.c_str();
}

namespace {

std::filesystem::path g_tmp;

nlohmann::json read_store()
{
    std::ifstream f(g_tmp / "netfriends.json");
    if (!f) {
        return nlohmann::json();
    }
    const std::string text{std::istreambuf_iterator<char>(f),
                           std::istreambuf_iterator<char>()};
    return nlohmann::json::parse(text, nullptr, /*exceptions*/ false);
}

void write_file(const std::string &text)
{
    std::filesystem::create_directories(g_tmp);
    std::ofstream(g_tmp / "netfriends.json", std::ios::trunc) << text;
}

const nlohmann::json *find_name(const nlohmann::json &doc, const char *name)
{
    const auto list = doc.find("friends");
    if (list == doc.end() || !list->is_array()) {
        return nullptr;
    }
    for (const auto &e : *list) {
        if (e.is_object() && e.value("name", "") == name) {
            return &e;
        }
    }
    return nullptr;
}

bool no_stray_tmp()
{
    for (const auto &e : std::filesystem::directory_iterator(g_tmp)) {
        if (e.path().filename().string().find(".tmp") != std::string::npos) {
            return false;
        }
    }
    return true;
}

void run_fixture()
{
    /* "A\"B" is a real JSON escape — the pre-nlohmann substring reader
     * would have stopped at the first inner quote and produced "A\". */
    write_file(
        "{\n"
        "  \"friends\": [\n"
        "    {\"name\": \"Foo\", \"added\": 10, \"blocked\": true},\n"
        "    {\"name\": \"bar\", \"added\": 20, \"blocked\": false},\n"
        "    {\"name\": \"A\\\"B\", \"added\": 30, \"blocked\": false}\n"
        "  ]\n"
        "}\n");

    SMASH_CHECK(port_netfriends_count() == 3);

    /* sorted case-insensitively: A"B < bar < Foo */
    PortNetFriend f{};
    SMASH_EXPECT(port_netfriends_get(0, &f));
    SMASH_CHECK(std::strcmp(f.name.data(), "A\"B") == 0);
    SMASH_CHECK(f.added_unix == 30 && !f.blocked);
    SMASH_EXPECT(port_netfriends_get(1, &f));
    SMASH_CHECK(std::strcmp(f.name.data(), "bar") == 0);
    SMASH_CHECK(f.added_unix == 20 && !f.blocked);
    SMASH_EXPECT(port_netfriends_get(2, &f));
    SMASH_CHECK(std::strcmp(f.name.data(), "Foo") == 0);
    SMASH_CHECK(f.blocked);
    SMASH_CHECK(!port_netfriends_get(3, &f));
    SMASH_CHECK(!port_netfriends_get(-1, &f));
    SMASH_CHECK(!port_netfriends_get(0, nullptr));

    SMASH_CHECK(port_netfriends_is_blocked("FOO"));      /* case-insensitive */
    SMASH_CHECK(!port_netfriends_is_blocked("bar"));
    SMASH_CHECK(!port_netfriends_is_blocked("ghost"));
    SMASH_CHECK(!port_netfriends_is_blocked(nullptr));
    SMASH_CHECK(!port_netfriends_is_blocked(""));

    /* add → auto-save; dup add is idempotent */
    SMASH_CHECK(port_netfriends_add("Zel"));
    SMASH_CHECK(port_netfriends_count() == 4);
    {
        const nlohmann::json doc = read_store();
        const nlohmann::json *e = find_name(doc, "Zel");
        SMASH_EXPECT(e != nullptr);
        SMASH_CHECK(e->value("blocked", true) == false);
        SMASH_CHECK(e->value("added", -1) > 0);
    }
    SMASH_CHECK(port_netfriends_add("zel"));             /* dup, ci */
    SMASH_CHECK(port_netfriends_count() == 4);

    SMASH_CHECK(port_netfriends_set_blocked("zEL", true));
    SMASH_CHECK(port_netfriends_is_blocked("Zel"));
    {
        const nlohmann::json doc = read_store();
        const nlohmann::json *e = find_name(doc, "Zel");
        SMASH_EXPECT(e != nullptr);
        SMASH_CHECK(e->value("blocked", false) == true);
    }
    SMASH_CHECK(!port_netfriends_set_blocked("ghost", true));

    SMASH_CHECK(port_netfriends_remove("FOO"));
    SMASH_CHECK(port_netfriends_count() == 3);
    SMASH_CHECK(!port_netfriends_remove("ghost"));
    {
        const nlohmann::json doc = read_store();
        SMASH_CHECK(find_name(doc, "Foo") == nullptr);
        SMASH_CHECK(find_name(doc, "bar") != nullptr);
        SMASH_CHECK(find_name(doc, "A\"B") != nullptr);  /* escape round-trips */
    }

    SMASH_CHECK(no_stray_tmp());
}

void run_corrupt()
{
    write_file("garbage [[[ not json");
    SMASH_CHECK(port_netfriends_count() == 0);
    SMASH_CHECK(port_netfriends_add("solo"));
    SMASH_CHECK(port_netfriends_count() == 1);
    {
        const nlohmann::json doc = read_store();
        SMASH_EXPECT(doc.is_object());
        SMASH_CHECK(find_name(doc, "solo") != nullptr);
    }
    SMASH_CHECK(no_stray_tmp());
}

} /* namespace */

int main(int argc, char **argv)
{
    const bool corrupt = argc > 1 && std::strcmp(argv[1], "corrupt") == 0;
    g_tmp = std::filesystem::temp_directory_path() /
            (std::string("ssb64_netfriends_") +
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
    return smash_test_report("netfriends_test");
}
