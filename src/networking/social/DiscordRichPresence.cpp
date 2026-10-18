// Discord Rich Presence over Discord's documented local IPC —
// replaces the archived discord-rpc SDK (and its bundled rapidjson).
// The protocol is a duplex stream of [u32le opcode][u32le len][json]
// frames over a named pipe (Windows: \\.\pipe\discord-ipc-N) or a
// unix socket (POSIX: $XDG_RUNTIME_DIR/discord-ipc-N). We speak only
// the three verbs this game uses: HANDSHAKE, FRAME (SET_ACTIVITY)
// and PING/PONG keepalive.

#include "enhancements.h"
#include <stdint.h>
#include <time.h>
#include <string.h>

// Include your port's logging system
#include "port_log.h"

// LibUltraShip includes for CVar access
#include <libultraship/libultraship.h>
#include <libultraship/bridge.h>

#include <string>

#include <nlohmann/json.hpp>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#define DISCORD_CLIENT_ID "1503877021615390741"

extern "C" {
    // Fetches the current scene ID from the game engine
    unsigned char port_diag_get_scene_curr(void);
}

namespace {

// IPC opcodes
constexpr uint32_t kOpHandshake = 0;
constexpr uint32_t kOpFrame = 1;
constexpr uint32_t kOpClose = 2;
constexpr uint32_t kOpPing = 3;
constexpr uint32_t kOpPong = 4;

constexpr int kPipeCount = 10;          // discord-ipc-0 .. discord-ipc-9
constexpr int kHandshakeTimeoutMs = 2000;
constexpr int kRetryCooldownSec = 15;

#ifdef _WIN32
HANDLE g_conn = INVALID_HANDLE_VALUE;
#else
int g_conn = -1;
#endif
bool g_drpInitialized = false;
int64_t g_startTime = 0;
time_t g_nextRetry = 0;                 // reconnect throttle
unsigned g_nonce = 0;
std::string g_recv;                     // partial-frame accumulator

bool ConnOpen() {
#ifdef _WIN32
    return g_conn != INVALID_HANDLE_VALUE;
#else
    return g_conn >= 0;
#endif
}

void ConnClose() {
#ifdef _WIN32
    if (g_conn != INVALID_HANDLE_VALUE) CloseHandle(g_conn);
    g_conn = INVALID_HANDLE_VALUE;
#else
    if (g_conn >= 0) close(g_conn);
    g_conn = -1;
#endif
    g_recv.clear();
}

// Returns false when the connection is dead (caller closes + retries).
bool ConnWrite(const void* data, size_t len) {
#ifdef _WIN32
    DWORD wrote = 0;
    return WriteFile(g_conn, data, (DWORD)len, &wrote, NULL) && wrote == len;
#else
    return write(g_conn, data, len) == (ssize_t)len;
#endif
}

// Bytes readable without blocking (0 = none, -1 = connection dead).
long ConnReadable() {
#ifdef _WIN32
    DWORD avail = 0;
    if (!PeekNamedPipe(g_conn, NULL, 0, NULL, &avail, NULL)) return -1;
    return (long)avail;
#else
    pollfd pfd{g_conn, POLLIN, 0};
    if (poll(&pfd, 1, 0) <= 0) return pfd.revents & (POLLERR | POLLHUP | POLLNVAL) ? -1 : 0;
    return pfd.revents & POLLIN ? 1 : 0;
#endif
}

// Append up to `want` readable bytes to g_recv. `want`==0 drains all
// currently available. Returns false on connection death.
bool ConnReadSome(size_t want) {
#ifdef _WIN32
    DWORD avail = 0;
    if (!PeekNamedPipe(g_conn, NULL, 0, NULL, &avail, NULL)) return false;
    if (want == 0 || want > avail) want = avail;
    if (want == 0) return true;
    size_t old = g_recv.size();
    g_recv.resize(old + want);
    DWORD got = 0;
    if (!ReadFile(g_conn, g_recv.data() + old, (DWORD)want, &got, NULL) || got == 0) {
        g_recv.resize(old);
        return false;
    }
    g_recv.resize(old + got);
    return true;
#else
    char buf[4096];
    ssize_t n = recv(g_conn, buf, want ? (want < sizeof(buf) ? want : sizeof(buf)) : sizeof(buf),
                     MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) return false;
    if (n > 0) g_recv.append(buf, (size_t)n);
    return true;
#endif
}

bool WriteFrame(uint32_t op, const std::string& json) {
    uint8_t hdr[8];
    uint32_t len = (uint32_t)json.size();
    for (int i = 0; i < 4; i++) {
        hdr[i] = (uint8_t)((op >> (8 * i)) & 0xff);
        hdr[4 + i] = (uint8_t)((len >> (8 * i)) & 0xff);
    }
    return ConnWrite(hdr, 8) && (len == 0 || ConnWrite(json.data(), len));
}

// Pop the next complete frame in g_recv, if any.
bool PopFrame(uint32_t& op, std::string& payload) {
    if (g_recv.size() < 8) return false;
    const uint8_t* h = (const uint8_t*)g_recv.data();
    uint32_t len = 0;
    for (int i = 0; i < 4; i++) len |= (uint32_t)h[4 + i] << (8 * i);
    if (len > 16 * 1024 * 1024) return false;     // absurd — stream is corrupt
    if (g_recv.size() < 8 + len) return false;
    op = 0;
    for (int i = 0; i < 4; i++) op |= (uint32_t)h[i] << (8 * i);
    payload.assign(g_recv, 8, len);
    g_recv.erase(0, 8 + len);
    return true;
}

// Answer PINGs and watch for CLOSE; drop the connection on stream errors.
void DrainFrames() {
    while (ConnReadable() > 0 && ConnReadSome(0)) {
        uint32_t op;
        std::string payload;
        while (PopFrame(op, payload)) {
            if (op == kOpPing) {
                if (!WriteFrame(kOpPong, payload)) goto dead;
            } else if (op == kOpClose) {
                goto dead;
            }
        }
    }
    if (ConnReadable() < 0) goto dead;
    return;
dead:
    ConnClose();
}

bool WaitForData(int timeout_ms) {
#ifdef _WIN32
    const int step = 10;
    for (int waited = 0; waited < timeout_ms; waited += step) {
        long r = ConnReadable();
        if (r < 0) return false;
        if (r > 0) return true;
        Sleep(step);
    }
    return ConnReadable() > 0;
#else
    pollfd pfd{g_conn, POLLIN, 0};
    int r;
    do {
        r = poll(&pfd, 1, timeout_ms);
    } while (r < 0 && errno == EINTR);
    return r > 0 && (pfd.revents & POLLIN);
#endif
}

// Open the IPC stream: first discord-ipc-N endpoint that accepts.
bool OpenEndpoint() {
#ifdef _WIN32
    for (int i = 0; i < kPipeCount; i++) {
        char path[64];
        snprintf(path, sizeof(path), "\\\\.\\pipe\\discord-ipc-%d", i);
        HANDLE h = CreateFileA(path, GENERIC_READ | GENERIC_WRITE,
                               0, NULL, OPEN_EXISTING, 0, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            g_conn = h;
            return true;
        }
    }
    return false;
#else
    const char* dirs[] = { getenv("XDG_RUNTIME_DIR"), getenv("TMPDIR"),
                           getenv("TMP"), "/tmp" };
    for (const char* dir : dirs) {
        if (!dir || !*dir) continue;
        for (int i = 0; i < kPipeCount; i++) {
            char path[108];                 // sockaddr_un.sun_path bound
            snprintf(path, sizeof(path), "%s/discord-ipc-%d", dir, i);
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            if (fd < 0) return false;
            sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
            if (connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0) {
                g_conn = fd;
                return true;
            }
            close(fd);
        }
    }
    return false;
#endif
}

// ---------------------------------------------------------
// INTERNAL LIFECYCLE
// ---------------------------------------------------------

void DRP_Init() {
    if (g_drpInitialized) return;
    g_drpInitialized = true;
    g_startTime = time(NULL);

    if (!OpenEndpoint()) {
        port_log("SSB64 DRP: Discord IPC endpoint not found (client not running?).\n");
        return;
    }

    nlohmann::json hello;
    hello["v"] = 1;
    hello["client_id"] = DISCORD_CLIENT_ID;
    if (!WriteFrame(kOpHandshake, hello.dump())) {
        ConnClose();
        return;
    }
    // Bounded wait for the READY event — if Discord stalls we stay
    // "initialized but disconnected" and the tick retries later.
    if (WaitForData(kHandshakeTimeoutMs) && ConnReadSome(0)) {
        uint32_t op;
        std::string payload;
        if (PopFrame(op, payload)) {
            const nlohmann::json resp =
                nlohmann::json::parse(payload, nullptr, /*exceptions*/ false);
            const auto evt = resp.find("evt");
            const auto cmd = resp.find("cmd");
            const bool ready =
                (evt != resp.end() && evt->is_string() &&
                 evt->get_ref<const std::string&>() == "READY") ||
                (cmd != resp.end() && cmd->is_string() &&
                 cmd->get_ref<const std::string&>() == "DISPATCH");
            if (ready) {
                std::string name;
                const auto data = resp.find("data");
                if (data != resp.end() && data->is_object()) {
                    const auto user = data->find("user");
                    if (user != data->end() && user->is_object()) {
                        const auto un = user->find("username");
                        if (un != user->end() && un->is_string())
                            name = un->get_ref<const std::string&>();
                    }
                }
                port_log("SSB64 DRP: Connected to Discord%s%s\n",
                         name.empty() ? "" : " as ", name.c_str());
                return;
            }
            // A non-READY first frame (e.g. an error) — reconnect later.
        }
    }
    ConnClose();
}

void DRP_Shutdown() {
    if (!g_drpInitialized) return;

    port_log("SSB64 DRP: Shutting down Discord presence.\n");
    if (ConnOpen()) {
        // Graceful close frame, then drop the pipe.
        WriteFrame(kOpClose, "{}");
        ConnClose();
    }
    g_drpInitialized = false;
    g_nextRetry = 0;
}

} // namespace

