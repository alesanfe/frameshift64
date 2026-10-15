// SPDX-License-Identifier: MIT
//
// Portions of this file are derived from the Ship of Harkinian (SoH) project
//   Copyright (c) The Harbour Masters
//   https://github.com/HarbourMasters/Shipwright
// Licensed under the MIT License; see LICENSE at repository root.

#include "PortMenu.h"

#include "Toasts.h"
#include "UIWidgets.hpp"
#include "Compat.h"
#include "networking/netplay/session/netlobby.h"
#include "networking/netplay/transport/websocket_transport.h"

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include <imgui.h>

#include <spdlog/fmt/fmt.h>

#include <chrono>
#include <cctype>
#include <cstring>
#include <string>

namespace ssb64 {

using namespace UIWidgets;

#if !defined(BATTLESHIP_UWP)
static const char* const kNetplayFighters[] = { "Mario", "Fox", "Donkey Kong", "Samus", "Luigi", "Link",
                                                "Yoshi", "Captain Falcon", "Kirby", "Pikachu", "Jigglypuff", "Ness" };
static const char* const kNetplayStages[] = { "Peach's Castle", "Sector Z", "Kongo Jungle", "Planet Zebes",
                                              "Hyrule Castle", "Yoshi's Island", "Dream Land", "Saffron City",
                                              "Mushroom Kingdom" };

// Room codes: 6 chars from the unambiguous alphabet used by netlobby.cpp
// (no 0/O/1/I). Sanitize keeps only alphabet chars, uppercased — so pasted
// "abc-123" or "ABC 123" both collapse to "ABC123".
constexpr size_t kRoomCodeLen = 6;
static void SanitizeRoomCode(const char* in, char* out, size_t outSize) {
    size_t n = 0;
    for (const char* p = in; *p != '\0' && n + 1 < outSize && n < kRoomCodeLen; p++) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
        if ((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '9')) {
            out[n++] = c;
        }
    }
    out[n] = '\0';
}

// "ABC123" -> "ABC 123" for display.
static std::string GroupRoomCode(const char* code) {
    std::string grouped;
    for (size_t i = 0; code[i] != '\0'; i++) {
        if (i == kRoomCodeLen / 2) {
            grouped += ' ';
        }
        grouped += code[i];
    }
    return grouped;
}

/* Structured netplay status: the raw lobby state maps to a severity tag and
 * color plus an optional recovery hint, so a failure reads as an actionable
 * state instead of an undifferentiated paragraph of text. */
struct NetStatusView {
    const ImVec4* color;
    const char* tag;
    const char* hint;
};

static NetStatusView NetStatusViewFor(int state) {
    switch (state) {
    case PORT_NETLOBBY_CREATING:
    case PORT_NETLOBBY_JOINING:
    case PORT_NETLOBBY_PUNCHING:
        return { &SemanticColors::Info, "Connecting", nullptr };
    case PORT_NETLOBBY_WAITING:
        return { &SemanticColors::Info, "Waiting", nullptr };
    case PORT_NETLOBBY_CONNECTED:
        return { &SemanticColors::Success, "Connected", nullptr };
    case PORT_NETLOBBY_STARTING:
        return { &SemanticColors::Info, "Starting", nullptr };
    case PORT_NETLOBBY_IN_MATCH:
        return { &SemanticColors::Success, "Live", nullptr };
    case PORT_NETLOBBY_FAILED:
        return { &SemanticColors::Error, "Failed",
                 "Check the room code and server address, then try again. "
                 "If it keeps failing, the lobby service may be unreachable." };
    default:
        return { nullptr, nullptr, nullptr };
    }
}

/* §25 — one-shot reachability probe for the configured lobby server. Opens
 * a WebSocket against the server's root path: a 101 upgrade means full
 * TCP+TLS+WS reachability; any other HTTP answer still proves the server is
 * up (the upgrade only lives under /ws) so it counts as reachable; anything
 * else is a real connectivity failure. Zero side effects — no room is
 * created or joined. Runs only while the lobby is idle: the control socket
 * is shared, so an in-flight test is cancelled when a session starts. */
