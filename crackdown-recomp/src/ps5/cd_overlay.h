// Crackdown PS5 - the frame rate counter (the menu's FPS COUNTER, crackdown_fps_counter), as GTA IV
// for PS5 draws it, at the top left: frames a second and the frame time; detailed, also
// the 1% low, the slowest frame and a graph of the last frames. The frames are the game's presents
// (rex::frame_stats, noted at the guest's swap).
#pragma once

#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <cstdint>

#include <rex/ui/imgui_dialog.h>

namespace cdps5 {

class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer);

 protected:
  void OnDraw(ImGuiIO& io) override;
  // Drawn with each game frame, not every vblank (a repaint every vblank queues repeats ahead of
  // the game's frames: GTA IV for PS5's ps5_overlay.h).
  bool WantsContinuousRepaint() const override { return false; }

 private:
  void Sample();

  uint64_t window_frames_ = 0;
  int64_t window_start_ns_ = 0;
  float fps_ = 0, ms_ = 0, low_fps_ = 0, worst_ms_ = 0;
};

}  // namespace cdps5

#endif  // REX_PLATFORM_PS5
