#pragma once

#include <ship/window/gui/GuiWindow.h>

namespace ssb64 {

/* Online play overlay, drawn even with the Esc menu closed:
 *   - in a match: a warning if the two games went out of sync;
 *   - after a match: where the players are (each back in their own menus),
 *     connection info, and Start rematch (host) / Leave. */
class OnlineStatusWindow : public Ship::GuiWindow {
  public:
    OnlineStatusWindow();
    void Draw() override;

  protected:
    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override;
};

} // namespace ssb64