enum class ConnTestState { Idle, Running, Done };
struct ConnTest {
    ConnTestState state = ConnTestState::Idle;
    std::chrono::steady_clock::time_point started{};
    std::string result;
    bool ok = false;
};

static void StartConnTest(ConnTest& test, const char* server) {
    std::string url = (server != nullptr && server[0] != '\0') ? server : PORT_NETLOBBY_DEFAULT_SERVER;
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    url += "/";
    port_ws_connect(url.c_str());
    test.state = ConnTestState::Running;
    test.started = std::chrono::steady_clock::now();
    test.result.clear();
    test.ok = false;
}

static void PollConnTest(ConnTest& test) {
    if (test.state != ConnTestState::Running) {
        return;
    }
    const auto elapsedMs = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - test.started)
            .count());
    const int ws = port_ws_state();
    if (ws == PORT_WS_OPEN || ws == PORT_WS_FAILED || ws == PORT_WS_CLOSED ||
        elapsedMs > 10000 /* hard timeout */) {
        const char* err = port_ws_error();
        const bool httpReply = (err != nullptr) && (std::strncmp(err, "server answered HTTP", 20) == 0);
        if (ws == PORT_WS_OPEN || httpReply) {
            test.result = fmt::format("Server reachable — handshake answered in {} ms", elapsedMs);
            test.ok = true;
        } else if (elapsedMs > 10000) {
            test.result = "Timed out — the server did not answer within 10 seconds";
            test.ok = false;
        } else {
            test.result = fmt::format("Unreachable — {}", (err != nullptr && err[0] != '\0') ? err : "no error detail");
            test.ok = false;
        }
        port_ws_close();
        test.state = ConnTestState::Done;
    }
}

