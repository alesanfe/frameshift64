/* See netlobby.h. Protocol with server/lobby-server.js uses "SSBR" lines;
 * peer-to-peer lobby messages use "SSBL" lines on the same UDP socket:
 *   SSBL HELLO | SSBL HELLOACK               punch / keepalive
 *   SSBL PICK <fkind> <time_ms>              character pick + RTT probe
 *   SSBL PONG <time_ms>                       immediate echo of a PICK's time
 *   SSBL START <stage> <stocks> <items> <seed> <delay> <hostfk> <hostcos> <clientfk> <clientcos>
 *   SSBL GO <seed>                            client accepted START */
#include "netlobby.h"
#include "port_udp.h"
#include "port_log.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

extern "C" void syNetPeerStartOnlineMatch(int is_host, unsigned int delay, unsigned int stage, unsigned int stocks,
                                          int is_items, unsigned int seed, int host_fkind, int host_costume,
                                          int client_fkind, int client_costume);

namespace {

constexpr unsigned int kCreateRetryMs = 500;
constexpr unsigned int kPingMs = 2000;
constexpr unsigned int kHelloMs = 100;
constexpr unsigned int kPunchTimeoutMs = 3000;
constexpr unsigned int kRelayTimeoutMs = 5000;
constexpr unsigned int kPickMs = 250;
constexpr unsigned int kStartRetryMs = 100;
constexpr unsigned int kPeerTimeoutMs = 10000;

struct Lobby {
    PortNetLobbyState state = PORT_NETLOBBY_IDLE;
    bool is_host = false;
    std::string server;
    std::string code;
    std::string nonce;
    std::string status = "Not connected";
    std::vector<std::string> candidates;    // peer public + LAN endpoints
    std::string peer;                       // chosen endpoint ("relay" when relayed)
    bool relay_requested = false;
    unsigned int state_ms = 0;
    unsigned int last_send_ms = 0;
    unsigned int last_ping_ms = 0;
    unsigned int last_heard_ms = 0;
    int my_fkind = 0;
    int peer_fkind = -1;
    int stage = 0;
    int stocks = 3;
    int items = 0;
    int delay = 2;
    unsigned int seed = 0;
    int ping_ms = -1;
};

Lobby sLobby;

/* Unattended testing (scripts/netplay_lobbytest.ps1):
 *   SSB64_LOBBY_HOST=<server>  host a room; writes the code to SSB64_LOBBY_CODE_FILE
 *   SSB64_LOBBY_JOIN=<server>  join the code read from SSB64_LOBBY_CODE_FILE
 *   SSB64_LOBBY_FKIND / SSB64_LOBBY_STAGE / SSB64_LOBBY_AUTOSTART=1 */
struct LobbyAuto {
    bool checked = false;
    bool host = false;
    bool join = false;
    bool autostart = false;
    bool code_written = false;
    std::string server;
    std::string code_file;
    unsigned int start_after_ms = 0;
};
LobbyAuto sAuto;

const char *env(const char *name)
{
    const char *value = std::getenv(name);
    return (value != nullptr && value[0] != '\0') ? value : nullptr;
}

void set_state(PortNetLobbyState state, const std::string &status)
{
    sLobby.state = state;
    sLobby.status = status;
    sLobby.state_ms = port_udp_now_ms();
    sLobby.last_send_ms = 0;
    port_log("SSB64 Lobby: %s\n", status.c_str());
}

void fail(const std::string &why)
{
    set_state(PORT_NETLOBBY_FAILED, why);
    port_udp_close();
}

bool elapsed(unsigned int since, unsigned int ms)
{
    return static_cast<int>(port_udp_now_ms() - since) >= static_cast<int>(ms);
}

void send_server(const std::string &text)
{
    port_udp_send_to(sLobby.server.c_str(), text.data(), static_cast<int>(text.size()));
}

void send_peer(const std::string &text)
{
    if (sLobby.peer == "relay" || (!sLobby.peer.empty() && sLobby.state >= PORT_NETLOBBY_CONNECTED)) {
        port_udp_send(text.data(), static_cast<int>(text.size()));
    } else if (!sLobby.peer.empty()) {
        port_udp_send_to(sLobby.peer.c_str(), text.data(), static_cast<int>(text.size()));
    }
}

bool open_socket(const std::string &server)
{
    port_udp_close();
    if (!port_udp_open("0.0.0.0:0")) {
        fail("Couldn't open a UDP socket");
        return false;
    }
    sLobby.server = server;
    std::random_device rd;
    sLobby.nonce = std::to_string(rd());
    sLobby.peer.clear();
    sLobby.candidates.clear();
    sLobby.relay_requested = false;
    sLobby.peer_fkind = -1;
    sLobby.ping_ms = -1;
    return true;
}

std::string local_endpoint(void)
{
    char text[64];
    return port_udp_local_endpoint(text, sizeof(text)) ? std::string(text) : std::string("-");
}

void connect_to(const std::string &endpoint)
{
    sLobby.peer = endpoint;
    if (endpoint != "relay") {
        port_udp_set_peer(endpoint.c_str());
    }
    sLobby.last_heard_ms = port_udp_now_ms();
    set_state(PORT_NETLOBBY_CONNECTED,
              std::string("Connected") + (endpoint == "relay" ? " (via relay)" : " (direct)") +
              (sLobby.is_host ? " - press Start match" : " - waiting for host to start"));
}

void start_as_client(const std::vector<std::string> &args)
{
    /* START <stage> <stocks> <items> <seed> <delay> <hostfk> <hostcos> <clientfk> <clientcos> */
    if (args.size() < 11) {
        return;
    }
    unsigned int stage = std::strtoul(args[2].c_str(), nullptr, 10);
    unsigned int stocks = std::strtoul(args[3].c_str(), nullptr, 10);
    int items = std::atoi(args[4].c_str());
    unsigned int seed = std::strtoul(args[5].c_str(), nullptr, 10);
    unsigned int delay = std::strtoul(args[6].c_str(), nullptr, 10);
    int hostfk = std::atoi(args[7].c_str()), hostcos = std::atoi(args[8].c_str());
    int clientfk = std::atoi(args[9].c_str()), clientcos = std::atoi(args[10].c_str());

    /* netpeer owns the socket's receive side once the match starts, so
     * acknowledge redundantly now (the host retransmits START until GO). */
    std::string go = "SSBL GO " + std::to_string(seed);
    for (int i = 0; i < 5; i++) {
        send_peer(go);
    }
    set_state(PORT_NETLOBBY_IN_MATCH, "In match");
    syNetPeerStartOnlineMatch(0, delay, stage, stocks, items, seed, hostfk, hostcos, clientfk, clientcos);
}

void start_as_host(void)
{
    int host_costume = 0;
    int client_costume = (sLobby.peer_fkind == sLobby.my_fkind) ? 1 : 0;   // same fighter: tell them apart

    set_state(PORT_NETLOBBY_IN_MATCH, "In match");
    syNetPeerStartOnlineMatch(1, sLobby.delay, sLobby.stage, sLobby.stocks, sLobby.items, sLobby.seed,
                              sLobby.my_fkind, host_costume, sLobby.peer_fkind, client_costume);
}

std::string start_message(void)
{
    char text[160];
    int client_costume = (sLobby.peer_fkind == sLobby.my_fkind) ? 1 : 0;
    std::snprintf(text, sizeof(text), "SSBL START %d %d %d %u %d %d %d %d %d", sLobby.stage, sLobby.stocks,
                  sLobby.items, sLobby.seed, sLobby.delay, sLobby.my_fkind, 0, sLobby.peer_fkind, client_costume);
    return text;
}

std::vector<std::string> split(const char *text)
{
    std::vector<std::string> parts;
    std::string current;
    for (const char *c = text; *c != '\0'; c++) {
        if (*c == ' ' || *c == '\n' || *c == '\r') {
            if (!current.empty()) {
                parts.push_back(current);
                current.clear();
            }
        } else {
            current += *c;
        }
    }
    if (!current.empty()) {
        parts.push_back(current);
    }
    return parts;
}

void handle_server(const std::vector<std::string> &args)
{
    if (args.size() < 2) {
        return;
    }
    if (args[1] == "ROOM" && args.size() >= 3 && sLobby.state == PORT_NETLOBBY_CREATING) {
        sLobby.code = args[2];
        set_state(PORT_NETLOBBY_WAITING, "Room " + sLobby.code + " - waiting for a player to join");
    } else if (args[1] == "PEER" && args.size() >= 5 &&
               (sLobby.state == PORT_NETLOBBY_WAITING || sLobby.state == PORT_NETLOBBY_JOINING)) {
        sLobby.candidates = { args[3] };
        if (args[4] != args[3] && args[4] != "-") {
            sLobby.candidates.push_back(args[4]);
        }
        set_state(PORT_NETLOBBY_PUNCHING, "Connecting to opponent...");
    } else if (args[1] == "RELAYING" && sLobby.state == PORT_NETLOBBY_PUNCHING) {
        port_udp_set_relay(sLobby.server.c_str(), sLobby.code.c_str());
        if (!sLobby.candidates.empty()) {
            port_udp_set_peer(sLobby.candidates[0].c_str());   // relay ignores the address but needs a peer
        }
        sLobby.peer = "relay";
        sLobby.status = "Connecting through relay...";
    } else if (args[1] == "ERR") {
        fail("Server: " + (args.size() >= 3 ? args[2] : std::string("error")));
    }
}

void handle_peer(const std::vector<std::string> &args, const std::string &from)
{
    if (args.size() < 2) {
        return;
    }
    sLobby.last_heard_ms = port_udp_now_ms();

    if (sLobby.state == PORT_NETLOBBY_PUNCHING && (args[1] == "HELLO" || args[1] == "HELLOACK")) {
        if (from != "relay") {
            port_udp_set_relay(nullptr, nullptr);   // a direct path beats the relay
        }
        connect_to(from);
        send_peer("SSBL HELLOACK");
        return;
    }
    if (args[1] == "HELLO") {
        send_peer("SSBL HELLOACK");
    } else if (args[1] == "PICK" && args.size() >= 3) {
        sLobby.peer_fkind = std::atoi(args[2].c_str());
        if (args.size() >= 4) {
            send_peer("SSBL PONG " + args[3]);
        }
    } else if (args[1] == "PONG" && args.size() >= 3) {
        sLobby.ping_ms = static_cast<int>(port_udp_now_ms() - std::strtoul(args[2].c_str(), nullptr, 10));
    } else if (args[1] == "START" && !sLobby.is_host && sLobby.state == PORT_NETLOBBY_CONNECTED) {
        start_as_client(args);
    } else if (args[1] == "GO" && sLobby.is_host && sLobby.state == PORT_NETLOBBY_STARTING) {
        start_as_host();
    }
}

void receive(void)
{
    char buffer[512];
    char from[64];

    for (;;) {
        int size = port_udp_recv_from(buffer, sizeof(buffer) - 1, from, sizeof(from));
        if (size <= 0) {
            return;
        }
        buffer[size] = '\0';
        if (size < 5) {
            continue;
        }
        std::vector<std::string> args = split(buffer);
        if (args.empty()) {
            continue;
        }
        if (args[0] == "SSBR") {
            handle_server(args);
        } else if (args[0] == "SSBL") {
            handle_peer(args, from);
        }
        if (sLobby.state == PORT_NETLOBBY_IN_MATCH || sLobby.state == PORT_NETLOBBY_FAILED) {
            return;     // netpeer owns the socket now
        }
    }
}

void update_auto(void)
{
    if (!sAuto.checked) {
        sAuto.checked = true;
        if (const char *server = env("SSB64_LOBBY_HOST")) {
            sAuto.host = true;
            sAuto.server = server;
        } else if (const char *server = env("SSB64_LOBBY_JOIN")) {
            sAuto.join = true;
            sAuto.server = server;
        }
        sAuto.code_file = env("SSB64_LOBBY_CODE_FILE") ? env("SSB64_LOBBY_CODE_FILE") : "lobby_code.txt";
        sAuto.autostart = env("SSB64_LOBBY_AUTOSTART") != nullptr;
        int fkind = env("SSB64_LOBBY_FKIND") ? std::atoi(env("SSB64_LOBBY_FKIND")) : 0;
        int stage = env("SSB64_LOBBY_STAGE") ? std::atoi(env("SSB64_LOBBY_STAGE")) : 0;

        if (sAuto.host) {
            std::remove(sAuto.code_file.c_str());
            port_netlobby_host(sAuto.server.c_str(), fkind, stage, 3, 0, 2);
        }
        sLobby.my_fkind = fkind;
    }
    if (sAuto.host && !sAuto.code_written && !sLobby.code.empty()) {
        if (FILE *f = std::fopen(sAuto.code_file.c_str(), "w")) {
            std::fputs(sLobby.code.c_str(), f);
            std::fclose(f);
            sAuto.code_written = true;
        }
    }
    if (sAuto.join && sLobby.state == PORT_NETLOBBY_IDLE) {
        if (FILE *f = std::fopen(sAuto.code_file.c_str(), "r")) {
            char code[16] = {};
            if (std::fgets(code, sizeof(code), f) != nullptr && std::strlen(code) >= 6) {
                code[6] = '\0';
                port_netlobby_join(sAuto.server.c_str(), code, sLobby.my_fkind);
                sAuto.join = false;
            }
            std::fclose(f);
        }
    }
    if (sAuto.host && sAuto.autostart && sLobby.state == PORT_NETLOBBY_CONNECTED && sLobby.peer_fkind >= 0) {
        if (sAuto.start_after_ms == 0) {
            sAuto.start_after_ms = port_udp_now_ms() + 1000;
        } else if (elapsed(sAuto.start_after_ms, 0)) {
            port_netlobby_start_match();
            sAuto.autostart = false;
        }
    }
}

} // namespace

