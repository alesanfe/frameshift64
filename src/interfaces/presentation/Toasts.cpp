#include "Toasts.h"

#include "UIWidgets.hpp"

#include <ship/window/gui/IconsFontAwesome4.h>

#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <utility>
#include "core/i18n/Locale.h"

namespace ssb64 {

namespace {

struct Toast {
    ToastLevel level;
    std::string message;
    std::chrono::steady_clock::time_point created;
    float lifetime; // seconds on screen
};

std::deque<Toast>& Queue() {
    static std::deque<Toast> q;
    return q;
}

constexpr size_t kMaxVisible = 5;
constexpr float kFadeOut = 0.8f;

float LifetimeFor(ToastLevel level) {
    switch (level) {
        case ToastLevel::Success:
        case ToastLevel::Info:
            return 4.0f;
        case ToastLevel::Warning:
            return 6.0f;
        case ToastLevel::Error:
            return 8.0f;
    }
    return 4.0f;
}

struct ToastStyle {
    const char* icon;
    ImVec4 color;
};

ToastStyle StyleFor(ToastLevel level) {
    switch (level) {
        case ToastLevel::Success:
            return { ICON_FA_CHECK_CIRCLE, UIWidgets::SemanticColors::Success };
        case ToastLevel::Warning:
            return { ICON_FA_EXCLAMATION_TRIANGLE, UIWidgets::SemanticColors::Warning };
        case ToastLevel::Error:
            return { ICON_FA_TIMES_CIRCLE, UIWidgets::SemanticColors::Error };
        case ToastLevel::Info:
        default:
            return { ICON_FA_INFO_CIRCLE, UIWidgets::SemanticColors::Info };
    }
}

} // namespace

void PushToast(ToastLevel level, std::string message) {
    Queue().push_back({ level, std::move(message), std::chrono::steady_clock::now(), LifetimeFor(level) });
    while (Queue().size() > kMaxVisible + 3) {
        Queue().pop_front();
    }
}

void ToastInfo(std::string message) {
    PushToast(ToastLevel::Info, std::move(message));
}

void ToastSuccess(std::string message) {
    PushToast(ToastLevel::Success, std::move(message));
}

void ToastWarning(std::string message) {
    PushToast(ToastLevel::Warning, std::move(message));
}

void ToastError(std::string message) {
    PushToast(ToastLevel::Error, std::move(message));
}

ToastsWindow::ToastsWindow() : Ship::GuiWindow("", true, "Toasts") {
}

void ToastsWindow::InitElement() {
    Show();
}

void ToastsWindow::UpdateElement() {
}

void ToastsWindow::DrawElement() {
    // Unused — Draw() renders the overlay directly so we control window
    // flags (non-interactive, undecorated, viewport-anchored).
}

void ToastsWindow::Draw() {
    auto& q = Queue();
    const auto now = std::chrono::steady_clock::now();

#ifdef SSB64_TEST_HOOKS
    /* SSB64_TOAST_DEMO=1 keeps one toast of each level on screen — lets the
     * menu-screenshot tool capture the overlay without a live action.
     * Re-seeds each level independently since their lifetimes differ. */
    if (std::getenv("SSB64_TOAST_DEMO") != nullptr) {
        static const struct {
            ToastLevel level;
            const char* message;
        } kDemo[] = {
            { ToastLevel::Info, "Room code copied to clipboard" },
            { ToastLevel::Success, "Texture pack installed and enabled" },
            { ToastLevel::Warning, "Mod signature invalid" },
            { ToastLevel::Error, "Pack install failed" },
        };
        for (const auto& demo : kDemo) {
            const bool present =
                std::any_of(q.begin(), q.end(), [&](const Toast& t) { return t.level == demo.level; });
            if (!present) {
                PushToast(demo.level, demo.message);
            }
        }
    }
#endif

    while (!q.empty() && std::chrono::duration<float>(now - q.front().created).count() > q.front().lifetime) {
        q.pop_front();
    }
    if (q.empty()) {
        return;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - 14.0f, viewport->WorkPos.y + 14.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::SetNextWindowBgAlpha(0.88f);
    ImGui::Begin("##Toasts", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);

    size_t shown = 0;
    for (auto it = q.rbegin(); it != q.rend() && shown < kMaxVisible; ++it, ++shown) {
        const float age = std::chrono::duration<float>(now - it->created).count();
        const float alpha = std::clamp((it->lifetime - age) / kFadeOut, 0.0f, 1.0f);
        const ToastStyle style = StyleFor(it->level);

        ImGui::PushID(static_cast<int>(shown));
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
        ImGui::TextColored(style.color, TR("%s"), style.icon);
        ImGui::SameLine();
        /* AlwaysAutoResize + TextWrapped collapses to a one-char column —
         * pin the wrap point so toasts keep a readable width. */
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 220.0f);
        ImGui::TextWrapped(TR("%s"), it->message.c_str());
        ImGui::PopTextWrapPos();
        if (shown + 1 < (std::min)(q.size(), kMaxVisible)) {
            ImGui::Separator();
        }
        ImGui::PopStyleVar();
        ImGui::PopID();
    }

    ImGui::End();
}

} // namespace ssb64
