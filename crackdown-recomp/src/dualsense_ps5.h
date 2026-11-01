// The DualSense's light bar, adaptive triggers and added rumble for Crackdown (see dualsense_ps5.cpp).
#pragma once

#include <cstdint>

namespace crackdown {

// Once a main-loop frame (frame_pacing_ps5.cpp's FrameLoop): the player's state, for the controller.
void DualSenseFrame(uint8_t* base);

}  // namespace crackdown