static void RenderNetplayLobby(WidgetInfo& /*widget*/) {
    static char serverBuf[128] = {};
    static char codeBuf[8] = {};
    static bool loaded = false;
    static ConnTest connTest;

    if (!loaded) {
        std::snprintf(serverBuf, sizeof(serverBuf), "%s", CVarGetString("gNetplay.Server", PORT_NETLOBBY_DEFAULT_SERVER));
        loaded = true;
    }
    int fighter = CVarGetInteger("gNetplay.Fighter", 0);
    int stage = CVarGetInteger("gNetplay.Stage", 0);
    int stocks = CVarGetInteger("gNetplay.Stocks", 3);
    int items = CVarGetInteger("gNetplay.Items", 0);
    int delay = CVarGetInteger("gNetplay.Delay", 2);
    auto save = []() { Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame(); };

    const int state = port_netlobby_state();
    const bool idle = (state == PORT_NETLOBBY_IDLE || state == PORT_NETLOBBY_FAILED);

    ImGui::TextWrapped("Play online with rollback netcode - up to 4 players in one room. One player hosts "
                       "and shares the room code; the others join with it. No port forwarding needed.");
    ImGui::Spacing();

    ImGui::BeginDisabled(state == PORT_NETLOBBY_STARTING || state == PORT_NETLOBBY_IN_MATCH);
    ImGui::TextUnformatted("Your fighter");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##NetplayFighter", &fighter, kNetplayFighters, IM_ARRAYSIZE(kNetplayFighters))) {
        CVarSetInteger("gNetplay.Fighter", fighter);
        port_netlobby_set_fkind(fighter);
        save();
    }
    ImGui::EndDisabled();

    /* Connect view — only while idle. Once you're in a room the panel swaps
     * to the room/status view below so the creation controls can't be
     * mistaken for live settings. */
    if (idle) {
        ImGui::SeparatorText("Host a match");
        ImGui::TextUnformatted("Stage");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##NetplayStage", &stage, kNetplayStages, IM_ARRAYSIZE(kNetplayStages))) {
            CVarSetInteger("gNetplay.Stage", stage);
            save();
        }
        ImGui::TextUnformatted("Stocks");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##NetplayStocks", &stocks, 1, 5)) {
            CVarSetInteger("gNetplay.Stocks", stocks);
            save();
        }
        bool itemsOn = items != 0;
        if (ImGui::Checkbox("Items", &itemsOn)) {
            CVarSetInteger("gNetplay.Items", itemsOn ? 1 : 0);
            save();
        }
        ImGui::TextUnformatted("Input delay (frames)");
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##NetplayDelay", &delay, 0, 4)) {
            CVarSetInteger("gNetplay.Delay", delay);
            save();
        }
        ImGui::TextDisabled("Recommended: 2 for most connections. Raise it on high-ping "
                            "links to reduce visible rollbacks; everyone in the room "
                            "plays with the host's value.");
        {
            const int ping = port_netlobby_ping_ms();
            if (ping >= 0) {
                // ~16.6ms per frame: cover the round trip plus jitter margin.
                const int recommended = ping < 30 ? 1 : ping < 70 ? 2 : ping < 110 ? 3 : 4;
                if (recommended != delay) {
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Use recommended")) {
                        delay = recommended;
                        CVarSetInteger("gNetplay.Delay", delay);
                        save();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("(%d frames at %d ms)", recommended, ping);
                }
            }
        }
        if (UIWidgets::Button("Host", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
            port_netlobby_host(serverBuf, fighter, stage, stocks, itemsOn ? 1 : 0, delay);
        }

        ImGui::SeparatorText("Join a match");
        ImGui::TextUnformatted("Room code");
        ImGui::SetNextItemWidth(-80.0f);
        const bool codeEnter = ImGui::InputText("##NetplayCode", codeBuf, sizeof(codeBuf),
                                                ImGuiInputTextFlags_CharsUppercase |
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        // Always run the sanitizer — paste via Ctrl+V lands in one edit, but
        // a programmatic write shouldn't leave invalid chars behind either.
        SanitizeRoomCode(codeBuf, codeBuf, sizeof(codeBuf));
        ImGui::SameLine();
        if (ImGui::SmallButton("Paste")) {
            SanitizeRoomCode(ImGui::GetClipboardText(), codeBuf, sizeof(codeBuf));
        }
        const size_t codeLen = std::strlen(codeBuf);
        if (codeLen > 0 && codeLen < kRoomCodeLen) {
            ImGui::TextDisabled("%d of %d characters", static_cast<int>(codeLen),
                                static_cast<int>(kRoomCodeLen));
        }
        const bool codeValid = (codeLen == kRoomCodeLen);
        if (codeEnter && codeValid) {
            port_netlobby_join(serverBuf, codeBuf, fighter);
        }
        ImGui::BeginDisabled(!codeValid);
        if (UIWidgets::Button("Join", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
            port_netlobby_join(serverBuf, codeBuf, fighter);
        }
        ImGui::SameLine();
        if (UIWidgets::Button("Spectate")) {
            port_netlobby_spectate(serverBuf, codeBuf);
        }
        ImGui::EndDisabled();
        if (!codeValid && codeLen == 0) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("Enter the 6-character room code shared by the host.");
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Watch the room's match live without playing - the spectator never\n"
                              "joins the match, so it can't lag or desync the players.");
        }

        ImGui::SeparatorText("LAN (direct IP)");
        {
            static char lanAddr[64] = {};
            static int lanPort = 41700;
            ImGui::TextUnformatted("Host port");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt("##LanPort", &lanPort, 0, 0);
            if (UIWidgets::Button("Host LAN", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
                port_netlobby_lan_host(lanPort, fighter, stage, stocks, itemsOn ? 1 : 0, delay);
            }
            ImGui::TextUnformatted("Host address (ip[:port])");
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##LanAddr", lanAddr, sizeof(lanAddr));
            if (UIWidgets::Button("Join LAN", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
                port_netlobby_lan_join(lanAddr, fighter);
            }
        }

        if (ImGui::CollapsingHeader("Advanced")) {
            ImGui::TextUnformatted("Lobby server");
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##NetplayServer", serverBuf, sizeof(serverBuf))) {
                CVarSetString("gNetplay.Server", serverBuf);
                save();
            }
            if (ImGui::SmallButton("Use default server")) {
                std::snprintf(serverBuf, sizeof(serverBuf), "%s", PORT_NETLOBBY_DEFAULT_SERVER);
                CVarClear("gNetplay.Server");
                save();
            }
        }
    }

    if (state == PORT_NETLOBBY_IN_MATCH) {
        ImGui::SeparatorText("Match in progress");
    } else {
        ImGui::SeparatorText("Status");
    }
    {
        const NetStatusView statusView = NetStatusViewFor(state);
        if (statusView.color != nullptr) {
            ImGui::TextColored(*statusView.color, "[%s]", statusView.tag);
            ImGui::SameLine(0.0f, 6.0f);
        }
        ImGui::TextWrapped("%s", port_netlobby_status());
        if (statusView.hint != nullptr) {
            ImGui::TextDisabled("%s", statusView.hint);
        }
    }

    if (port_netlobby_code()[0] != '\0' && port_netlobby_is_host() && !idle) {
        ImGui::Text("Room code: %s", GroupRoomCode(port_netlobby_code()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            ImGui::SetClipboardText(port_netlobby_code());
            ToastSuccess("Room code copied to clipboard");
        }
    }
    if (state == PORT_NETLOBBY_CONNECTED || state == PORT_NETLOBBY_STARTING || state == PORT_NETLOBBY_IN_MATCH) {
        const int mySlot = port_netlobby_my_slot();
        const int peers = port_netlobby_peer_count();
        const int fighterCount = static_cast<int>(IM_ARRAYSIZE(kNetplayFighters));
        ImGui::Text("Players in room: %d", port_netlobby_player_count());
        ImGui::BulletText("Player %d: %s (you)", mySlot + 1,
                          (fighter >= 0 && fighter < fighterCount) ? kNetplayFighters[fighter] : "?");
        for (int i = 0; i < peers; i++) {
            const int slot = port_netlobby_peer_slot(i);
            if (slot < 0 || slot == mySlot) {
                continue;
            }
            const char* label;
            int fk = port_netlobby_peer_fkind_at(i);
            if (!port_netlobby_peer_connected_at(i)) {
                label = "connecting...";
            } else if (fk >= 0 && fk < fighterCount) {
                label = kNetplayFighters[fk];
            } else {
                label = "choosing...";
            }
            ImGui::BulletText("Player %d: %s", slot + 1, label);
        }
        int ping = port_netlobby_ping_ms();
        if (ping >= 0) {
            const char* quality = ping < 50 ? "Good" : ping < 100 ? "Fair" : "Poor";
            const ImVec4 qualityColor = ping < 50   ? SemanticColors::Success
                                        : ping < 100 ? SemanticColors::Warning
                                                     : SemanticColors::Error;
            ImGui::Text("Connection: ");
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(qualityColor, "%s", quality);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled(" — %d ms, %s", ping,
                                port_netlobby_is_relay() ? "relayed" : "direct");
        }
        if (port_netlobby_is_host()) {
            ImGui::TextDisabled("Input delay: %d frames (applies to all players)",
                                CVarGetInteger("gNetplay.Delay", 2));
        }
    }
    if (port_netlobby_is_host() && state == PORT_NETLOBBY_CONNECTED) {
        bool canStart = false;
        const int peers = port_netlobby_peer_count();
        for (int i = 0; i < peers; i++) {
            if (port_netlobby_peer_connected_at(i)) {
                canStart = port_netlobby_peer_fkind_at(i) >= 0;
                if (!canStart) {
                    break;
                }
            }
        }
        ImGui::BeginDisabled(!canStart);
        if (UIWidgets::Button("Start match", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Green))) {
            port_netlobby_start_match();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canStart) {
            ImGui::SetTooltip("Waiting for every connected player to pick a fighter.");
        }
    }
    if (!idle && state != PORT_NETLOBBY_IN_MATCH) {
        if (UIWidgets::Button("Leave", UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Red))) {
            port_netlobby_cancel();
        }
    }
}

#endif // !BATTLESHIP_UWP (netplay lobby)

void PortMenu::AddMenuOnline() {
#if !defined(BATTLESHIP_UWP)
    AddMenuEntry("Online", CVAR_SETTING("Menu.OnlineSidebarSection"));
    WidgetPath path = { "Online", "Netplay", SECTION_COLUMN_1 };
    AddSidebarEntry("Online", "Netplay", 1);
    AddWidget(path, "Netplay Lobby", WIDGET_CUSTOM).RaceDisable(false).CustomFunction(RenderNetplayLobby);
#endif
}

} // namespace ssb64