extern "C" void port_netlobby_update(void)
{
    Lobby &l = sLobby;

    update_auto();

    if (l.state == PORT_NETLOBBY_IDLE || l.state == PORT_NETLOBBY_FAILED || l.state == PORT_NETLOBBY_IN_MATCH) {
        return;
    }
    receive();

    switch (l.state) {
    case PORT_NETLOBBY_CREATING:
        if (elapsed(l.last_send_ms, kCreateRetryMs)) {
            send_server("SSBR CREATE " + l.nonce + " " + local_endpoint());
            l.last_send_ms = port_udp_now_ms();
        }
        if (elapsed(l.state_ms, kPeerTimeoutMs)) {
            fail("No answer from server " + l.server);
        }
        break;

    case PORT_NETLOBBY_JOINING:
        if (elapsed(l.last_send_ms, kCreateRetryMs)) {
            send_server("SSBR JOIN " + l.code + " " + l.nonce + " " + local_endpoint());
            l.last_send_ms = port_udp_now_ms();
        }
        if (elapsed(l.state_ms, kPeerTimeoutMs)) {
            fail("No answer from server " + l.server);
        }
        break;

    case PORT_NETLOBBY_WAITING:
        if (elapsed(l.last_ping_ms, kPingMs)) {
            send_server("SSBR PING " + l.code);
            l.last_ping_ms = port_udp_now_ms();
        }
        break;

    case PORT_NETLOBBY_PUNCHING:
        /* SSB64_LOBBY_FORCE_RELAY: skip the direct attempt (tests the relay). */
        if (!l.relay_requested && env("SSB64_LOBBY_FORCE_RELAY") != nullptr) {
            send_server("SSBR RELAY " + l.code);
            l.relay_requested = true;
        }
        if (elapsed(l.last_send_ms, kHelloMs)) {
            if (l.peer == "relay") {
                send_peer("SSBL HELLO");
            } else if (env("SSB64_LOBBY_FORCE_RELAY") == nullptr) {
                for (const std::string &candidate : l.candidates) {
                    port_udp_send_to(candidate.c_str(), "SSBL HELLO", 10);
                }
            }
            l.last_send_ms = port_udp_now_ms();
        }
        if (!l.relay_requested && elapsed(l.state_ms, kPunchTimeoutMs)) {
            send_server("SSBR RELAY " + l.code);
            l.relay_requested = true;
            l.status = "Direct connection failed, trying relay...";
        }
        if (elapsed(l.state_ms, kPunchTimeoutMs + kRelayTimeoutMs)) {
            fail("Couldn't reach the other player");
        }
        break;

    case PORT_NETLOBBY_CONNECTED:
    case PORT_NETLOBBY_STARTING:
        if (elapsed(l.last_send_ms, (l.state == PORT_NETLOBBY_STARTING) ? kStartRetryMs : kPickMs)) {
            send_peer("SSBL PICK " + std::to_string(l.my_fkind) + " " + std::to_string(port_udp_now_ms()));
            if (l.state == PORT_NETLOBBY_STARTING) {
                send_peer(start_message());
            }
            l.last_send_ms = port_udp_now_ms();
        }
        if (elapsed(l.last_ping_ms, kPingMs)) {
            send_server("SSBR PING " + l.code);     // keeps the NAT mapping and relay room alive
            l.last_ping_ms = port_udp_now_ms();
        }
        if (elapsed(l.last_heard_ms, kPeerTimeoutMs)) {
            fail("Lost connection to the other player");
        }
        break;

    default:
        break;
    }
}

