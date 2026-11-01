// The PC recomp's frame pacing (saulob/crackdown-xbox360-recompiled src/frame_pacing.cpp,
// character_step_fixes.cpp, vehicle_action_step.cpp, audio_wave_timeout.cpp, nan_guards.cpp), ported for the PS5 with
// the Title Update 6 addresses of its src/guest_layout.h and src/guest_bindings_tu6.cpp.
//
// At 60 (crackdown_frame_rate, always on the PS5) the title runs its variable-step mode with a
// step of the measured frame, presenting every vblank. At 30 it keeps the shipped fixed step, but
// a late frame advances the game by the time it really took (up to three steps) instead of
// playing in slow motion. The codegen hooks below keep the character controller, ragdolls,
// vehicles and camera right at steps shorter than 1/30 s; the PS5 build's codegen gets them
// through ci/filter_overrides.py like the other ported hooks. frame_timing.h,
// character_step_fixes.h, vehicle_action_step.h and audio_wave_timeout.h are the PC recomp's,
// copied in by CI.

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "audio_wave_timeout.h"
#include <rex/platform.h>

#include "character_step_fixes.h"
#include "cheat_hooks.h"
#include "dualsense_ps5.h"
#include "frame_timing.h"
#include "guest_memory.h"
#include "vehicle_action_step.h"

namespace crackdown {
void KeepCoreLayout();  // core_layout_ps5.cpp
}  // namespace crackdown

REXCVAR_DEFINE_INT32(crackdown_frame_rate, 60, "Crackdown",
                     "30 (the shipped fixed step) or 60 (the title's variable-step mode)");

