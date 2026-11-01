// Crackdown PS5 - the in-game quick menu: L3 + R3 together (both sticks pressed in) opens it over the
// game, to change the cheats (the orb markers among them), the FPS counter and the DualSense features
// without going back to the app's menu. While it's open the game reads an idle controller; O (or
// L3 + R3 again) closes it. The cheats are kept in /app0/cheats.ini, the rest in menu.ini.
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <rex/ui/imgui_dialog.h>

namespace cdps5 {

class QuickMenu final : public rex::ui::ImGuiDialog {
 public:
  explicit QuickMenu(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;
  // (every vblank only while it's open: drawn with the game's frames otherwise)
  bool WantsContinuousRepaint() const override;

 private:
  int row_ = 0;
  float shown_ = 0;  // 0..1, the fade
};

}  // namespace cdps5

#endif  // REX_PLATFORM_PS5
