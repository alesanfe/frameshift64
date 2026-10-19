// SPDX-License-Identifier: MIT
//
// Portions of this file are derived from the Ship of Harkinian (SoH) project
//   Copyright (c) The Harbour Masters
//   https://github.com/HarbourMasters/Shipwright
// Licensed under the MIT License; see LICENSE at repository root.

#include "PortMenu.h"
#include "PortI18n.h"

#include "Toasts.h"
#include "UIWidgets.hpp"
#include "Compat.h"
#include "netfriends.h"
#include "netleaderboard.h"
#include "netlobby.h"
#include "netstats.h"
#include "udp_transport.h"
#include "websocket_transport.h"

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
        return { &SemanticColors::Info, TR("Connecting"), nullptr };
    case PORT_NETLOBBY_WAITING:
        return { &SemanticColors::Info, TR("Waiting"), nullptr };
    case PORT_NETLOBBY_CONNECTED:
        return { &SemanticColors::Success, TR("Connected"), nullptr };
    case PORT_NETLOBBY_STARTING:
        return { &SemanticColors::Info, TR("Starting"), nullptr };
    case PORT_NETLOBBY_IN_MATCH:
        return { &SemanticColors::Success, TR("Live"), nullptr };
    case PORT_NETLOBBY_FAILED:
        return { &SemanticColors::Error, TR("Failed"),
                 TR("Check the room code and server address, then try again. "
                    "If it keeps failing, the lobby service may be unreachable.") };
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
    /* Phase 2 (STUN probe): kicked off as soon as the lobby handshake
     * finishes so UDP egress + NAT behaviour are tested too. */
    bool stun_started = false;
    std::string stun_result;
};

static void StartStunProbe(ConnTest& test) {
    const char* servers[PORT_UDP_STUN_MAX] = {};
    const int n = port_netlobby_stun_server_count();
    for (int i = 0; i < n && i < PORT_UDP_STUN_MAX; i++) {
        servers[i] = port_netlobby_stun_server_at(i);
    }
    test.stun_started = true;
    if (n <= 0 || !port_udp_stun_probe_start(servers, n)) {
        test.stun_result = TR("UDP probe unavailable on this platform");
        return;
    }
}

static void PollStunProbe(ConnTest& test) {
    if (!test.stun_started || port_udp_stun_probe_poll() != 0) {
        return;
    }
    std::string endpoints[PORT_UDP_STUN_MAX];
    const int n = port_netlobby_stun_server_count();
    int answered = 0;
    for (int i = 0; i < n && i < PORT_UDP_STUN_MAX && answered < PORT_UDP_STUN_MAX; i++) {
        char buf[48];
        if (port_udp_stun_probe_result(i, buf, sizeof(buf))) {
            endpoints[answered++] = buf;
        }
    }
    port_udp_stun_probe_close();
    if (answered == 0) {
        test.stun_result = TR("UDP blocked — STUN got no answer, matches will relay through the server");
    } else if (answered == 1) {
        test.stun_result = fmt::format(fmt::runtime(TR("Public UDP endpoint %s — direct peer-to-peer should work")),
                                       endpoints[0].c_str());
    } else if (endpoints[0] == endpoints[1]) {
        test.stun_result = fmt::format(fmt::runtime(TR("Public UDP endpoint %s — NAT keeps the mapping, direct peer-to-peer should work")),
                                       endpoints[0].c_str());
    } else {
        test.stun_result = fmt::format(fmt::runtime(TR("Symmetric NAT (%s vs %s) — hole punching may fail, expect relay fallback")),
                                       endpoints[0].c_str(), endpoints[1].c_str());
    }
}

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
    test.stun_started = false;
    test.stun_result.clear();
    port_udp_stun_probe_close();   /* in case a previous probe was still pending */
}

static bool ConnTestFinished(int ws, int elapsedMs) {
    return ws == PORT_WS_OPEN || ws == PORT_WS_FAILED || ws == PORT_WS_CLOSED ||
           elapsedMs > 10000 /* hard timeout */;
}

