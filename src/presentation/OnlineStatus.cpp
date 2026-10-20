#include "OnlineStatus.h"

#include "UIWidgets.hpp"

#include <imgui.h>
#include "PortI18n.h"

namespace ssb64 {

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
    mView = mViewModel.Capture();
    const bool wanted = mView.mode != OnlineStatusMode::Hidden;
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
    if (mView.mode == OnlineStatusMode::Desynced) {
        ImGui::TextColored(UIWidgets::SemanticColors::Error, "%s", TR("Out of sync with the other players."));
        ImGui::TextWrapped("%s", TR("Your games no longer match - what you see may differ from what they see. "
                           "The result of this match may be inaccurate."));
        ImGui::Spacing();
        if (ImGui::Button(TR("Return to room"), ImVec2(140, 0))) {
            mViewModel.ReturnToRoom();
        }
        ImGui::SameLine();
        if (ImGui::Button(TR("Leave"), ImVec2(100, 0))) {
            mViewModel.Leave();
        }
        return;
    }

    ImGui::TextWrapped("%s", TR(mView.status.c_str()));
    ImGui::TextDisabled("%s", TR("Match complete - everyone is back in their own menus."));
    ImGui::Spacing();

    // Per-player state, mirroring the lobby list so "who is still picking"
    // is visible without opening the menu.
    for (const OnlinePeerView& peer : mView.peers) {
        ImGui::BulletText(TR("Player %d: %s"), peer.slot + 1, peer.label.c_str());
    }

    if (mView.pingMs >= 0) {
        ImGui::Text(TR("Ping: %d ms%s"), mView.pingMs, mView.isRelay ? " (relayed)" : "");
    }
    ImGui::Spacing();
    if (mView.isHost) {
        ImGui::BeginDisabled(!mView.rematchEnabled);
        if (ImGui::Button(TR("Rematch"), ImVec2(140, 0))) {
            mViewModel.Rematch();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !mView.rematchEnabled) {
            ImGui::SetTooltip("%s", TR("Waiting for the other players to pick a fighter."));
        }
        ImGui::SameLine();
    }
    if (ImGui::Button(TR("Leave"), ImVec2(100, 0))) {
        mViewModel.Leave();
    }
}

} // namespace ssb64