// ---------------------------------------------------------
// PUBLIC FUNCTIONS
// ---------------------------------------------------------

namespace ssb64 {
    namespace enhancements {

        void UpdateDiscordPresence(const char* gameState, const char* matchDetails) {
            bool isDRPEnabled = CVarGetInteger("gSettings.Menu.EnableDRP", 0) != 0;

            if (!isDRPEnabled) {
                if (g_drpInitialized) {
                    port_log("SSB64 DRP: CVar turned off, triggering shutdown.\n");
                    DRP_Shutdown();
                }
                return;
            }

            if (!g_drpInitialized) {
                DRP_Init();
            }
            if (!ConnOpen()) {
                // Reconnect attempts are throttled — opening a pipe is cheap
                // but we still don't want a per-frame syscall storm while
                // Discord isn't running.
                const time_t now = time(NULL);
                if (now < g_nextRetry) return;
                g_nextRetry = now + kRetryCooldownSec;
                g_drpInitialized = false;   // let DRP_Init run again
                DRP_Init();                 // re-handshake
                if (!ConnOpen()) return;
            }

            DrainFrames();
            if (!ConnOpen()) return;        // pipe dropped mid-drain

            port_log("SSB64 DRP: Updating presence -> State: '%s' | Details: '%s'\n", gameState, matchDetails);

            nlohmann::json activity;
            activity["state"] = gameState;
            activity["details"] = matchDetails;
            activity["timestamps"]["start"] = g_startTime;
            activity["assets"]["large_image"] = "battleship_logo";
            activity["assets"]["large_text"] = "Unofficial Smash 64 PC Port";

            nlohmann::json frame;
            frame["cmd"] = "SET_ACTIVITY";
            frame["args"]["pid"] =
#ifdef _WIN32
                (int)GetCurrentProcessId();
#else
                (int)getpid();
#endif
            frame["args"]["activity"] = std::move(activity);
            frame["nonce"] = std::to_string(++g_nonce);
            if (!WriteFrame(kOpFrame, frame.dump())) ConnClose();
        }