namespace {

using GuestFunction = void (*)(PPCContext&, uint8_t*);
using crackdown::FrameStep;
using crackdown::guest::Load32;
using crackdown::guest::LoadFloat;
using crackdown::guest::Store32;
using crackdown::guest::StoreFloat;

// Title Update 6 (the PC recomp's kLayoutTitleUpdate6).
constexpr uint32_t kTimeScale = 0x82CF6C98;
constexpr uint32_t kTimingObject = 0x82E79A14;
constexpr uint32_t kLimiterFrameRate = 0x82C3A9A4;
constexpr uint32_t kMeasuredFrameValid = 0x82E7B447;
constexpr uint32_t kMeasuredFrameMs = 0x82E2C268;
constexpr uint32_t kCharacterLookTarget = 3088;
constexpr uint32_t kCharacterRagdollFlags = 2436;
constexpr uint8_t kCharacterRagdollMask = 0x20;
constexpr uint32_t kHitMergeTicks = 5;
constexpr uint32_t kCharacterPositionOffset = 0x140;
constexpr std::array<uint32_t, 2> kControllerAim = {5760, 5728};
constexpr uint32_t kCameraBytes = 7184;
constexpr std::array<uint32_t, 8> kCameraFields = {272, 288, 304, 320, 6368, 6384, 6400, 6416};

constexpr uint32_t kOriginalPresentInterval = crackdown::PresentIntervalFor(crackdown::kOriginalFrameRate);
constexpr uint32_t kFixedStepOffset = 13;
constexpr uint32_t kSuspendedOffset = 14;
constexpr uint32_t kLogicalTickOffset = 16;
constexpr uint32_t kDevicePresentIntervalOffset = 13220;
constexpr uint32_t kSwapImmediateThresholdMask = 0xFF;
constexpr uint32_t kSwapImmediateAlways = 100;
constexpr uint32_t kVariableStepFrameMs = 16;

uint8_t* GuestBase() { return rex::system::kernel_state()->memory()->virtual_membase(); }

std::atomic<uint32_t> g_step_bits{std::bit_cast<uint32_t>(1.0f / 30.0f)};
std::atomic<uint32_t> g_frame_ms{kVariableStepFrameMs};
std::atomic<double> g_original_steps{1.0};

// Only the main thread runs frames.
FrameStep g_step = crackdown::FixedFrameStep(crackdown::kOriginalFrameRate);
crackdown::CompensatedStepClock g_clock;
int g_clock_rate = 0;
int64_t g_last_ns = 0;
double g_logical_tick_fraction = 0.0;
double g_frame_ms_fraction = 0.0;
int g_logged_rate = -1;
crackdown::VehicleActionClock g_vehicle_clock;
crackdown::WaveTimeoutClock g_wave_clock;

// 30 or 60: both present at the guest vblank.
int FrameRate() { return REXCVAR_GET(crackdown_frame_rate) == 30 ? 30 : 60; }
bool HighFrameRate(int rate) { return rate != crackdown::kOriginalFrameRate; }

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The real interval since the last frame, compensated (a late frame takes up to three steps).
void BeginFrame(int rate) {
  const int64_t now = NowNs();
  const double interval = g_last_ns ? double(now - g_last_ns) * 1e-9 : 0.0;
  g_last_ns = now;
  if (rate != g_clock_rate) {
    g_clock_rate = rate;
    g_clock.Reset();
    g_step = crackdown::FixedFrameStep(rate);
  } else {
    g_step = crackdown::StepFromSeconds(g_clock.Take(interval, 1.0 / rate).seconds);
  }
  g_frame_ms_fraction += g_step.seconds * 1000.0;
  g_frame_ms.store(crackdown::TakeFrameMs(g_frame_ms_fraction), std::memory_order_relaxed);
  g_step_bits.store(std::bit_cast<uint32_t>(g_step.seconds_f), std::memory_order_relaxed);
  g_original_steps.store(g_step.original_steps, std::memory_order_relaxed);
}

// Every new timing object starts in fixed-step mode.
void SelectVariableStep(uint8_t* base, int rate) {
  if (const uint32_t timing = Load32(base, kTimingObject)) {
    crackdown::guest::Store8(base, timing + kFixedStepOffset, 0);
  }
  const uint32_t limiter = crackdown::LimiterFrameRateFor(rate);
  if (Load32(base, kLimiterFrameRate) != limiter) Store32(base, kLimiterFrameRate, limiter);
}

// +16 counts the title's 1/30 s steps; tops the original's count up to the compensated step.
void SyncLogicalTick(uint8_t* base, uint32_t timing, uint32_t applied) {
  g_logical_tick_fraction = std::max(0.0, g_logical_tick_fraction + g_step.original_steps - double(applied));
  if (const uint32_t whole = crackdown::TakeWhole(g_logical_tick_fraction)) {
    Store32(base, timing + kLogicalTickOffset, Load32(base, timing + kLogicalTickOffset) + whole);
  }
}

uint32_t TitlePresentInterval(uint32_t interval) {
  return HighFrameRate(FrameRate()) && interval == kOriginalPresentInterval ? 1 : interval;
}

double OriginalSteps() { return g_original_steps.load(std::memory_order_relaxed); }

// The title leaves the w lane as garbage; only x, y and z count.
bool XyzFinite(uint8_t* base, uint32_t address) {
  for (uint32_t i = 0; i < 3; ++i) {
    if (!std::isfinite(LoadFloat(base, address + 4 * i))) return false;
  }
  return true;
}

void NoteRepair(const char* what) {
  static std::atomic<int64_t> last_ms{0};
  const int64_t now = NowNs() / 1'000'000;
  int64_t last = last_ms.load(std::memory_order_relaxed);
  if (now - last >= 2000 && last_ms.compare_exchange_strong(last, now)) {
    REXLOG_WARN("[NAN_GUARD] {} was not finite", what);
  }
}

float g_listener_matrix[4][3];
float g_listener_vector[3];
bool g_listener_valid = false;
float g_last_target[3];
bool g_target_valid = false;

// ---- the guest functions taken over (TU6 addresses) ----

// One main-loop frame.
void FrameLoop(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  crackdown::KeepCoreLayout();
  const int rate = FrameRate();
  if (rate != g_logged_rate) {
    g_logged_rate = rate;
    REXLOG_INFO("Frame rate: {} FPS ({} vblank(s) a frame{})", rate, crackdown::PresentIntervalFor(rate),
                HighFrameRate(rate) ? ", variable step" : "");
  }
  BeginFrame(rate);
  if (HighFrameRate(rate)) SelectVariableStep(base, rate);
#if REX_PLATFORM_PS5
  crackdown::DualSenseFrame(base);
  crackdown::cheats::RunGameTick(base);  // (a cheat just turned on refills health or ammo)
#endif
  original(ctx, base);
}

// The simulation update: its caller passes 1/30 s times the time scale.
void SimulationUpdate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  ctx.f1.f64 = double(float(ctx.f1.f64 * g_step.original_steps));
  original(ctx, base);
}

