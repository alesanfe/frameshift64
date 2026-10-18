#pragma once

#include <ship/window/gui/GuiWindow.h>

#include <string>

namespace ssb64 {

enum class ToastLevel {
    Info,
    Success,
    Warning,
    Error,
};

/* Non-blocking notification, rendered as a stacked card in the top-right
 * corner regardless of menu state. Safe to call from any UI action; the
 * message is copied, so callers may pass temporaries. */
void PushToast(ToastLevel level, std::string message);
void ToastInfo(std::string message);
void ToastSuccess(std::string message);
void ToastWarning(std::string message);
void ToastError(std::string message);

/* Top-right toast stack. Always "visible" — the window itself hides when
 * the queue is empty — and never takes input, so it cannot steal focus or
 * controller nav from the game or the menu. */
class ToastsWindow : public Ship::GuiWindow {
  public:
    ToastsWindow();
    void Draw() override;

  protected:
    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override;
};

} // namespace ssb64
