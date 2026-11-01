// The DualSense's light bar, adaptive triggers and added rumble for Crackdown (Title Update 6 addresses).
//
// The runtime's PS5 controller driver already plays the game's own rumble and drives the light bar,
// the triggers and short added pulses from what a game tells rex::input::sony (GTA IV's model): here
// Crackdown tells it, once a frame, where the player is, and the shots and hits as they happen.
//
// - Light bar: the Agency's green; red for a moment when the agent is hit, pulsing red while their
//   health is low.
// - Triggers: driving, L2 and R2 push back like pedals; on foot with a gun, R2 has a trigger's
//   resistance (L2 a lighter one while it's held to lock on) and kicks back with each shot.
// - Rumble: a pulse with each of the player's shots and hits, over the game's own.
//
// The player's character, vehicle and health are read as the PC recomp's cheats read them
// (saulob/crackdown-xbox360-recompiled, src/cheat_hooks.cpp, guest_layout.h's TU6 layout).

#include "dualsense_ps5.h"

#include <SDL3/SDL_timer.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>

#include <rex/hook.h>
#include <rex/input/sony_feedback.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "guest_hooks.h"
#include "guest_memory.h"

namespace {

namespace sony = rex::input::sony;
using crackdown::guest::Load32;
using crackdown::guest::LoadFloat;
using crackdown::guest::Plausible;
using crackdown::hooks::GuestFunction;

constexpr uint32_t kPlayerController = 0x82E7B3DC;  // the local player's controller
constexpr uint32_t kCoopSession = 0x82E7B344;       // non-zero in a co-op session
constexpr uint32_t kCharacterInController = 292;    // the controlled character + 32
constexpr uint32_t kCharacterBase = 32;
constexpr uint32_t kHealth = 400, kHealthScale = 404, kHealthMultiplier = 408;  // floats
constexpr uint32_t kVehicle = 3588;
constexpr uint32_t kVehicleFlags = 3473;
constexpr uint8_t kDrivingFlags = 0x18;
constexpr uint32_t kPrimaryEquipment = 4304, kSecondaryEquipment = 4308;
constexpr uint32_t kWeaponOwner = 160;
constexpr uint32_t kThrownWeaponVtables[] = {0x820A0530, 0x8209C2B0, 0x8209C988, 0x8209CFE8};

constexpr uint32_t kUser = 0;
constexpr std::array<uint8_t, 3> kAgencyGreen{40, 255, 90}, kHitRed{255, 24, 24}, kDimRed{90, 0, 0};

std::atomic<uint64_t> g_hit_ms{0};

uint32_t PlayerCharacter(uint8_t* base) {
  const uint32_t controller = Load32(base, kPlayerController);
  if (!Plausible(controller)) return 0;
  const uint32_t sub = Load32(base, controller + kCharacterInController);
  return Plausible(sub) ? sub - kCharacterBase : 0;
}

float HealthFraction(uint8_t* base, uint32_t character) {
  const float full = LoadFloat(base, character + kHealthScale) * LoadFloat(base, character + kHealthMultiplier);
  const float now = LoadFloat(base, character + kHealth);
  return std::isfinite(full) && std::isfinite(now) && full > 0 ? std::clamp(now / full, 0.0f, 1.0f) : 1.0f;
}

// A gun the character holds (a thrown weapon - grenades - isn't one).
bool HoldsGun(uint8_t* base, uint32_t character) {
  for (const uint32_t slot : {kPrimaryEquipment, kSecondaryEquipment}) {
    const uint32_t weapon = Load32(base, character + slot);
    if (!Plausible(weapon) || Load32(base, weapon + kWeaponOwner) != character) continue;
    const uint32_t vtable = Load32(base, weapon);
    if (std::find(std::begin(kThrownWeaponVtables), std::end(kThrownWeaponVtables), vtable) ==
        std::end(kThrownWeaponVtables))
      return true;
  }
  return false;
}

void Emit(sony::Event event, float strength) {
  const sony::Device device = sony::GetService().ReadDevice(kUser);
  if (device.model == sony::Model::kNone) return;
  sony::GetService().Emit(kUser, device.generation, event, std::clamp(strength, 0.0f, 1.0f), SDL_GetTicks());
}

// The character's damage entry: r3 the character, r4 the damage.
void DamageEntry(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t character = ctx.r3.u32;
  const bool player = character && character == PlayerCharacter(base);
  const float before = player ? HealthFraction(base, character) : 0.0f;
  crackdown::hooks::DamageEntry(ctx, base, original);  // (the cheats': infinite health, no fall damage)
  if (!player) return;
  const float lost = before - HealthFraction(base, character);
  if (lost <= 0.0f) return;
  g_hit_ms.store(SDL_GetTicks(), std::memory_order_relaxed);
  Emit(sony::Event::kDamage, 0.35f + lost * 4.0f);
}

// The two weapon fire paths that spend a round: r3 the weapon.
void FireWeapon(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t weapon = ctx.r3.u32;
  const uint32_t character = PlayerCharacter(base);
  const bool player = character && Plausible(weapon) && Load32(base, weapon + kWeaponOwner) == character;
  crackdown::hooks::FireWeaponTracked(ctx, base, original);  // (the cheats': infinite ammo)
  if (player) Emit(sony::Event::kShot, 0.8f);
}

}  // namespace

void crackdown::DualSenseFrame(uint8_t* base) {
  const uint64_t now = SDL_GetTicks();
  const sony::Device device = sony::GetService().ReadDevice(kUser);
  if (device.model == sony::Model::kNone) return;
  sony::GameState state;
  const uint32_t character = Load32(base, kCoopSession) == 0 ? PlayerCharacter(base) : 0;
  if (character) {
    state.active = true;
    const uint8_t flags = crackdown::guest::Load8(base, character + kVehicleFlags);
    const uint32_t vehicle = Load32(base, character + kVehicle);
    state.in_vehicle = Plausible(vehicle);
    state.driver = state.in_vehicle && (flags & kDrivingFlags) == kDrivingFlags;
    if (state.driver) {
      state.trigger_context = sony::TriggerContext::kVehicle;
    } else if (!state.in_vehicle && HoldsGun(base, character)) {
      state.trigger_context = sony::TriggerContext::kWeapon;
      state.can_fire = true;
      state.aiming = device.left_trigger > 40;
    }
    const float health = HealthFraction(base, character);
    state.has_color = true;
    if (now - g_hit_ms.load(std::memory_order_relaxed) < 220) {
      state.color = kHitRed;
    } else if (health < 0.3f) {
      state.color = (now / 400) % 2 ? kHitRed : kDimRed;
    } else {
      state.color = kAgencyGreen;
    }
    state.player_identity = character;
    state.context_identity = state.in_vehicle ? vehicle : 0;
  }
  sony::GetService().Publish(kUser, device.generation, state, now);
}

#define CD_BIND(address, handler)   \
  REX_EXTERN(__imp__sub_##address); \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }

CD_BIND(822CC508, DamageEntry)
CD_BIND(82390340, FireWeapon)
CD_BIND(82394040, FireWeapon)