// The frame delta getter (replaced): time scale * the frame's step.
void FrameDelta(PPCContext& ctx, uint8_t* base) {
  ctx.f1.f64 = double(LoadFloat(base, kTimeScale) * std::bit_cast<float>(g_step_bits.load(std::memory_order_relaxed)));
}

void TimingUpdate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t timing = Load32(base, kTimingObject);
  const bool active = timing && !crackdown::guest::Load8(base, timing + kSuspendedOffset);
  const uint32_t before = active ? Load32(base, timing + kLogicalTickOffset) : 0;
  original(ctx, base);
  if (active) SyncLogicalTick(base, timing, Load32(base, timing + kLogicalTickOffset) - before);
}

// With the fixed step off the title publishes a short frame as 0 ms (a camera, player and audio
// listener NaN after a ragdoll); the port's own frame time instead, never 0.
void FrameTimer(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  original(ctx, base);
  if (HighFrameRate(FrameRate()) && crackdown::guest::Load8(base, kMeasuredFrameValid)) {
    Store32(base, kMeasuredFrameMs, g_frame_ms.load(std::memory_order_relaxed));
  }
}

// D3D present interval setter: r4 = the interval.
void PresentInterval(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  ctx.r4.u64 = TitlePresentInterval(ctx.r4.u32);
  original(ctx, base);
}

// The swap packet takes the device's interval (the one from device creation skips the setter).
void SwapPacket(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t field = ctx.r3.u32 + kDevicePresentIntervalOffset;
  const uint32_t interval = Load32(base, field);
  if (const uint32_t wanted = TitlePresentInterval(interval); wanted != interval) Store32(base, field, wanted);
  original(ctx, base);
}

// The swap callback: r3 = front buffer | interval << 8 | immediate threshold. A frame finished just
// after its vblank went out at once instead of a whole vblank later.
void SwapCallback(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (HighFrameRate(FrameRate())) ctx.r3.u64 = (ctx.r3.u32 & ~kSwapImmediateThresholdMask) | kSwapImmediateAlways;
  original(ctx, base);
}

// Havok vehicle action: runs once per 1/30 s of physics time (cars pulled away slower above 30).
void VehicleAction(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  constexpr uint32_t kDeltaTime = 8;
  constexpr uint32_t kInvDeltaTime = 12;
  const uint32_t info = ctx.r4.u32;
  const uint32_t dt_bits = Load32(base, info + kDeltaTime);
  const float dt = std::bit_cast<float>(dt_bits);
  const float step = g_vehicle_clock.Step(ctx.r3.u32, dt);
  if (step == 0.0f) return;
  if (step == dt) {
    original(ctx, base);
    return;
  }
  const uint32_t inv_bits = Load32(base, info + kInvDeltaTime);
  StoreFloat(base, info + kDeltaTime, step);
  StoreFloat(base, info + kInvDeltaTime, 1.0f / step);
  original(ctx, base);
  Store32(base, info + kDeltaTime, dt_bits);
  Store32(base, info + kInvDeltaTime, inv_bits);
}

// The player's aim update writes two aim points; a NaN one keeps the previous point.
void AimUpdate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  constexpr uint32_t kCharacterOffsetInController = 292;
  constexpr uint32_t kCharacterBase = 32;
  const uint32_t controller = ctx.r3.u32;
  uint8_t saved[2][16];
  for (int i = 0; i < 2; ++i) {
    std::memcpy(saved[i], rex::memory::GuestPtr<const uint8_t*>(base, controller + kControllerAim[i]), 16);
  }
  original(ctx, base);
  for (int i = 0; i < 2; ++i) {
    const uint32_t field = controller + kControllerAim[i];
    if (XyzFinite(base, field)) continue;
    NoteRepair("The player's aim point");
    bool saved_finite = true;
    for (int lane = 0; lane < 3; ++lane) {
      uint32_t raw = 0;
      std::memcpy(&raw, saved[i] + 4 * lane, 4);
      saved_finite = saved_finite && std::isfinite(std::bit_cast<float>(__builtin_bswap32(raw)));
    }
    if (saved_finite) {
      std::memcpy(rex::memory::GuestPtr<uint8_t*>(base, field), saved[i], 12);
      continue;
    }
    const uint32_t subobject = Load32(base, controller + kCharacterOffsetInController);
    const uint32_t character = crackdown::guest::Plausible(subobject) ? subobject - kCharacterBase : 0;
    for (uint32_t lane = 0; lane < 3; ++lane) {
      const float position = character ? LoadFloat(base, character + kCharacterPositionOffset + 4 * lane) : 0.0f;
      StoreFloat(base, field + 4 * lane, std::isfinite(position) ? position : 0.0f);
    }
  }
}

