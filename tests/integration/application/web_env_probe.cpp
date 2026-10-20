/* web_env_probe — exercises the URL->env bridge in the REAL
 * port_web_stubs.cpp under __EMSCRIPTEN__ (compiled into this binary).
 *
 * The bridge is a pre-main __attribute__((constructor)) that evals a JS
 * snippet through emscripten_run_script_string and setenv()s the parsed
 * NAME=VALUE lines. The fake implements run_script_string by executing
 * the production JS verbatim under node with a stubbed
 * globalThis.location, so the alias table, SSB64_ whitelist regex and
 * newline/'=' injection filters are the shipped code — nothing is
 * re-implemented for the test.
 *
 * Since the constructor runs BEFORE main, the probe self-execs: parent
 * mode spawns children with FAKE_EMS_LOCATION_SEARCH=<query>; each
 * child (WEB_ENV_CHILD=1) simply reports getenv() for the watched names
 * and the parent asserts on the report.
 *
 * POSIX/WSL only — MSVC cannot compile __attribute__((constructor)).
 * Requires node on PATH (or FAKE_EMS_NODE=<cmd>). */

#include "smash_test/check.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

namespace {

const char *kWatch[] = {
    "SSB64_LOBBY_HOST",   "SSB64_LOBBY_JOIN",       "SSB64_LOBBY_CODE",
    "SSB64_LOBBY_SPECTATE", "SSB64_LOBBY_CODE_FILE", "SSB64_LOBBY_AUTOSTART",
    "SSB64_LOBBY_FKIND",  "SSB64_LOBBY_STAGE",      "SSB64_LOBBY_MIN_PLAYERS",
    "SSB64_LOBBY_FORCE_RELAY", "SSB64_OTHER",       "NOT_WHITELISTED",
};

using EnvMap = std::map<std::string, std::string>;

EnvMap run_child(const char *self, const char *search)
{
    /* env -i would also drop FAKE_EMS_NODE; keep PATH so node resolves. */
    std::string cmd = "WEB_ENV_CHILD=1 FAKE_EMS_LOCATION_SEARCH='";
    for (const char *p = search; *p != '\0'; ++p) {
        if (*p == '\'') {
            cmd += "'\\''";
        } else {
            cmd += *p;
        }
    }
    cmd += "' ";
    cmd += self;

    EnvMap env;
    FILE *pipe = popen(cmd.c_str(), "r");
    if (pipe == nullptr) {
        return env;
    }
    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        std::string line(buf);
        const size_t eq = line.find('=');
        if (eq != std::string::npos) {
            std::string val = line.substr(eq + 1);
            while (!val.empty() && (val.back() == '\n' || val.back() == '\r')) {
                val.pop_back();
            }
            env[line.substr(0, eq)] = val;
        }
    }
    pclose(pipe);
    return env;
}

std::string at(const EnvMap &m, const char *key)
{
    const auto it = m.find(key);
    return (it != m.end()) ? it->second : std::string();
}

bool unset(const EnvMap &m, const char *key)
{
    return at(m, key) == "<unset>";
}

} // namespace

int main(int argc, char **argv)
{
    if (getenv("WEB_ENV_CHILD") != nullptr) {
        for (const char *n : kWatch) {
            const char *v = getenv(n);
            std::printf("%s=%s\n", n, v != nullptr ? v : "<unset>");
        }
        return 0;
    }
    const char *self = (argc > 0) ? argv[0] : "./web_env_probe";

    /* C1: alias mapping + percent-decoding of a ws:// URL value. */
    {
        const EnvMap m = run_child(
            self, "?lobby_join=ABC123&lobby_host=ws%3A%2F%2Fx%2F&lobby_spectate=1");
        SMASH_CHECK(at(m, "SSB64_LOBBY_JOIN") == "ABC123");
        SMASH_CHECK(at(m, "SSB64_LOBBY_HOST") == "ws://x/");
        SMASH_CHECK(at(m, "SSB64_LOBBY_SPECTATE") == "1");
    }

    /* C2: raw SSB64_* name passes through verbatim. */
    {
        const EnvMap m = run_child(self, "?SSB64_LOBBY_CODE=XYZ789");
        SMASH_CHECK(at(m, "SSB64_LOBBY_CODE") == "XYZ789");
    }

    /* C3: every alias in the table maps. */
    {
        const EnvMap m = run_child(
            self, "?lobby_code_file=%2Ftmp%2Fc&lobby_autostart=2&"
                  "lobby_fkind=3&lobby_stage=4&lobby_min_players=5&"
                  "lobby_force_relay=1");
        SMASH_CHECK(at(m, "SSB64_LOBBY_CODE_FILE") == "/tmp/c");
        SMASH_CHECK(at(m, "SSB64_LOBBY_AUTOSTART") == "2");
        SMASH_CHECK(at(m, "SSB64_LOBBY_FKIND") == "3");
        SMASH_CHECK(at(m, "SSB64_LOBBY_STAGE") == "4");
        SMASH_CHECK(at(m, "SSB64_LOBBY_MIN_PLAYERS") == "5");
        SMASH_CHECK(at(m, "SSB64_LOBBY_FORCE_RELAY") == "1");
    }

    /* C4: injection filters — decoded newline or '=' kills the value. */
    {
        const EnvMap m = run_child(
            self, "?SSB64_LOBBY_JOIN=bad%0Aline&SSB64_LOBBY_CODE=a%3Db");
        SMASH_CHECK(unset(m, "SSB64_LOBBY_JOIN"));
        SMASH_CHECK(unset(m, "SSB64_LOBBY_CODE"));
    }

    /* C5: non-whitelisted params are dropped entirely. */
    {
        const EnvMap m = run_child(self, "?random=x&evil=SSB64_LOBBY_JOIN");
        SMASH_CHECK(unset(m, "SSB64_LOBBY_JOIN"));
        SMASH_CHECK(unset(m, "NOT_WHITELISTED"));
    }

    /* C6: alias lookup is case-insensitive (k.toLowerCase()), so an
     * uppercase 'LOBBY_JOIN' still maps — while a lowercase 'ssb64_'
     * name fails the strict uppercase whitelist regex. */
    {
        const EnvMap m = run_child(
            self, "?LOBBY_JOIN=X&ssb64_lobby_code=Y");
        SMASH_CHECK(at(m, "SSB64_LOBBY_JOIN") == "X");
        SMASH_CHECK(unset(m, "SSB64_LOBBY_CODE"));
    }

    /* C7: empty query — nothing applied. */
    {
        const EnvMap m = run_child(self, "");
        SMASH_CHECK(unset(m, "SSB64_LOBBY_JOIN"));
        SMASH_CHECK(unset(m, "SSB64_LOBBY_HOST"));
    }

    return smash_test_report("web_env_probe");
}
