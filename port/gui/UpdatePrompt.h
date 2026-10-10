#pragma once

#include <ship/window/gui/GuiWindow.h>

namespace ssb64 {

/* Startup "update available" prompt (like Jump Circuit's): shown once per
 * launch when the updater finds a newer release that the player hasn't
 * skipped. Update now / Later / Skip this version. */
class UpdatePromptWindow : public Ship::GuiWindow {
  public:
    UpdatePromptWindow();
    void Draw() override;

  protected:
    void InitElement() override;
    void DrawElement() override;
    void UpdateElement() override;

  private:
    bool mPrompted = false;
};

} // namespace ssb64