static void PollConnTest(ConnTest& test) {
    if (test.state == ConnTestState::Running) {
        const auto elapsedMs = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - test.started)
                .count());
        const int ws = port_ws_state();
        if (ConnTestFinished(ws, elapsedMs)) {
            const char* err = port_ws_error();
            const bool httpReply = (err != nullptr) && (std::strncmp(err, "server answered HTTP", 20) == 0);
            if (ws == PORT_WS_OPEN || httpReply) {
                test.result = fmt::format(fmt::runtime(TR("Server reachable — handshake answered in {} ms")), elapsedMs);
                test.ok = true;
            } else if (elapsedMs > 10000) {
                test.result = TR("Timed out — the server did not answer within 10 seconds");
                test.ok = false;
            } else {
                test.result = fmt::format(fmt::runtime(TR("Unreachable — {}")), (err != nullptr && err[0] != '\0') ? err : "no error detail");
                test.ok = false;
            }
            port_ws_close();
            test.state = ConnTestState::Done;
            StartStunProbe(test);   /* phase 2: UDP egress + NAT behaviour */
        }
    }
    /* The STUN probe keeps running after the WS phase finishes. */
    if (test.stun_started && test.stun_result.empty()) {
        PollStunProbe(test);
    }
}

static void RenderNetplayLobby(WidgetInfo& /*widget*/) {
    static char serverBuf[128] = {};
    static char codeBuf[8] = {};
    static bool loaded = false;
    static ConnTest connTest;
    static float sPingHistory[120] = {};
    static int sPingHead = 0;
    static int sPingCount = 0;
    static std::chrono::steady_clock::time_point sPingLast;

    if (!loaded) {
        std::snprintf(serverBuf, sizeof(serverBuf), "%s", CVarGetString("gNetplay.Server", PORT_NETLOBBY_DEFAULT_SERVER));
        loaded = true;
    }
    int fighter = CVarGetInteger("gNetplay.Fighter", 0);
    int stage = CVarGetInteger("gNetplay.Stage", 0);
    int stocks = CVarGetInteger("gNetplay.Stocks", 3);
    int items = CVarGetInteger("gNetplay.Items", 0);
    int delay = CVarGetInteger("gNetplay.Delay", 2);
    // §22 — "Automatic" is a persistent mode, not a one-shot copy: the
    // delay resolves from measured ping each time a session is hosted
    // (fallback 2 before any ping is known).
    const bool autoDelay = CVarGetInteger("gNetplay.AutoDelay", 1) != 0;
    const int lobbyPing = port_netlobby_ping_ms();
    const int pingRecommended = lobbyPing < 30 ? 1 : lobbyPing < 70 ? 2 : lobbyPing < 110 ? 3 : 4;
    const int effectiveDelay = autoDelay ? (lobbyPing >= 0 ? pingRecommended : 2) : delay;
    auto save = []() { Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame(); };

    const int state = port_netlobby_state();
    const bool idle = (state == PORT_NETLOBBY_IDLE || state == PORT_NETLOBBY_FAILED);
    if (!idle && connTest.state == ConnTestState::Running) {
        // Lobby session claimed the shared control socket — abandon the probe.
        connTest.state = ConnTestState::Idle;
        connTest.result.clear();
    }
    if (idle) {
        sPingCount = 0;
        sPingHead = 0;
    }
    PollConnTest(connTest);

    ImGui::TextWrapped("%s", TR("Play online with rollback netcode - up to 4 players in one room. One player hosts "
                       "and shares the room code; the others join with it. No port forwarding needed."));
    ImGui::Spacing();

    ImGui::BeginDisabled(state == PORT_NETLOBBY_STARTING || state == PORT_NETLOBBY_IN_MATCH);
    ImGui::TextUnformatted(TR("Your fighter"));
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
        if (ImGui::CollapsingHeader(TR("How to play online"))) {
            ImGui::PushTextWrapPos();
            ImGui::TextWrapped("%s", TR(
                "• Quick play: press Find match — you join a public queue room and\n"
                "  get paired with whoever else is searching. No code needed.\n"
                "• Play with a friend: one of you presses Host and shares the\n"
                "  6-letter room code; the other enters it under Join a match.\n"
                "• Spectate: enter a room code and press Spectate — you watch\n"
                "  live without affecting the match or its netcode.\n"
                "• Ready up when you're set; the host starts the match.\n"
                "• LAN (direct IP) skips the lobby server entirely — use it on\n"
                "  the same network or over a VPN."));
            ImGui::PopTextWrapPos();
        }

        ImGui::SeparatorText(TR("Host a match"));
        ImGui::TextUnformatted(TR("Stage"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::Combo("##NetplayStage", &stage, kNetplayStages, IM_ARRAYSIZE(kNetplayStages))) {
            CVarSetInteger("gNetplay.Stage", stage);
            save();
        }
        ImGui::TextUnformatted(TR("Stocks"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::SliderInt("##NetplayStocks", &stocks, 1, 5)) {
            CVarSetInteger("gNetplay.Stocks", stocks);
            save();
        }
        bool itemsOn = items != 0;
        if (ImGui::Checkbox(TR("Items"), &itemsOn)) {
            CVarSetInteger("gNetplay.Items", itemsOn ? 1 : 0);
            save();
        }
        ImGui::TextUnformatted(TR("Input delay"));
        ImGui::SetNextItemWidth(-FLT_MIN);
        {
            // ~16.6ms per frame: cover the round trip plus jitter margin.
            const std::string delayPreview =
                autoDelay ? TR("Automatic (recommended)")
                          : fmt::format(fmt::runtime(TR("{} frames")), delay);
            if (ImGui::BeginCombo("##NetplayDelay", delayPreview.c_str())) {
                if (ImGui::Selectable(TR("Automatic (recommended)"), autoDelay)) {
                    CVarSetInteger("gNetplay.AutoDelay", 1);
                    save();
                }
                for (int v = 0; v <= 4; v++) {
                    const std::string lbl = fmt::format(fmt::runtime(TR("{} frames")), v);
                    if (ImGui::Selectable(lbl.c_str(), !autoDelay && delay == v)) {
                        CVarSetInteger("gNetplay.AutoDelay", 0);
                        CVarSetInteger("gNetplay.Delay", v);
                        save();
                    }
                }
                ImGui::EndCombo();
            }
        }
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        if (autoDelay) {
            ImGui::TextWrapped(TR("Automatic: resolves from measured ping at room start (now: %d "
                                  "frames). Everyone in the room plays with the host's value."),
                               effectiveDelay);
        } else {
            ImGui::TextWrapped("%s", TR("Recommended: 2 for most connections. Raise it on high-ping "
                               "links to reduce visible rollbacks; everyone in the room "
                               "plays with the host's value."));
        }
        ImGui::PopStyleColor();
        if (UIWidgets::Button(TR("Host"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
            port_netlobby_host(serverBuf, fighter, stage, stocks, itemsOn ? 1 : 0, effectiveDelay);
        }

        ImGui::SeparatorText(TR("Quick play"));
        /* Local rating mirror: rating, record and history live in
         * netstats.json next to the save file. */
        {
            const int rating = port_netstats_rating();
            const int n = port_netstats_wins() + port_netstats_losses() + port_netstats_draws();
            char seasonBuf[16] = {};
            port_netstats_season(seasonBuf, sizeof(seasonBuf));
            if (seasonBuf[0] != '\0') {
                ImGui::TextDisabled(TR("Season %s"), seasonBuf);
            }
            if (n == 0) {
                ImGui::TextDisabled("%s", TR("No online matches on record yet."));
            } else {
                char tierBuf[16] = {};
                port_netstats_rank_tier_name(tierBuf, sizeof(tierBuf));
                std::string line = port_netstats_provisional()
                    ? fmt::format(fmt::runtime(TR("Rating: %d (provisional)  ·  %d-%d-%d")),
                                  rating, port_netstats_wins(), port_netstats_losses(),
                                  port_netstats_draws())
                    : fmt::format(fmt::runtime(TR("Rating: %d [%s]  ·  %d-%d-%d")),
                                  rating, tierBuf, port_netstats_wins(), port_netstats_losses(),
                                  port_netstats_draws());
                ImGui::TextWrapped("%s", line.c_str());
                if (ImGui::TreeNode(TR("Recent matches"))) {
                    const int total = port_netstats_history_count();
                    const int shown = (std::min)(total, 5);
                    for (int i = 0; i < shown; i++) {
                        int won = 0, opp = -1;
                        unsigned int ticks = 0;
                        unsigned long long ts = 0;
                        if (!port_netstats_history_at(i, &won, &opp, &ticks, &ts)) {
                            continue;
                        }
                        const char* verdict = won == 1 ? TR("Win")
                                            : won == 0 ? TR("Loss") : TR("Draw");
                        const ImVec4 vc = won == 1 ? SemanticColors::Success
                                        : won == 0 ? SemanticColors::Error
                                                   : SemanticColors::Muted;
                        ImGui::Bullet();
                        ImGui::TextColored(vc, "%s", verdict);
                        ImGui::SameLine();
                        if (opp > 0) {
                            ImGui::TextDisabled(TR("vs ~%d rating  ·  %ds"), opp, ticks / 60);
                        } else {
                            ImGui::TextDisabled(TR("%ds"), ticks / 60);
                        }
                    }
                    ImGui::TreePop();
                }
            }
        }
        if (UIWidgets::Button(TR("Find match"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
            port_netlobby_quickplay(serverBuf, fighter, stage, stocks, itemsOn ? 1 : 0, effectiveDelay);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("Joins the public queue — pairs with whoever else is waiting.\n"
                                      "Your rating is shared with the opponent and updates after the match."));
        }

        ImGui::SeparatorText(TR("Join a match"));
        ImGui::TextUnformatted(TR("Room code"));
        ImGui::SetNextItemWidth(-80.0f);
        const bool codeEnter = ImGui::InputText("##NetplayCode", codeBuf, sizeof(codeBuf),
                                                ImGuiInputTextFlags_CharsUppercase |
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
        // Always run the sanitizer — paste via Ctrl+V lands in one edit, but
        // a programmatic write shouldn't leave invalid chars behind either.
        SanitizeRoomCode(codeBuf, codeBuf, sizeof(codeBuf));
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Paste"))) {
            SanitizeRoomCode(ImGui::GetClipboardText(), codeBuf, sizeof(codeBuf));
        }
        const size_t codeLen = std::strlen(codeBuf);
        if (codeLen > 0 && codeLen < kRoomCodeLen) {
            ImGui::TextDisabled(TR("%d of %d characters"), static_cast<int>(codeLen), static_cast<int>(kRoomCodeLen));
        }
        const bool codeValid = (codeLen == kRoomCodeLen);
        if (codeEnter && codeValid) {
            port_netlobby_join(serverBuf, codeBuf, fighter);
        }
        ImGui::BeginDisabled(!codeValid);
        if (UIWidgets::Button(TR("Join"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
            port_netlobby_join(serverBuf, codeBuf, fighter);
        }
        ImGui::SameLine();
        if (UIWidgets::Button(TR("Spectate"))) {
            port_netlobby_spectate(serverBuf, codeBuf);
        }
        ImGui::EndDisabled();
        if (!codeValid && codeLen == 0) {
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                ImGui::SetTooltip("%s", TR("Enter the 6-character room code shared by the host."));
            }
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("Watch the room's match live without playing - the spectator never\n"
                              "joins the match, so it can't lag or desync the players."));
        }

        ImGui::SeparatorText(TR("LAN (direct IP)"));
        {
            static char lanAddr[64] = {};
            static int lanPort = 41700;
            ImGui::TextUnformatted(TR("Host port"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputInt("##LanPort", &lanPort, 0, 0);
            lanPort = std::clamp(lanPort, 1, 65535);
            if (UIWidgets::Button(TR("Host LAN"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
                port_netlobby_lan_host(lanPort, fighter, stage, stocks, itemsOn ? 1 : 0, effectiveDelay);
            }
            ImGui::TextUnformatted(TR("Host address (ip[:port])"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputText("##LanAddr", lanAddr, sizeof(lanAddr));
            if (UIWidgets::Button(TR("Join LAN"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
                port_netlobby_lan_join(lanAddr, fighter);
            }
        }

        if (ImGui::CollapsingHeader(TR("Friends"))) {
            static char friendBuf[32] = {};
            ImGui::SetNextItemWidth(-80.0f);
            const bool nameEnter = ImGui::InputText("##FriendName", friendBuf, sizeof(friendBuf),
                                                    ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            const bool canAdd = friendBuf[0] != '\0';
            ImGui::BeginDisabled(!canAdd);
            if (ImGui::SmallButton(TR("Add")) || (nameEnter && canAdd)) {
                if (port_netfriends_add(friendBuf)) {
                    ToastSuccess(TR("Friend added"));
                    friendBuf[0] = '\0';
                } else {
                    ToastError(TR("Could not add that name"));
                }
            }
            ImGui::EndDisabled();
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", TR("Names are the ones players type in their profile — a friend "
                                       "list entry flags them when they join your room. Blocking a "
                                       "name warns you if someone using it connects."));
            ImGui::PopStyleColor();
            const int friendCount = port_netfriends_count();
            if (friendCount == 0) {
                ImGui::TextDisabled("%s", TR("No friends added yet."));
            }
            for (int i = 0; i < friendCount; i++) {
                PortNetFriend f;
                if (!port_netfriends_get(i, &f)) {
                    continue;
                }
                ImGui::PushID(i);
                ImGui::BulletText("%s", f.name.data());
                ImGui::SameLine();
                if (f.blocked) {
                    ImGui::TextColored(SemanticColors::Error, "%s", TR("blocked"));
                    ImGui::SameLine();
                    if (ImGui::SmallButton(TR("Unblock"))) {
                        port_netfriends_set_blocked(f.name.data(), false);
                    }
                } else {
                    if (ImGui::SmallButton(TR("Block"))) {
                        port_netfriends_set_blocked(f.name.data(), true);
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton(TR("Remove"))) {
                    port_netfriends_remove(f.name.data());
                }
                ImGui::PopID();
            }
        }

        if (ImGui::CollapsingHeader(TR("Leaderboard"))) {
            /* Self-reported board on the lobby server — refreshed on
             * demand, rows cached between draws. */
            bool submitStats = CVarGetInteger("gNetplay.SubmitStats", 0) != 0;
            if (ImGui::Checkbox(TR("Publish my rating after matches"), &submitStats)) {
                CVarSetInteger("gNetplay.SubmitStats", submitStats ? 1 : 0);
                save();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", TR("Shares your display name and rating with the lobby server. "
                                       "Off by default."));
            ImGui::PopStyleColor();
            if (ImGui::SmallButton(TR("Refresh"))) {
                port_netleaderboard_fetch(serverBuf);
                port_netleaderboard_fetch_history(serverBuf);
            }
            const int lbState = port_netleaderboard_state();
            if (lbState == 1) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", TR("Loading..."));
            }
            if (lbState == -1) {
                ImGui::TextColored(SemanticColors::Error, "%s",
                                 TR("Could not reach the leaderboard."));
            } else if (lbState == 2) {
                const int myRank = port_netleaderboard_my_rank();
                if (myRank > 0) {
                    ImGui::TextColored(SemanticColors::Success,
                                       TR("Your rank: #%d"), myRank);
                }
                const int total = port_netleaderboard_count();
                if (total == 0) {
                    ImGui::TextDisabled("%s", TR("No entries yet."));
                }
                static char sReportName[128] = {};
                static bool sReportPopup = false;
                for (int i = 0; i < total; i++) {
                    /* Server allows 24 Unicode chars — up to 96 bytes UTF-8. */
                    char name[128] = {};
                    int r = 0, w = 0, l = 0, suspicious = 0, banned = 0;
                    if (!port_netleaderboard_at(i, name, sizeof(name), &r, &w, &l,
                                                &suspicious, &banned)) {
                        continue;
                    }
                    const bool mine = (myRank == i + 1);
                    ImGui::Bullet();
                    if (mine) {
                        ImGui::TextColored(SemanticColors::Success, "#%d %s", i + 1, name);
                    } else {
                        ImGui::Text("#%d %s", i + 1, name);
                    }
                    if (banned) {
                        ImGui::SameLine();
                        ImGui::TextColored(SemanticColors::Error, "[banned]");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", TR("Flagged by community reports."));
                        }
                    } else if (suspicious) {
                        ImGui::SameLine();
                        ImGui::TextColored(SemanticColors::Warning, "[?]");
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip("%s", TR("New account with an unusual record — "
                                                       "treat this rating as provisional."));
                        }
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%d  ·  %d-%d", r, w, l);
                    if (!mine && !banned) {
                        ImGui::SameLine();
                        ImGui::PushID(i);
                        if (ImGui::SmallButton(TR("Report"))) {
                            std::snprintf(sReportName, sizeof(sReportName), "%s", name);
                            sReportPopup = true;
                        }
                        ImGui::PopID();
                    }
                }
                if (sReportPopup) {
                    ImGui::OpenPopup(TR("Report player?"));
                    sReportPopup = false;
                }
                if (ImGui::BeginPopupModal(TR("Report player?"), nullptr,
                                           ImGuiWindowFlags_AlwaysAutoResize)) {
                    ImGui::TextWrapped("%s",
                        TR("Report %s for cheating or abuse? The flag sticks when "
                           "several distinct players report within 24 hours."),
                        sReportName);
                    if (ImGui::Button(TR("Report"))) {
                        port_netleaderboard_report(serverBuf, sReportName);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(TR("Cancel"))) {
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::EndPopup();
                }
            } else {
                ImGui::TextDisabled("%s", TR("Press Refresh to load the board."));
            }
            /* Verified match history rides the same Refresh — the service
             * keeps the last LB_HISTORY_MAX corroborated results. */
            if (ImGui::TreeNode(TR("Recent matches"))) {
                const int hState = port_netleaderboard_history_state();
                if (hState == 1) {
                    ImGui::TextDisabled("%s", TR("Loading..."));
                } else if (hState == -1) {
                    ImGui::TextColored(SemanticColors::Error, "%s",
                                     TR("Could not load match history."));
                } else if (hState == 2) {
                    const int hTotal = port_netleaderboard_history_count();
                    if (hTotal == 0) {
                        ImGui::TextDisabled("%s", TR("No verified matches yet."));
                    }
                    for (int i = 0; i < hTotal && i < 20; i++) {
                        PortLbMatchRow m;
                        if (!port_netleaderboard_history_at(i, &m)) {
                            continue;
                        }
                        if (m.draw) {
                            ImGui::BulletText(TR("%s drew %s"), m.a, m.b);
                        } else {
                            ImGui::BulletText(TR("%s beat %s"), m.w, m.l);
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s", m.stamp);
                    }
                }
                ImGui::TreePop();
            }
            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
            ImGui::TextWrapped("%s", TR("Ratings update when both players report the same result — "
                                       "a casual ladder, not tournament-grade ranked."));
            ImGui::PopStyleColor();
        }

        if (ImGui::CollapsingHeader(TR("Advanced"))) {
            ImGui::TextUnformatted(TR("Lobby server"));
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##NetplayServer", serverBuf, sizeof(serverBuf))) {
                CVarSetString("gNetplay.Server", serverBuf);
                save();
            }
            if (ImGui::SmallButton(TR("Use default server"))) {
                std::snprintf(serverBuf, sizeof(serverBuf), "%s", PORT_NETLOBBY_DEFAULT_SERVER);
                CVarClear("gNetplay.Server");
                save();
            }
            ImGui::BeginDisabled(connTest.state == ConnTestState::Running);
            if (UIWidgets::Button(TR("Test connection"))) {
                StartConnTest(connTest, serverBuf);
            }
            ImGui::EndDisabled();
            if (connTest.state == ConnTestState::Running) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", TR("Testing..."));
            } else if (connTest.state == ConnTestState::Done && !connTest.result.empty()) {
                ImGui::TextColored(connTest.ok ? SemanticColors::Success : SemanticColors::Error, TR("%s"), connTest.result.c_str());
            }
            if (connTest.stun_started && connTest.stun_result.empty()) {
                ImGui::TextDisabled("%s", TR("Probing UDP / NAT..."));
            } else if (!connTest.stun_result.empty()) {
                ImGui::TextWrapped("%s", connTest.stun_result.c_str());
            }
        }
    }

    if (state == PORT_NETLOBBY_IN_MATCH) {
        ImGui::SeparatorText(TR("Match in progress"));
    } else {
        ImGui::SeparatorText(TR("Status"));
    }
    {
        const NetStatusView statusView = NetStatusViewFor(state);
        if (statusView.color != nullptr) {
            ImGui::TextColored(*statusView.color, TR("[%s]"), statusView.tag);
            ImGui::SameLine(0.0f, 6.0f);
        }
        ImGui::TextWrapped("%s", TR(port_netlobby_status()));
        if (statusView.hint != nullptr) {
            ImGui::TextDisabled(TR("%s"), statusView.hint);
        }
    }

    if (port_netlobby_code()[0] != '\0' && port_netlobby_is_host() && !idle) {
        ImGui::Text(TR("Room code: %s"), GroupRoomCode(port_netlobby_code()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Copy"))) {
            ImGui::SetClipboardText(port_netlobby_code());
            ToastSuccess(TR("Room code copied to clipboard"));
        }
    }
    if (state == PORT_NETLOBBY_CONNECTED || state == PORT_NETLOBBY_STARTING || state == PORT_NETLOBBY_IN_MATCH) {
        const int mySlot = port_netlobby_my_slot();
        const int peers = port_netlobby_peer_count();
        const int fighterCount = static_cast<int>(IM_ARRAYSIZE(kNetplayFighters));
        ImGui::Text(TR("Players in room: %d"), port_netlobby_player_count());
        ImGui::BulletText(TR("Player %d: %s (you)"), mySlot + 1, (fighter >= 0 && fighter < fighterCount) ? kNetplayFighters[fighter] : "?");
        ImGui::SameLine(0.0f, 4.0f);
        if (port_netlobby_is_host()) {
            ImGui::TextDisabled("%s", TR("— host"));
        } else {
            const bool meReady = port_netlobby_is_ready() != 0;
            ImGui::TextColored(meReady ? SemanticColors::Success : SemanticColors::Muted, "%s",
                               meReady ? TR("— ready") : TR("— not ready"));
        }
        for (int i = 0; i < peers; i++) {
            const int slot = port_netlobby_peer_slot(i);
            if (slot < 0 || slot == mySlot) {
                continue;
            }
            const char* label;
            int fk = port_netlobby_peer_fkind_at(i);
            if (!port_netlobby_peer_connected_at(i)) {
                label = TR("connecting...");
            } else if (fk >= 0 && fk < fighterCount) {
                label = kNetplayFighters[fk];
            } else {
                label = TR("choosing...");
            }
            ImGui::BulletText(TR("Player %d: %s"), slot + 1, label);
            ImGui::SameLine(0.0f, 4.0f);
            const char* pname = port_netlobby_peer_name_at(i);
            if (pname[0] != '\0') {
                ImGui::TextDisabled(TR("(%s)"), pname);
                ImGui::SameLine(0.0f, 4.0f);
            }
            if (slot == 0) {
                ImGui::TextDisabled("%s", TR("— host"));
            } else {
                const bool peerReady = port_netlobby_peer_ready_at(i) != 0;
                ImGui::TextColored(peerReady ? SemanticColors::Success : SemanticColors::Muted, "%s",
                                   peerReady ? TR("— ready") : TR("— not ready"));
            }
        }
        int ping = port_netlobby_ping_ms();
        if (ping >= 0) {
            /* Worst-peer loss rate feeds the tier too — low ping with
             * dropped packets still desyncs. Cumulative over the match. */
            double worstLoss = 0.0;
            const int peerCount = port_netlobby_peer_count();
            for (int i = 0; i < peerCount; i++) {
                unsigned g = 0, d = 0, o = 0, hw = 0;
                int adv = 0;
                if (port_netlobby_peer_stats_at(i, &g, &d, &o, &adv, &hw) && hw > 0) {
                    worstLoss = (std::max)(worstLoss, 100.0 * g / hw);
                }
            }
            int tier = ping < 50 ? 0 : ping < 100 ? 1 : 2;
            if (worstLoss >= 5.0) {
                tier = 2;
            } else if (worstLoss >= 2.0 && tier < 1) {
                tier = 1;
            }
            const char* quality = tier == 0 ? TR("Good") : tier == 1 ? TR("Fair") : TR("Poor");
            const ImVec4 qualityColor = tier == 0   ? SemanticColors::Success
                                        : tier == 1 ? SemanticColors::Warning
                                                    : SemanticColors::Error;
            ImGui::Text("%s", TR("Connection: "));
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextColored(qualityColor, TR("%s"), quality);
            ImGui::SameLine(0.0f, 0.0f);
            ImGui::TextDisabled(TR(" — %d ms, %s"), ping, port_netlobby_is_relay() ? TR("relayed") : TR("direct"));
            if (worstLoss > 0.05) {
                ImGui::SameLine(0.0f, 4.0f);
                ImGui::TextDisabled(TR("(loss %.1f%%)"), worstLoss);
            }
        }
        if (port_netlobby_is_host()) {
            if (autoDelay) {
                ImGui::TextDisabled(TR("Input delay: automatic, %d frames (applies to all players)"), effectiveDelay);
            } else {
                ImGui::TextDisabled(TR("Input delay: %d frames (applies to all players)"), delay);
            }
        }
        // §21 — per-peer wire stats: what "Poor" actually means. The ping
        // history sparkline makes jitter visible instead of summarizing it
        // away — samples every ~100 ms while connected, cleared on leave.
        const auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - sPingLast).count() >= 100) {
            sPingLast = now;
            const int p = port_netlobby_ping_ms();
            if (p >= 0) {
                sPingHistory[sPingHead] = static_cast<float>(p);
                sPingHead = (sPingHead + 1) % 120;
                sPingCount = (sPingCount < 120) ? sPingCount + 1 : 120;
            }
        }
        if (sPingCount > 2) {
            // Reorder the ring into draw order (oldest -> newest).
            float ordered[120];
            for (int i = 0; i < sPingCount; i++) {
                ordered[i] = sPingHistory[(sPingHead - sPingCount + i + 120) % 120];
            }
            float peak = 0.0f;
            for (int i = 0; i < sPingCount; i++) {
                peak = (std::max)(peak, ordered[i]);
            }
            ImGui::PlotLines("##pinghist", ordered, sPingCount, 0,
                             fmt::format(fmt::runtime(TR("ping {} ms (peak {:.0f})")), port_netlobby_ping_ms(), peak).c_str(),
                             0.0f, (std::max)(peak * 1.1f, 60.0f), ImVec2(-1.0f, 48.0f));
        }
        if (ImGui::TreeNode(TR("Connection details"))) {
            bool anyStats = false;
            for (int i = 0; i < peers; i++) {
                const int slot = port_netlobby_peer_slot(i);
                if (slot < 0 || slot == mySlot || !port_netlobby_peer_connected_at(i)) {
                    continue;
                }
                unsigned gaps = 0, dups = 0, ooo = 0, highwater = 0;
                int advantage = 0;
                const int peerPing = port_netlobby_peer_ping_at(i);
                ImGui::BulletText(TR("Player %d: "), slot + 1);
                ImGui::SameLine(0.0f, 0.0f);
                if (peerPing >= 0) {
                    ImGui::TextDisabled(TR("%d ms, %s"), peerPing, port_udp_link_is_relay(slot) ? TR("relayed") : TR("direct"));
                } else {
                    ImGui::TextDisabled(TR("ping unknown, %s"), port_udp_link_is_relay(slot) ? TR("relayed") : TR("direct"));
                }
                if (port_netlobby_peer_stats_at(i, &gaps, &dups, &ooo, &advantage, &highwater)) {
                    anyStats = true;
                    ImGui::SameLine(0.0f, 4.0f);
                    ImGui::TextDisabled(TR("— loss %u, dup %u, ooo %u, rollback %d frames"), gaps, dups, ooo, advantage);
                }
            }
            if (!anyStats) {
                ImGui::TextDisabled("%s", TR("Live stats appear once the match starts."));
            }
            ImGui::TreePop();
        }
    }
    if (!port_netlobby_is_host() && !port_netlobby_is_spectator() && state == PORT_NETLOBBY_CONNECTED) {
        const bool amReady = port_netlobby_is_ready() != 0;
        if (UIWidgets::Button(amReady ? TR("Cancel ready") : TR("I'm ready"),
                              UIWidgets::ButtonOptions().Color(amReady ? UIWidgets::Colors::Gray
                                                                       : UIWidgets::Colors::Green))) {
            port_netlobby_set_ready(amReady ? 0 : 1);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%s", TR("Flag yourself ready - changing your fighter un-readies you."));
        }
    }
    if (port_netlobby_is_host() && state == PORT_NETLOBBY_CONNECTED) {
        bool allPicked = true;
        bool anyPeer = false;
        const int peers = port_netlobby_peer_count();
        for (int i = 0; i < peers; i++) {
            if (port_netlobby_peer_connected_at(i)) {
                anyPeer = true;
                if (port_netlobby_peer_fkind_at(i) < 0) {
                    allPicked = false;
                    break;
                }
            }
        }
        const bool allReady = port_netlobby_all_ready() != 0;
        const bool canStart = anyPeer && allPicked && allReady;
        ImGui::BeginDisabled(!canStart);
        if (UIWidgets::Button(TR("Start match"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Green))) {
            port_netlobby_start_match();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !canStart) {
            if (anyPeer && allPicked && !allReady) {
                ImGui::SetTooltip("%s", TR("Waiting for every player to ready up."));
            } else {
                ImGui::SetTooltip("%s", TR("Waiting for every connected player to pick a fighter."));
            }
        }
    }
    if (!idle && state != PORT_NETLOBBY_IN_MATCH) {
        if (UIWidgets::Button(TR("Leave"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Red))) {
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