// The camera's update: a non-finite camera is put back to its state before the update.
void CameraUpdate(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t camera = ctx.r3.u32;
  const bool plausible = crackdown::guest::Plausible(camera);
  thread_local std::array<uint8_t, kCameraBytes> saved;
  if (plausible) std::memcpy(saved.data(), rex::memory::GuestPtr<const uint8_t*>(base, camera), kCameraBytes);
  original(ctx, base);
  if (!plausible) return;
  for (const uint32_t offset : kCameraFields) {
    if (!XyzFinite(base, camera + offset)) {
      NoteRepair("The camera");
      std::memcpy(rex::memory::GuestPtr<uint8_t*>(base, camera), saved.data(), kCameraBytes);
      return;
    }
  }
}

// Wave instance setup (r3 the pooled instance): a new clip starts its timeout afresh.
void WaveInstanceSetup(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  g_wave_clock.Reset(ctx.r3.u32);
  original(ctx, base);
}

}  // namespace

// The guest function sub_<address> runs the handler, its own code (__imp__sub_<address>) passed
// as `original`; a _REPLACE binding has no use for it.
#define CD_BIND(address, handler)                    \
  REX_EXTERN(__imp__sub_##address);                  \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }
#define CD_BIND_REPLACE(address, handler)            \
  REX_EXTERN(__imp__sub_##address);                  \
  [[gnu::used]] static const auto bound_##address = &__imp__sub_##address; \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base); }

CD_BIND(8272F7C0, FrameLoop)
CD_BIND(8272ED00, SimulationUpdate)
CD_BIND_REPLACE(825B7020, FrameDelta)
CD_BIND(82358AD0, TimingUpdate)
CD_BIND(8235A750, FrameTimer)
CD_BIND(8273A4E8, PresentInterval)
CD_BIND(82745900, SwapPacket)
CD_BIND(82745768, SwapCallback)
CD_BIND(824F4600, VehicleAction)
CD_BIND(82560740, AimUpdate)
CD_BIND(822AE1E0, CameraUpdate)
CD_BIND(8227F8A0, WaveInstanceSetup)

// ---- codegen hooks (config/tu6/overrides.toml) ----

// (The 16 ms of the variable-step sites stays: 60 presents at the vblank, as on the PC.)

// The 1/30 of the crowd animation and the 0.033 of a cosmetic timer.
void crackdown_crowd_anim_step(PPCRegister& step) {
  step.f64 = double(crackdown::ScalePerOriginalStep(float(step.f64), OriginalSteps()));
}

void crackdown_cosmetic_timer_step(PPCRegister& step) {
  step.f64 = double(crackdown::ScalePerOriginalStep(float(step.f64), OriginalSteps()));
}

void crackdown_ledge_snap_down(PPCRegister& height_pull) {
  height_pull.f64 = double(crackdown::ScaleLedgeSnapDown(float(height_pull.f64), OriginalSteps()));
}

void crackdown_ground_snap_decay(PPCRegister& decay) {
  decay.f64 = double(crackdown::ScaleGroundSnapDecay(float(decay.f64), OriginalSteps()));
}

void crackdown_ground_snap_lerp(PPCRegister& fraction) {
  fraction.f64 = double(crackdown::ScaleGroundSnapLerp(float(fraction.f64), OriginalSteps()));
}

void crackdown_hit_merge_window(PPCRegister& deadline) {
  const uint32_t ticks = crackdown::ScaleTickWindow(kHitMergeTicks, OriginalSteps());
  deadline.u64 = uint32_t(deadline.u32 - kHitMergeTicks + ticks);
}

void crackdown_ragdoll_tick_window(PPCRegister& deadline) {
  constexpr uint32_t kTicks = 15;
  const uint32_t ticks = crackdown::ScaleTickWindow(kTicks, OriginalSteps());
  deadline.u64 = uint32_t(deadline.u32 - kTicks + ticks);
}

void crackdown_first_gravity_step(PPCRegister& gravity_step, PPCRegister& accumulated) {
  gravity_step.f64 = double(
      crackdown::ScaleFirstGravityStep(float(gravity_step.f64), float(accumulated.f64), OriginalSteps()));
}

void crackdown_ragdoll_pose_dt(PPCRegister& dt, PPCRegister& character) {
  const bool ragdolled =
      (crackdown::guest::Load8(GuestBase(), character.u32 + kCharacterRagdollFlags) & kCharacterRagdollMask) != 0;
  dt.f64 = double(float(crackdown::ScaleRagdollPoseDt(dt.f64, ragdolled, OriginalSteps())));
}

void crackdown_projectile_probe_dt(PPCRegister& stack) {
  constexpr uint32_t kProbeDtOffset = 476;
  uint8_t* base = GuestBase();
  const uint32_t slot = stack.u32 + kProbeDtOffset;
  StoreFloat(base, slot, crackdown::ScaleProjectileProbeDt(LoadFloat(base, slot), OriginalSteps()));
}

// After the `addi r11,r11,1` of a wave instance's elapsed count (r30 the instance): its safety
// timeout counts 1/30 s steps, so speech no longer stops early above 30.
void crackdown_audio_wave_timeout_tick(PPCRegister& elapsed, PPCRegister& instance) {
  elapsed.u64 = elapsed.u32 - 1u + g_wave_clock.Tick(instance.u32, OriginalSteps());
}

void crackdown_camera_lift_divisor(PPCRegister& forward_y) {
  forward_y.f64 = double(crackdown::GuardCameraLiftDivisor(float(forward_y.f64)));
}

// Before the listener is submitted: r3 its 4x4 matrix, r4 its velocity.
void crackdown_audio_listener_guard(PPCRegister& matrix, PPCRegister& vector) {
  uint8_t* base = GuestBase();
  bool finite = XyzFinite(base, vector.u32);
  for (uint32_t row = 0; row < 4; ++row) finite = finite && XyzFinite(base, matrix.u32 + 16 * row);
  if (finite) {
    for (uint32_t row = 0; row < 4; ++row) {
      for (uint32_t i = 0; i < 3; ++i) g_listener_matrix[row][i] = LoadFloat(base, matrix.u32 + 16 * row + 4 * i);
    }
    for (uint32_t i = 0; i < 3; ++i) g_listener_vector[i] = LoadFloat(base, vector.u32 + 4 * i);
    g_listener_valid = true;
    return;
  }
  NoteRepair("The audio listener");
  for (uint32_t row = 0; row < 4; ++row) {
    for (uint32_t i = 0; i < 3; ++i) {
      StoreFloat(base, matrix.u32 + 16 * row + 4 * i,
                 g_listener_valid ? g_listener_matrix[row][i] : (row == i ? 1.0f : 0.0f));
    }
  }
  for (uint32_t i = 0; i < 3; ++i) StoreFloat(base, vector.u32 + 4 * i, g_listener_valid ? g_listener_vector[i] : 0.0f);
}

// Before the reads of the character's look-at target (r31): a NaN one becomes the last finite one.
void crackdown_look_target_guard(PPCRegister& character) {
  uint8_t* base = GuestBase();
  const uint32_t target = character.u32 + kCharacterLookTarget;
  if (XyzFinite(base, target)) {
    for (uint32_t i = 0; i < 3; ++i) g_last_target[i] = LoadFloat(base, target + 4 * i);
    g_target_valid = true;
    return;
  }
  NoteRepair("The character's look-at target");
  for (uint32_t i = 0; i < 3; ++i) {
    const float fallback = LoadFloat(base, character.u32 + kCharacterPositionOffset + 4 * i);
    StoreFloat(base, target + 4 * i, g_target_valid ? g_last_target[i] : (std::isfinite(fallback) ? fallback : 0.0f));
  }
}
