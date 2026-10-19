// SPDX-License-Identifier: MIT
//
// Online-menu community panels — Friends / Leaderboard / Tournaments.
// Extracted from PortMenuOnline.cpp to keep each TU under the 1000-line
// file-size cap; called by RenderNetplayLobby while the lobby is idle.

#include "PortMenuOnlineBoards.h"

#include "PortI18n.h"
#include "Toasts.h"
#include "UIWidgets.hpp"
#include "netfriends.h"
#include "netleaderboard.h"

#include <ship/Context.h>
#include <ship/window/Window.h>
#include <ship/window/gui/Gui.h>

#include <cstdio>
#include <cstring>
#include <string>

#include <imgui.h>

namespace ssb64 {

using namespace UIWidgets;

/* Forward decls — helpers call each other regardless of order. */
static void RenderBoardRows(const char *serverBuf, int total, int myRank);
static void RenderRecentMatches();
static void RenderReportModal(const char *serverBuf, const char *name);
static void RenderPlayerCard();
static void RenderTournamentCreateBar(const char *serverBuf);
static void RenderBracketRounds(int bracketIdx, int rounds, int size);
static void RenderBracketDetail(const char *serverBuf, int bracketIdx,
                                const PortLbBracket &br);

void SaveOnlineCvars() {
    Ship::Context::GetInstance()->GetWindow()->GetGui()->SaveConsoleVariablesNextFrame();
}

/* Modal content for the report-player popup — opened via OpenPopup by the
 * caller when a row's Report button fires. */
/* Moderation badges on a leaderboard row — [banned] / provisional-[?]. */
static void RenderBoardRowFlags(int suspicious, int banned) {
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
}

/* Board UI state shared by RenderBoardRow/RenderBoardRows. */
static char sReportName[128] = {};
static bool sReportPopup = false;
static int sSelectedRow = -1;

static void RenderBoardRow(const char *serverBuf, int i, int myRank) {
    /* Server allows 24 Unicode chars — up to 96 bytes UTF-8. */
    char name[128] = {};
    int r = 0, w = 0, l = 0, suspicious = 0, banned = 0;
    if (!port_netleaderboard_at(i, name, sizeof(name), &r, &w, &l,
                                &suspicious, &banned)) {
        return;
    }
    const bool mine = (myRank == i + 1);
    /* Clicking a row loads that player's card (rank +
     * recent verified matches) from /player?name=. */
    char rowLabel[160];
    std::snprintf(rowLabel, sizeof(rowLabel), "#%d %s", i + 1, name);
    ImGui::PushID(i);
    if (mine) {
        ImGui::PushStyleColor(ImGuiCol_Text, SemanticColors::Success);
    }
    if (ImGui::Selectable(rowLabel, sSelectedRow == i)) {
        sSelectedRow = i;
        port_netleaderboard_fetch_player(serverBuf, name);
    }
    if (mine) {
        ImGui::PopStyleColor();
    }
    ImGui::PopID();
    RenderBoardRowFlags(suspicious, banned);
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

static void RenderBoardRows(const char *serverBuf, int total, int myRank) {
    for (int i = 0; i < total; i++) {
        RenderBoardRow(serverBuf, i, myRank);
    }
    if (sReportPopup) {
        ImGui::OpenPopup(TR("Report player?"));
        sReportPopup = false;
    }
    RenderReportModal(serverBuf, sReportName);
    /* Player card for the selected row — /player?name=X. */
    if (sSelectedRow >= 0) RenderPlayerCard();
}

static void RenderRecentMatches() {
    if (!ImGui::TreeNode(TR("Recent matches"))) {
        return;
    }
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

static void RenderReportModal(const char *serverBuf, const char *name) {
    if (ImGui::BeginPopupModal(TR("Report player?"), nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextWrapped("%s",
            TR("Report %s for cheating or abuse? The flag sticks when "
               "several distinct players report within 24 hours."), name);
        if (ImGui::Button(TR("Report"))) {
            port_netleaderboard_report(serverBuf, name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Cancel"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void RenderPlayerCard() {
    if (!ImGui::TreeNode(TR("Player card"))) {
        return;
    }
    const int pState = port_netleaderboard_player_state();
    if (pState == 1) {
        ImGui::TextDisabled("%s", TR("Loading..."));
    } else if (pState == -1) {
        ImGui::TextColored(SemanticColors::Error, "%s",
                           TR("Could not load this player."));
    } else if (pState == 2) {
        char pname[128] = {};
        int rank = 0, pr = 0, pw = 0, pl = 0;
        if (port_netleaderboard_player_card(pname, sizeof(pname),
                                            &rank, &pr, &pw, &pl)) {
            ImGui::Text("%s — #%d · %d · %d-%d", pname, rank, pr, pw, pl);
            const int pTotal = port_netleaderboard_player_matches_count();
            for (int m = 0; m < pTotal && m < 10; m++) {
                PortLbMatchRow row;
                if (!port_netleaderboard_player_match_at(m, &row)) {
                    continue;
                }
                if (row.draw) {
                    ImGui::BulletText(TR("%s drew %s"), row.a, row.b);
                } else {
                    ImGui::BulletText(TR("%s beat %s"), row.w, row.l);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("%s", row.stamp);
            }
        }
    }
    ImGui::TreePop();
}

void RenderOnlineFriends() {
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
}

void RenderOnlineLeaderboard(const char *serverBuf) {
    if (ImGui::CollapsingHeader(TR("Leaderboard"))) {
        /* Self-reported board on the lobby server — refreshed on
         * demand, rows cached between draws. */
        bool submitStats = CVarGetInteger("gNetplay.SubmitStats", 0) != 0;
        if (ImGui::Checkbox(TR("Publish my rating after matches"), &submitStats)) {
            CVarSetInteger("gNetplay.SubmitStats", submitStats ? 1 : 0);
            SaveOnlineCvars();
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
            RenderBoardRows(serverBuf, total, myRank);
        } else {
            ImGui::TextDisabled("%s", TR("Press Refresh to load the board."));
        }
        /* Verified match history rides the same Refresh — the service
         * keeps the last LB_HISTORY_MAX corroborated results. */
        RenderRecentMatches();
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
        ImGui::TextWrapped("%s", TR("Ratings update when both players report the same result — "
                                   "a casual ladder, not tournament-grade ranked."));
        ImGui::PopStyleColor();
    }
}

static void RenderTournamentCreateBar(const char *serverBuf) {
    static char sBracketName[64] = {};
    static int sBracketSize = 1;   /* index into kBracketSizes */
    static const int kBracketSizes[] = {4, 8, 16, 32};
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputTextWithHint("##bracketname", TR("Bracket name"),
                             sBracketName, sizeof(sBracketName));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    if (ImGui::BeginCombo("##bracketsize",
                          std::to_string(kBracketSizes[sBracketSize]).c_str())) {
        for (int s = 0; s < 4; s++) {
            if (ImGui::Selectable(std::to_string(kBracketSizes[s]).c_str(),
                                  sBracketSize == s)) {
                sBracketSize = s;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(TR("Create"))) {
        port_netleaderboard_tournament_create(serverBuf, sBracketName,
                                              kBracketSizes[sBracketSize]);
    }
}

static void RenderBracketRounds(int bracketIdx, int rounds, int size) {
    for (int r = 0; r < rounds; r++) {
        ImGui::TextDisabled(TR("Round %d"), r + 1);
        for (int s = 0; s < size; s++) {
            PortLbMatchRow pr;
            if (!port_netleaderboard_tournament_pairing(bracketIdx, r, s, &pr)) {
                break;
            }
            if (pr.a[0] == '\0' && pr.b[0] == '\0') {
                continue;   /* padded bye */
            }
            ImGui::BulletText("%s vs %s",
                              pr.a[0] ? pr.a : "—",
                              pr.b[0] ? pr.b : "—");
            ImGui::SameLine();
            if (pr.w[0] != '\0') {
                ImGui::TextDisabled("%s %s", TR("winner:"), pr.w);
            } else {
                ImGui::TextDisabled("%s", TR("open"));
            }
        }
    }
}

static void RenderBracketDetail(const char *serverBuf, int bracketIdx,
                                const PortLbBracket &br) {
    ImGui::TextDisabled(TR("host: %s"), br.owner);
    const int rounds = port_netleaderboard_tournament_rounds(bracketIdx);
    if (br.joined > 0 && rounds == 0) {
        /* Open bracket — list signups */
        ImGui::SameLine();
        ImGui::TextDisabled("·");
        for (int p = 0; p < br.joined; p++) {
            char pn[128] = {};
            if (port_netleaderboard_tournament_player_at(
                    bracketIdx, p, pn, sizeof(pn))) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", pn);
            }
        }
    }
    RenderBracketRounds(bracketIdx, rounds, br.size);
    const bool isOpen = std::strncmp(br.status, "open", 4) == 0;
    if (isOpen && !br.mine) {
        if (ImGui::SmallButton(TR("Join"))) {
            port_netleaderboard_tournament_join(serverBuf, br.id);
        }
    } else if (isOpen && br.owner_mine) {
        if (ImGui::SmallButton(TR("Start"))) {
            port_netleaderboard_tournament_start(serverBuf, br.id);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", TR("(you host this bracket)"));
    }
}

void RenderOnlineTournaments(const char *serverBuf) {
    if (ImGui::CollapsingHeader(TR("Tournaments"))) {
        const int tState = port_netleaderboard_tournament_state();
        if (ImGui::SmallButton(TR("Refresh##tournaments"))) {
            port_netleaderboard_fetch_tournaments(serverBuf);
        }
        RenderTournamentCreateBar(serverBuf);
        if (tState == 1) {
            ImGui::TextDisabled("%s", TR("Loading..."));
        } else if (tState == -1) {
            ImGui::TextColored(SemanticColors::Error, "%s",
                               TR("Could not load tournaments."));
        } else if (tState == 2) {
            const int tTotal = port_netleaderboard_tournament_count();
            if (tTotal == 0) {
                ImGui::TextDisabled("%s", TR("No tournaments yet — create one above."));
            }
            for (int i = 0; i < tTotal; i++) {
                PortLbBracket br;
                if (!port_netleaderboard_tournament_at(i, &br)) {
                    continue;
                }
                ImGui::PushID(i);
                char hdr[220];
                if (br.winner[0] != '\0') {
                    std::snprintf(hdr, sizeof(hdr), "%s — %s %s",
                                  br.name, TR("winner:"), br.winner);
                } else {
                    std::snprintf(hdr, sizeof(hdr), "%s — %s %d/%d",
                                  br.name, br.status, br.joined, br.size);
                }
                if (ImGui::TreeNode(hdr)) {
                    RenderBracketDetail(serverBuf, i, br);
                    ImGui::TreePop();
                }
                ImGui::PopID();
            }
        }
    }
}

} // namespace ssb64