        static void UpdatePresenceForScene(unsigned char sceneId) {
            switch (sceneId) {
                // --- MENUS ---
                case 1:  // Title
                    UpdateDiscordPresence("Title Screen", "Press Start");
                    break;
                case 7:  // Main Menu
                case 8:  // 1P Mode Menu
                case 9:  // VS Mode Menu
                case 10: // VS Options
                case 11: // VS Item Switch
                case 15: // Screen Adjust
                    UpdateDiscordPresence("In Menus", "Choosing a Game Mode");
                    break;
                case 25: // Records Screen
                case 26: // Character Data Screen
                    UpdateDiscordPresence("In Menus", "Looking at Stats");
                    break;

                // --- CHARACTER SELECT (CSS) ---
                case 16: // VS Character Select
                    // --- 1P / TRAINING CHARACTER SELECT ---
                case 17: // 1P Game Players
                    UpdateDiscordPresence("CSS", "Choosing a Fighter");
                    break;
                case 18: // Training Mode Players
                    UpdateDiscordPresence("Training Mode", "Choosing a Fighter");
                    break;
                case 19: // Bonus 1 Players
                    UpdateDiscordPresence("Break the Targets", "Choosing a Fighter");
                    break;
                case 20: // Bonus 2 Players
                    UpdateDiscordPresence("Board the Platforms", "Choosing a Fighter");
                    break;

                // --- STAGE SELECT ---
                case 21: // Stage Select
                    UpdateDiscordPresence("Stage Select", "Picking a Stage");
                    break;

                // --- IN-GAME & RESULTS ---
                case 14: // 1P VS Screen (The "VS" splash before a fight)

                // --- GAMEPLAY ---
                case 22: // VS Battle In-Game
                    UpdateDiscordPresence("Versus Mode", "Brawling");
                    break;
                case 23: // 1P Gameplay (Classic, Training, Bonus)
                    UpdateDiscordPresence("Single Player", "In a Match");
                    break;
                case 24: // VS Results Screen
                    UpdateDiscordPresence("Post-Match", "Viewing Results");
                    break;
                case 13: // Challenger Approaching Screen
                    UpdateDiscordPresence("Challenger Approaching!", "A New Foe Has Appeared");
                    break;

                // --- INTRO & CUTSCENES ---
                case 27: // N64 Logo / Startup
                case 28: // Opening Room (Master Hand desk)
                case 29: case 30: case 31: case 32: case 33: case 34:
                case 35: case 36: case 37: case 38: case 39: case 40:
                case 41: case 42: case 43: case 44: case 45: case 46:
                    UpdateDiscordPresence("Watching the Intro", "Nostalgia Trip");
                    break;

                // --- FALLBACK (Including Debug Menus & Messages) ---
                case 0:  // No Controller Screen
                case 12: // Unlock Message
                default:
                    UpdateDiscordPresence("in the game", "Somewhere");
                    break;
            }
        }

        void TickDiscordPresence() {
            // 1. Read the menu toggle
            bool isDRPEnabled = CVarGetInteger("gSettings.Menu.EnableDRP", 0) != 0;

            // Remember the last scene so we don't spam Discord's API every frame
            static unsigned char s_lastScene = 255;

            if (isDRPEnabled) {
                // Fetch the current scene from the game engine
                unsigned char currentScene = port_diag_get_scene_curr();

                // If the scene changed (or the game just booted), push an update!
                if (currentScene != s_lastScene) {
                    s_lastScene = currentScene;
                    UpdatePresenceForScene(currentScene);
                }

                // Drain PING/CLOSE frames if we're connected
                if (g_drpInitialized && ConnOpen()) {
                    DrainFrames();
                }
            }
            else if (g_drpInitialized) {
                // If the player unchecks the box mid-game, shut it down immediately
                DRP_Shutdown();

                // Reset our tracker so it updates instantly if they check the box again
                s_lastScene = 255;
            }
        }

    } // namespace enhancements
} // namespace ssb64
