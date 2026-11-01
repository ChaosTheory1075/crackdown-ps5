// The cheats on the PS5: the PC recomp's cheats.cpp, cheat_hooks.cpp and orb_map.cpp (copied in by
// ci/local_build.sh) bound to Title Update 6's functions (cheats_ps5.cpp), their values kept in
// /app0/cheats.ini and changed from the in-game quick menu (ps5/cd_quick_menu.h).
#pragma once

#include <filesystem>

namespace crackdown::cheats_ps5 {

// The saved values (key = value lines, the PC recomp's cheat keys); missing ones are off.
void Load(const std::filesystem::path& file);
// The values in effect now.
void Save(const std::filesystem::path& file);

}  // namespace crackdown::cheats_ps5
