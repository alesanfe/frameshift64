#include "OnlineStatus.h"

#include "netplay/session/netlobby.h"

#include <imgui.h>

extern "C" int syNetRollbackIsDesynced(void);

namespace ssb64 {

namespace {
bool InMatchDesynced() {
    return port_netlobby_state() == PORT_NETLOBBY_IN_MATCH && syNetRollbackIsDesynced() != 0;
}

bool AfterMatch() {
    return port_netlobby_after_match() != 0 && port_netlobby_state() == PORT_NETLOBBY_CONNECTED;
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
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "Out of sync with the other players.");
        ImGui::TextWrapped("Your games no longer match - what you see may differ from what they see. "
                           "Finish or quit this match and start a rematch.");
        return;
    }
    ImGui::TextWrapped("%s", port_netlobby_status());
    ImGui::TextDisabled("Everyone is back in their own menus. Use this window to play again.");
    const int ping = port_netlobby_ping_ms();
    if (ping >= 0) {
        ImGui::Text("Ping: %d ms%s", ping, port_netlobby_is_relay() ? " (relayed)" : "");
    }
    ImGui::Spacing();
    if (port_netlobby_is_host()) {
        ImGui::BeginDisabled(port_netlobby_peer_fkind() < 0);
        if (ImGui::Button("Start rematch", ImVec2(160, 0))) {
            port_netlobby_start_match();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
    }
    if (ImGui::Button("Leave", ImVec2(120, 0))) {
        port_netlobby_cancel();
    }
}

} // namespace ssb64
