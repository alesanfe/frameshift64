#include "OnlineStatus.h"

#include "UIWidgets.hpp"

#include "netlobby.h"

#include <imgui.h>
#include "PortI18n.h"

extern "C" int syNetRollbackIsDesynced(void);

namespace ssb64 {

namespace {
bool InMatchDesynced() {
    return port_netlobby_state() == PORT_NETLOBBY_IN_MATCH && syNetRollbackIsDesynced() != 0;
}

bool AfterMatch() {
    return port_netlobby_after_match() != 0 && port_netlobby_state() == PORT_NETLOBBY_CONNECTED;
}

const char* const kFighterNames[] = { "Mario",  "Fox",     "Donkey Kong", "Samus", "Link",    "Yoshi",
                                      "Kirby",  "Captain Falcon",        "Pikachu", "Jigglypuff", "Ness" };

const char* PeerStateLabel(int i) {
    if (!port_netlobby_peer_connected_at(i)) {
        return "connecting...";
    }
    const int fk = port_netlobby_peer_fkind_at(i);
    if (fk >= 0 && fk < static_cast<int>(IM_ARRAYSIZE(kFighterNames))) {
        return kFighterNames[fk];
    }
    return "choosing...";
}
} // namespace

OnlineStatusWindow::OnlineStatusWindow()
    : Ship::GuiWindow("gOnlineStatus.Visible", false, "Online", ImVec2(-1, -1),
                      ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize |
                          ImGuiWindowFlags_NoSavedSettings) {
}

void OnlineStatusWindow::InitElement() {
    Hide();
}

void OnlineStatusWindow::UpdateElement() {
}

void OnlineStatusWindow::Draw() {
    const bool wanted = InMatchDesynced() || AfterMatch();
    if (wanted != IsVisible()) {
        if (wanted) {
            Show();
        } else {
            Hide();
        }
    }
    if (!IsVisible()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->GetCenter().x, viewport->WorkPos.y + 12.0f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.0f));
    Ship::GuiWindow::Draw();
}

void OnlineStatusWindow::DrawElement() {
    if (InMatchDesynced()) {
        ImGui::TextColored(UIWidgets::SemanticColors::Error, "%s", TR("Out of sync with the other players."));
        ImGui::TextWrapped("%s", TR("Your games no longer match - what you see may differ from what they see. "
                           "The result of this match may be inaccurate."));
        ImGui::Spacing();
        if (ImGui::Button(TR("Return to room"), ImVec2(140, 0))) {
            port_netlobby_on_match_end();
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Leave"), ImVec2(100, 0))) {
            port_netlobby_cancel();
        }
        return;
    }

    ImGui::TextWrapped("%s", TR(port_netlobby_status()));
    ImGui::TextDisabled("%s", TR("Match complete - everyone is back in their own menus."));
    ImGui::Spacing();

    // Per-player state, mirroring the lobby list so "who is still picking"
    // is visible without opening the menu.
    const int peers = port_netlobby_peer_count();
    const int mySlot = port_netlobby_my_slot();
    for (int i = 0; i < peers; i++) {
        const int slot = port_netlobby_peer_slot(i);
        if (slot < 0 || slot == mySlot) {
            continue;
        }
        ImGui::BulletText(TR("Player %d: %s"), slot + 1, PeerStateLabel(i));
    }

    const int ping = port_netlobby_ping_ms();
    if (ping >= 0) {
        ImGui::Text(TR("Ping: %d ms%s"), ping, port_netlobby_is_relay() ? " (relayed)" : "");
    }
    ImGui::Spacing();
    if (port_netlobby_is_host()) {
        ImGui::BeginDisabled(port_netlobby_peer_fkind() < 0);
        if (ImGui::Button(TR("Rematch"), ImVec2(140, 0))) {
            port_netlobby_start_match();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && port_netlobby_peer_fkind() < 0) {
            ImGui::SetTooltip("%s", TR("Waiting for the other players to pick a fighter."));
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(TR("Leave"), ImVec2(100, 0))) {
        port_netlobby_cancel();
    }
}

} // namespace ssb64
