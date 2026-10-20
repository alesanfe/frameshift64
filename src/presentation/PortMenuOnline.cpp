// SPDX-License-Identifier: MIT
//
// Portions of this file are derived from the Ship of Harkinian (SoH) project
//   Copyright (c) The Harbour Masters
//   https://github.com/HarbourMasters/Shipwright
// Licensed under the MIT License; see LICENSE at repository root.

#include "PortMenu.h"
#include "PortMenuOnlineBoards.h"
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
    std::chrono::steady_clock::time_point started;
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



/* Per-frame lobby view state: the CVar mirror + lobby snapshot the panel
 * helpers read. `save` is the persistence callback (SaveOnlineCvars). */
struct LobbyFrameState {
    int fighter;
    int stage;
    int stocks;
    bool itemsOn;
    int delay;
    bool autoDelay;
    int effectiveDelay;
    int state;
    bool idle;
    void (*save)(void);
};

/* Server buffer + connection probe are shared by several sections, so
 * they live at file scope (previously function-statics in the monolith). */
static char sServerBuf[128] = {};
static ConnTest sConnTest;

static void RenderHowToPlay() {
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
}

static void RenderHostSection(LobbyFrameState& S) {
    ImGui::SeparatorText(TR("Host a match"));
    ImGui::TextUnformatted(TR("Stage"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##NetplayStage", &S.stage, kNetplayStages, IM_ARRAYSIZE(kNetplayStages))) {
        CVarSetInteger("gNetplay.Stage", S.stage);
        S.save();
    }
    ImGui::TextUnformatted(TR("Stocks"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderInt("##NetplayStocks", &S.stocks, 1, 5)) {
        CVarSetInteger("gNetplay.Stocks", S.stocks);
        S.save();
    }
    if (ImGui::Checkbox(TR("Items"), &S.itemsOn)) {
        CVarSetInteger("gNetplay.Items", S.itemsOn ? 1 : 0);
        S.save();
    }
    ImGui::TextUnformatted(TR("Input delay"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    {
        // ~16.6ms per frame: cover the round trip plus jitter margin.
        const std::string delayPreview =
            S.autoDelay ? TR("Automatic (recommended)")
                        : fmt::format(fmt::runtime(TR("{} frames")), S.delay);
        if (ImGui::BeginCombo("##NetplayDelay", delayPreview.c_str())) {
            if (ImGui::Selectable(TR("Automatic (recommended)"), S.autoDelay)) {
                CVarSetInteger("gNetplay.AutoDelay", 1);
                S.save();
            }
            for (int v = 0; v <= 4; v++) {
                const std::string lbl = fmt::format(fmt::runtime(TR("{} frames")), v);
                if (ImGui::Selectable(lbl.c_str(), !S.autoDelay && S.delay == v)) {
                    CVarSetInteger("gNetplay.AutoDelay", 0);
                    CVarSetInteger("gNetplay.Delay", v);
                    S.save();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    if (S.autoDelay) {
        ImGui::TextWrapped(TR("Automatic: resolves from measured ping at room start (now: %d "
                              "frames). Everyone in the room plays with the host's value."),
                           S.effectiveDelay);
    } else {
        ImGui::TextWrapped("%s", TR("Recommended: 2 for most connections. Raise it on high-ping "
                           "links to reduce visible rollbacks; everyone in the room "
                           "plays with the host's value."));
    }
    ImGui::PopStyleColor();
    if (UIWidgets::Button(TR("Host"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
        port_netlobby_host(sServerBuf, S.fighter, S.stage, S.stocks, S.itemsOn ? 1 : 0, S.effectiveDelay);
    }
}

static void RenderRecentMatches() {
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
}

/* Local rating mirror: rating, record and history live in netstats.json
 * next to the save file. */
static void RenderQuickPlayStats() {
    const int rating = port_netstats_rating();
    const int n = port_netstats_wins() + port_netstats_losses() + port_netstats_draws();
    char seasonBuf[16] = {};
    port_netstats_season(seasonBuf, sizeof(seasonBuf));
    if (seasonBuf[0] != '\0') {
        ImGui::TextDisabled(TR("Season %s"), seasonBuf);
    }
    if (n == 0) {
        ImGui::TextDisabled("%s", TR("No online matches on record yet."));
        return;
    }
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
        RenderRecentMatches();
        ImGui::TreePop();
    }
}

static void RenderQuickPlaySection(LobbyFrameState& S) {
    ImGui::SeparatorText(TR("Quick play"));
    RenderQuickPlayStats();
    if (UIWidgets::Button(TR("Find match"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
        port_netlobby_quickplay(sServerBuf, S.fighter, S.stage, S.stocks, S.itemsOn ? 1 : 0, S.effectiveDelay);
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s", TR("Joins the public queue — pairs with whoever else is waiting.\n"
                                  "Your rating is shared with the opponent and updates after the match."));
    }
}

static void RenderJoinSection(LobbyFrameState& S) {
    static char codeBuf[8] = {};
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
        port_netlobby_join(sServerBuf, codeBuf, S.fighter);
    }
    ImGui::BeginDisabled(!codeValid);
    if (UIWidgets::Button(TR("Join"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
        port_netlobby_join(sServerBuf, codeBuf, S.fighter);
    }
    ImGui::SameLine();
    if (UIWidgets::Button(TR("Spectate"))) {
        port_netlobby_spectate(sServerBuf, codeBuf);
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
}

static void RenderLanSection(LobbyFrameState& S) {
    static char lanAddr[64] = {};
    static int lanPort = 41700;
    ImGui::SeparatorText(TR("LAN (direct IP)"));
    ImGui::TextUnformatted(TR("Host port"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputInt("##LanPort", &lanPort, 0, 0);
    lanPort = std::clamp(lanPort, 1, 65535);
    if (UIWidgets::Button(TR("Host LAN"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
        port_netlobby_lan_host(lanPort, S.fighter, S.stage, S.stocks, S.itemsOn ? 1 : 0, S.effectiveDelay);
    }
    ImGui::TextUnformatted(TR("Host address (ip[:port])"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::InputText("##LanAddr", lanAddr, sizeof(lanAddr));
    if (UIWidgets::Button(TR("Join LAN"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::LightBlue))) {
        port_netlobby_lan_join(lanAddr, S.fighter);
    }
}

static void RenderAdvancedSection(LobbyFrameState& S) {
    if (!ImGui::CollapsingHeader(TR("Advanced"))) {
        return;
    }
    ImGui::TextUnformatted(TR("Lobby server"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::InputText("##NetplayServer", sServerBuf, sizeof(sServerBuf))) {
        CVarSetString("gNetplay.Server", sServerBuf);
        S.save();
    }
    if (ImGui::SmallButton(TR("Use default server"))) {
        std::snprintf(sServerBuf, sizeof(sServerBuf), "%s", PORT_NETLOBBY_DEFAULT_SERVER);
        CVarClear("gNetplay.Server");
        S.save();
    }
    ImGui::BeginDisabled(sConnTest.state == ConnTestState::Running);
    if (UIWidgets::Button(TR("Test connection"))) {
        StartConnTest(sConnTest, sServerBuf);
    }
    ImGui::EndDisabled();
    if (sConnTest.state == ConnTestState::Running) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("Testing..."));
    } else if (sConnTest.state == ConnTestState::Done && !sConnTest.result.empty()) {
        ImGui::TextColored(sConnTest.ok ? SemanticColors::Success : SemanticColors::Error, TR("%s"), sConnTest.result.c_str());
    }
    if (sConnTest.stun_started && sConnTest.stun_result.empty()) {
        ImGui::TextDisabled("%s", TR("Probing UDP / NAT..."));
    } else if (!sConnTest.stun_result.empty()) {
        ImGui::TextWrapped("%s", sConnTest.stun_result.c_str());
    }
}

static void RenderStatusLine(LobbyFrameState& S) {
    if (S.state == PORT_NETLOBBY_IN_MATCH) {
        ImGui::SeparatorText(TR("Match in progress"));
    } else {
        ImGui::SeparatorText(TR("Status"));
    }
    {
        const NetStatusView statusView = NetStatusViewFor(S.state);
        if (statusView.color != nullptr) {
            ImGui::TextColored(*statusView.color, TR("[%s]"), statusView.tag);
            ImGui::SameLine(0.0f, 6.0f);
        }
        ImGui::TextWrapped("%s", TR(port_netlobby_status()));
        if (statusView.hint != nullptr) {
            ImGui::TextDisabled(TR("%s"), statusView.hint);
        }
    }

    if (port_netlobby_code()[0] != '\0' && port_netlobby_is_host() && !S.idle) {
        ImGui::Text(TR("Room code: %s"), GroupRoomCode(port_netlobby_code()).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(TR("Copy"))) {
            ImGui::SetClipboardText(port_netlobby_code());
            ToastSuccess(TR("Room code copied to clipboard"));
        }
    }
}

static const char* PeerFighterLabel(int i, int fighterCount) {
    int fk = port_netlobby_peer_fkind_at(i);
    if (!port_netlobby_peer_connected_at(i)) {
        return TR("connecting...");
    }
    if (fk >= 0 && fk < fighterCount) {
        return kNetplayFighters[fk];
    }
    return TR("choosing...");
}

static void RenderReadyTag(bool ready) {
    ImGui::TextColored(ready ? SemanticColors::Success : SemanticColors::Muted, "%s",
                       ready ? TR("— ready") : TR("— not ready"));
}

static void RenderRosterPeerList(LobbyFrameState& S, int peers, int mySlot, int fighterCount) {
    ImGui::Text(TR("Players in room: %d"), port_netlobby_player_count());
    ImGui::BulletText(TR("Player %d: %s (you)"), mySlot + 1, (S.fighter >= 0 && S.fighter < fighterCount) ? kNetplayFighters[S.fighter] : "?");
    ImGui::SameLine(0.0f, 4.0f);
    if (port_netlobby_is_host()) {
        ImGui::TextDisabled("%s", TR("— host"));
    } else {
        RenderReadyTag(port_netlobby_is_ready() != 0);
    }
    for (int i = 0; i < peers; i++) {
        const int slot = port_netlobby_peer_slot(i);
        if (slot < 0 || slot == mySlot) {
            continue;
        }
        ImGui::BulletText(TR("Player %d: %s"), slot + 1, PeerFighterLabel(i, fighterCount));
        ImGui::SameLine(0.0f, 4.0f);
        const char* pname = port_netlobby_peer_name_at(i);
        if (pname[0] != '\0') {
            ImGui::TextDisabled(TR("(%s)"), pname);
            ImGui::SameLine(0.0f, 4.0f);
        }
        if (slot == 0) {
            ImGui::TextDisabled("%s", TR("— host"));
        } else {
            RenderReadyTag(port_netlobby_peer_ready_at(i) != 0);
        }
    }
}

static double WorstPeerLossPct() {
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
    return worstLoss;
}

static int ConnectionTier(int ping, double worstLoss) {
    int tier = ping < 50 ? 0 : ping < 100 ? 1 : 2;
    if (worstLoss >= 5.0) {
        return 2;
    }
    if (worstLoss >= 2.0 && tier < 1) {
        return 1;
    }
    return tier;
}

static void RenderConnectionQuality() {
    const int ping = port_netlobby_ping_ms();
    if (ping < 0) {
        return;
    }
    const double worstLoss = WorstPeerLossPct();
    const int tier = ConnectionTier(ping, worstLoss);
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

static void RenderPingSparkline(bool idle) {
    static float sPingHistory[120] = {};
    static int sPingHead = 0;
    static int sPingCount = 0;
    static std::chrono::steady_clock::time_point sPingLast;
    if (idle) {
        sPingCount = 0;
        sPingHead = 0;
        return;
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
}

static void RenderPeerDetailTree(int peers, int mySlot) {
    if (!ImGui::TreeNode(TR("Connection details"))) {
        return;
    }
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

static void RenderRosterSection(LobbyFrameState& S) {
    const int mySlot = port_netlobby_my_slot();
    const int peers = port_netlobby_peer_count();
    const int fighterCount = static_cast<int>(IM_ARRAYSIZE(kNetplayFighters));
    RenderRosterPeerList(S, peers, mySlot, fighterCount);
    RenderConnectionQuality();
    if (port_netlobby_is_host()) {
        if (S.autoDelay) {
            ImGui::TextDisabled(TR("Input delay: automatic, %d frames (applies to all players)"), S.effectiveDelay);
        } else {
            ImGui::TextDisabled(TR("Input delay: %d frames (applies to all players)"), S.delay);
        }
    }
    RenderPingSparkline(S.idle);
    RenderPeerDetailTree(peers, mySlot);
}

static void RenderReadyButton() {
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

static void RenderStartMatchButton() {
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

static void RenderLobbyActions(LobbyFrameState& S) {
    if (!port_netlobby_is_host() && !port_netlobby_is_spectator() && S.state == PORT_NETLOBBY_CONNECTED) {
        RenderReadyButton();
    }
    if (port_netlobby_is_host() && S.state == PORT_NETLOBBY_CONNECTED) {
        RenderStartMatchButton();
    }
    if (!S.idle && S.state != PORT_NETLOBBY_IN_MATCH) {
        if (UIWidgets::Button(TR("Leave"), UIWidgets::ButtonOptions().Color(UIWidgets::Colors::Red))) {
            port_netlobby_cancel();
        }
    }
}

static void LoadLobbyFrameState(LobbyFrameState& S) {
    S.fighter = CVarGetInteger("gNetplay.Fighter", 0);
    S.stage = CVarGetInteger("gNetplay.Stage", 0);
    S.stocks = CVarGetInteger("gNetplay.Stocks", 3);
    S.itemsOn = CVarGetInteger("gNetplay.Items", 0) != 0;
    S.delay = CVarGetInteger("gNetplay.Delay", 2);
    // §22 — "Automatic" is a persistent mode, not a one-shot copy: the
    // delay resolves from measured ping each time a session is hosted
    // (fallback 2 before any ping is known).
    S.autoDelay = CVarGetInteger("gNetplay.AutoDelay", 1) != 0;
    const int lobbyPing = port_netlobby_ping_ms();
    const int pingRecommended = lobbyPing < 30 ? 1 : lobbyPing < 70 ? 2 : lobbyPing < 110 ? 3 : 4;
    S.effectiveDelay = S.autoDelay ? (lobbyPing >= 0 ? pingRecommended : 2) : S.delay;
    S.save = SaveOnlineCvars;

    S.state = port_netlobby_state();
    S.idle = (S.state == PORT_NETLOBBY_IDLE || S.state == PORT_NETLOBBY_FAILED);
}

static void RenderFighterPicker(LobbyFrameState& S) {
    ImGui::BeginDisabled(S.state == PORT_NETLOBBY_STARTING || S.state == PORT_NETLOBBY_IN_MATCH);
    ImGui::TextUnformatted(TR("Your fighter"));
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##NetplayFighter", &S.fighter, kNetplayFighters, IM_ARRAYSIZE(kNetplayFighters))) {
        CVarSetInteger("gNetplay.Fighter", S.fighter);
        port_netlobby_set_fkind(S.fighter);
        S.save();
    }
    ImGui::EndDisabled();
}

static void RenderNetplayLobby(WidgetInfo& /*widget*/) {
    static bool loaded = false;
    if (!loaded) {
        std::snprintf(sServerBuf, sizeof(sServerBuf), "%s", CVarGetString("gNetplay.Server", PORT_NETLOBBY_DEFAULT_SERVER));
        loaded = true;
    }

    LobbyFrameState S = {};
    LoadLobbyFrameState(S);
    if (!S.idle && sConnTest.state == ConnTestState::Running) {
        // Lobby session claimed the shared control socket — abandon the probe.
        sConnTest.state = ConnTestState::Idle;
        sConnTest.result.clear();
    }
    if (S.idle) {
        RenderPingSparkline(true);   /* drain-on-idle keeps the ring reset */
    }
    PollConnTest(sConnTest);

    ImGui::TextWrapped("%s", TR("Play online with rollback netcode - up to 4 players in one room. One player hosts "
                       "and shares the room code; the others join with it. No port forwarding needed."));
    ImGui::Spacing();

    RenderFighterPicker(S);

    /* Connect view — only while idle. Once you're in a room the panel swaps
     * to the room/status view below so the creation controls can't be
     * mistaken for live settings. */
    if (S.idle) {
        RenderHowToPlay();
        RenderHostSection(S);
        RenderQuickPlaySection(S);
        RenderJoinSection(S);
        RenderLanSection(S);
        RenderOnlineFriends();
        RenderOnlineLeaderboard(sServerBuf);
        /* Community brackets — the lobby service runs single-elimination
         * tournaments that settle off verified leaderboard results. */
        RenderOnlineTournaments(sServerBuf);
        RenderAdvancedSection(S);
    }

    RenderStatusLine(S);

    if (S.state == PORT_NETLOBBY_CONNECTED || S.state == PORT_NETLOBBY_STARTING || S.state == PORT_NETLOBBY_IN_MATCH) {
        RenderRosterSection(S);
    }
    RenderLobbyActions(S);
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