extern "C" void port_netlobby_host(const char *server, int fkind, int stage, int stocks, int items, int delay)
{
    sLobby.is_host = true;
    sLobby.my_fkind = fkind;
    sLobby.stage = stage;
    sLobby.stocks = stocks;
    sLobby.items = items;
    sLobby.delay = delay;
    sLobby.code.clear();
    if (open_socket(server ? server : "")) {
        set_state(PORT_NETLOBBY_CREATING, "Creating room...");
    }
}

extern "C" void port_netlobby_join(const char *server, const char *code, int fkind)
{
    sLobby.is_host = false;
    sLobby.my_fkind = fkind;
    sLobby.code = code ? code : "";
    for (char &c : sLobby.code) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (sLobby.code.size() != 6) {
        fail("Room codes are 6 characters");
        return;
    }
    if (open_socket(server ? server : "")) {
        set_state(PORT_NETLOBBY_JOINING, "Joining room " + sLobby.code + "...");
    }
}

extern "C" void port_netlobby_cancel(void)
{
    port_udp_close();
    set_state(PORT_NETLOBBY_IDLE, "Not connected");
}

extern "C" void port_netlobby_start_match(void)
{
    if (!sLobby.is_host || sLobby.state != PORT_NETLOBBY_CONNECTED || sLobby.peer_fkind < 0) {
        return;
    }
    std::random_device rd;
    sLobby.seed = (rd() & 0x7FFFFFFF) | 1;
    set_state(PORT_NETLOBBY_STARTING, "Starting match...");
}

extern "C" void port_netlobby_set_fkind(int fkind)
{
    sLobby.my_fkind = fkind;
}

extern "C" void port_netlobby_on_match_end(void)
{
    if (sLobby.state == PORT_NETLOBBY_IN_MATCH) {
        sLobby.last_heard_ms = port_udp_now_ms();
        set_state(PORT_NETLOBBY_CONNECTED, std::string("Match over") +
                  (sLobby.is_host ? " - press Start match for a rematch" : " - waiting for host"));
    }
}

extern "C" int port_netlobby_state(void) { return sLobby.state; }
extern "C" const char *port_netlobby_status(void) { return sLobby.status.c_str(); }
extern "C" const char *port_netlobby_code(void) { return sLobby.code.c_str(); }
extern "C" int port_netlobby_peer_fkind(void) { return sLobby.peer_fkind; }
extern "C" int port_netlobby_is_host(void) { return sLobby.is_host ? 1 : 0; }
extern "C" int port_netlobby_is_relay(void) { return port_udp_is_relay(); }
extern "C" int port_netlobby_ping_ms(void) { return sLobby.ping_ms; }
