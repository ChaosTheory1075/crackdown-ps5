// The cheats on the PS5 (see cheats_ps5.h): the PC recomp's hooks bound to Title Update 6's guest
// functions (saulob/crackdown-xbox360-recompiled, guest_bindings_tu6.cpp). The character's damage
// entry and the two weapon fire paths are bound in dualsense_ps5.cpp, which hands them on to these.

#include "cheats_ps5.h"

#include <cstdio>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "cheat_hooks.h"
#include "cheats.h"
#include "guest_hooks.h"
#include "guest_layout.h"
#include "guest_memory.h"

// (the orb map's picture shape: the PS5 shows the game's own 16:9)
REXCVAR_DEFINE_STRING(crackdown_aspect_ratio, "16:9", "Crackdown", "The picture's shape (16:9 on the PS5)");

namespace crackdown::cheats_ps5 {

void Load(const std::filesystem::path& file) {
  using namespace crackdown::cheats;
  std::FILE* f = std::fopen(file.string().c_str(), "rb");
  if (!f) return;
  char line[256];
  while (std::fgets(line, sizeof(line), f)) {
    char key[64] = {};
    int value = 0;
    if (std::sscanf(line, " %63[a-z_] = %d", key, &value) != 2) continue;
    for (const CheatInfo& info : kCheats)
      if (key == std::string(info.key)) SetSavedValue(info.cheat, value);
  }
  std::fclose(f);
  ResetSession();
}

void Save(const std::filesystem::path& file) {
  using namespace crackdown::cheats;
  std::FILE* f = std::fopen(file.string().c_str(), "wb");
  if (!f) {
    REXLOG_WARN("Cheats: cannot write {}", file.string());
    return;
  }
  for (const CheatInfo& info : kCheats) std::fprintf(f, "%s = %d\n", info.key, Value(info.cheat));
  std::fclose(f);
}

}  // namespace crackdown::cheats_ps5

// The Jump Height cheat. Title Update 6's jump launch (sub_8231E980, the base game's sub_822ECED0)
// no longer writes the launch speed into the character: it hands the jump's height and time to a
// virtual of the character's motion object (+1260), which solves the launch from them. Codegen hook
// right after the height is worked out (fmuls f30,f1,f31: the action's height times the
// character's own factor), with r11 = the action's owner (+252 its character): the height is
// scaled by the multiplier, the time kept (ci/local_build.sh adds the hook to the overrides).
void crackdown_jump_height(PPCRegister& height, PPCRegister& owner) {
  using namespace crackdown;
  if (!cheats::Enabled(cheats::Cheat::JumpHeight)) return;
  uint8_t* base = rex::system::kernel_state()->memory()->virtual_membase();
  if (!guest::Plausible(owner.u32) || guest::Load32(base, Layout().coop_session) != 0) return;
  const uint32_t character = guest::Load32(base, owner.u32 + Layout().action_character);
  if (!character || character != cheats::LocalPlayerCharacter(base)) return;
  height.f64 = double(float(height.f64 * double(cheats::Factor(cheats::Cheat::JumpHeight))));
}

#define CD_BIND(address, handler)   \
  REX_EXTERN(__imp__sub_##address); \
  REX_HOOK_RAW(sub_##address) { crackdown::hooks::handler(ctx, base, __imp__sub_##address); }

CD_BIND(82527AC8, VehicleDamage)
CD_BIND(8250EBC8, VehicleDamage)
CD_BIND(82514B78, VehicleDamage)
CD_BIND(82533750, VehicleDamage)
CD_BIND(824F1660, EngineTorque)
CD_BIND(8255ED88, WalkVelocity)

CD_BIND(8254EF28, OrbConstructed)
CD_BIND(8254D8F8, OrbDestroyed)
CD_BIND(826B3E98, OrbMapViewConstructed)
CD_BIND(826B4848, OrbMapViewDestroyed)
CD_BIND(8269A8C0, OrbFrontendScreenConstructed)
CD_BIND(8269A9D8, OrbFrontendScreenDestroyed)
CD_BIND(824324E0, OrbListDescriptorShown)
CD_BIND(82432728, OrbListDescriptorPosition)
CD_BIND(82432CD8, OrbMinimapProjection)
