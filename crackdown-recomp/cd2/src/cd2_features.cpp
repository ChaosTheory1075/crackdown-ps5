// Crackdown 2 (Title Update 5) - the menu's settings that need the game: SKIP INTRO, LANGUAGE and the
// DualSense light bar. Crackdown 1's are built from its own Title Update 6 addresses (skip_intro_ps5.cpp,
// locale_ps5.cpp, dualsense_ps5.cpp) and aren't in this program.
//
// SKIP INTRO. The start-up splash sequence is a state machine (its update: sub_828AB9F0, called through
// its vtable), its step at +0x144:
//   0  the MGS logo (MGS.usm)          ends when the movie does, or the skip check says so
//   1  the Ruffian logo (Ruffian.usm)  likewise
//   2  the autosave warning            at least 4.5 s (from the time at +0x148), and no button held
//   3  the title's fade-up             (TitleFadeUp.usm) likewise, then the title screen
// The skip check, sub_828B60C8, reads the pads' "pressed" bits; two other places use it too. While the
// option is on, it answers yes inside the splash update only (steps 0, 1 and 3), and step 2's start time
// is moved back by its 4.5 s. No controller state is changed: nothing reaches the menus after it.
//
// LANGUAGE. crackdown2_locale (the menu's folder, "cs-CZ" ...) is read by the framework's NtCreateFile
// (ci/fw_perf_fixes.py): a file in game:\Local\<xx-XX>\ or update:\Local\<xx-XX>\ is read from that
// folder when it has it.
//
// DUALSENSE FEATURES. The light bar in Crackdown 2's blue while it's on (the runtime's light bar is the
// controller's own while it's off).
#include <rex/platform.h>

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "cd2_cheats.h"

#if REX_PLATFORM_PS5
#include <SDL3/SDL_timer.h>
#include <rex/input/sony_feedback.h>
#endif

REXCVAR_DEFINE_BOOL(crackdown_skip_intro, true, "Crackdown", "Start at the title screen, past the logos");
REXCVAR_DEFINE_STRING(crackdown2_locale, "", "Crackdown", "Crackdown 2's text folder (game:\\Local\\<xx-XX>), empty: the game's own");

REX_EXTERN(__imp__sub_828AB9F0);  // the splash sequence's update
REX_EXTERN(__imp__sub_828B60C8);  // the skip check (a pad's "pressed" bits)
REX_EXTERN(__imp__sub_82954E48);  // the time in ms
REX_EXTERN(__imp__sub_825D3870);  // the scene's render targets (once): 2X MSAA colour + depth, one tile high
REX_EXTERN(__imp__sub_8291B400);  // D3D: a render target (r3 width, r4 height, r5 format, r6 multisample)

// No 2X MSAA, no predicated tiling (2026-11-11, the user's yes; SMAA from the menu smooths the edges). The scene is
// 1280x720 with 2X MSAA, colour + depth 14.7 MB, more than the 10 MB of EDRAM, so the game draws it in two tiles
// (count table 0x820EE97C = 2, rects at 0x830A05CC: 0-484 and 480-720 rows) and every object in both: 94% of a
// city frame's 11-14k draws were predicated (crackdown2_024). Without MSAA the scene is 7.4 MB and fits in one: one
// tile covering 1280x720 and the scene targets made without MSAA. The game's own bin select (sub_82261E60) stays:
// it selects tile 0 (80000003), so the tile-1 sections of its per-object command snippets don't run. On by default; cd2_no_msaa = false in crackdown2.toml gives the game's own 2X MSAA and two tiles back.
REXCVAR_DEFINE_BOOL(cd2_no_msaa, true, "Crackdown",
                    "Crackdown 2 without its 2X MSAA, drawn in one tile instead of two (about half the draws)");

namespace {

constexpr uint32_t kStep = 0x144, kWarningStart = 0x148, kWarningMs = 0x1194;
thread_local bool g_in_splash = false;
std::atomic<int> g_logged{0};

uint32_t Load32(uint8_t* base, uint32_t address) { return rex::memory::load_and_swap<uint32_t>(base + address); }
void Store32(uint8_t* base, uint32_t address, uint32_t value) { rex::memory::store_and_swap<uint32_t>(base + address, value); }

}  // namespace

void Cd2OneTileEarly(uint8_t* base);  // below: the tile tables, before anything is drawn

REX_HOOK_RAW(sub_828AB9F0) {
  Cd2OneTileEarly(base);
  if (!REXCVAR_GET(crackdown_skip_intro)) {
    __imp__sub_828AB9F0(ctx, base);
    return;
  }
  const uint32_t self = ctx.r3.u32;
  const uint32_t step = Load32(base, self + kStep);
  if (step == 2) {
    // the autosave warning: its time already up
    const uint64_t r3 = ctx.r3.u64, lr = ctx.lr;
    __imp__sub_82954E48(ctx, base);
    const uint32_t now = ctx.r3.u32;
    ctx.r3.u64 = r3, ctx.lr = lr;
    Store32(base, self + kWarningStart, now - kWarningMs - 1);
  }
  // (once per step: a step's update runs every frame, so a count of lines showed only step 0)
  if (step <= 3 && !(g_logged.fetch_or(1 << step) & (1 << step)))
    REXLOG_INFO("Crackdown 2 SKIP INTRO: splash step {} skipped (SKIP INTRO)", step);
  g_in_splash = step <= 3 && step != 2;
  __imp__sub_828AB9F0(ctx, base);
  g_in_splash = false;
}

REX_HOOK_RAW(sub_828B60C8) {
  if (g_in_splash) {
    ctx.r3.u64 = 1;
    return;
  }
  __imp__sub_828B60C8(ctx, base);
}

// FRAME RATE 60 (crackdown2_frame_rate, the menu's row: 30 by default, the game as shipped; 60 on the user's pick). Ported from
// pacific-city-recompiled/crackdown-2-recomp (BSD-3: crackdown2/src/hooks_framerate.cpp, hooks_frametime.cpp,
// host_tuning.cpp, config/midasm_hooks.toml; the same Title Update 5, 0.0.5.1), its Archipelago and overlay parts
// left out. Crackdown 2 steps its simulation a fixed 1/30 s a frame and waits two vblanks for it; at 60 the
// guest's vblanks run free (vsync off, crackdown2_app.cpp), the end of each frame is paced to 60 here, and the
// measured frame time goes to the game update, the step query, the 16 inlined `li rX,33` per-frame ms timers
// and 10 frame-locked steps (cd2_config.toml's [[midasm_hook]]s), so the game runs at its own speed at any rate.
REXCVAR_DEFINE_INT32(crackdown2_frame_rate, 30, "Crackdown",
                     "Crackdown 2: 30 (the shipped fixed step, default) or 60 (measured game clock, paced to 60)");

REX_EXTERN(__imp__sub_82385DA8);  // end of frame / present (the only caller of VdSwap)
REX_EXTERN(__imp__sub_82915C70);  // the game update, f1 = dt
REX_EXTERN(__imp__sub_827671D0);  // the step query, returns the step in f1
REX_EXTERN(__imp__sub_82208998);  // the character's step-down ground snap

namespace {
using FrameClock = std::chrono::steady_clock;
constexpr uint32_t kLockedFrameMs = 33;
constexpr double kMinStep = 1.0 / 240.0, kMaxStep = 1.0 / 20.0;

bool Sixty() {
  static const bool on = [] {
    const bool v = REXCVAR_GET(crackdown2_frame_rate) == 60;
    REXLOG_INFO("Crackdown 2 frame rate: {}", v ? "60 (measured game clock)" : "30 (the shipped fixed step)");
    return v;
  }();
  return on;
}

FrameClock::time_point g_last_frame{};
double g_ms_carry = 0.0, g_counter_carry = 0.0;
std::atomic<uint32_t> g_frame_ms{kLockedFrameMs};
std::atomic<float> g_step_scale{1.0f};  // measured step / (1/30)
float StepScale() { return g_step_scale.load(std::memory_order_relaxed); }

// The game has no limiter of its own once its vblanks run free: sleep to ~1.5 ms before the deadline, then
// yield (the PC recomp's PaceFrame). A frame that ran long resets the deadline (no catch-up).
void PaceFrame() {
  const FrameClock::duration period =
      std::chrono::duration_cast<FrameClock::duration>(std::chrono::duration<double>(1.0 / 60.0));
  static FrameClock::time_point deadline{};
  FrameClock::time_point now = FrameClock::now();
  if (now >= deadline) {
    deadline = now + period;
    return;
  }
  while (deadline - now > std::chrono::microseconds(1500)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    now = FrameClock::now();
  }
  while (now < deadline) {
    std::this_thread::yield();
    now = FrameClock::now();
  }
  deadline += period;
}

void UpdateSimClock() {
  const FrameClock::time_point now = FrameClock::now();
  if (g_last_frame != FrameClock::time_point{}) {
    const double dt = std::clamp(std::chrono::duration<double>(now - g_last_frame).count(), kMinStep, kMaxStep);
    g_ms_carry += dt * 1000.0;
    const uint32_t ms = uint32_t(std::floor(g_ms_carry));
    g_ms_carry -= ms;
    g_frame_ms.store(ms, std::memory_order_relaxed);
    g_step_scale.store(float(ms) / 1000.0f * 30.0f, std::memory_order_relaxed);
  }
  g_last_frame = now;
}
}  // namespace

REX_HOOK_RAW(sub_82385DA8) {
  if (Sixty()) {
    PaceFrame();
    UpdateSimClock();
  }
  cd2::cheats::Tick(base);  // (the quick menu's cheats, once a frame on the game's thread: cd2_cheats.h)
  __imp__sub_82385DA8(ctx, base);
}

REX_HOOK_RAW(sub_82915C70) {
  if (Sixty()) ctx.f1.f64 = double(float(ctx.f1.f64) * StepScale());
  __imp__sub_82915C70(ctx, base);
}

REX_HOOK_RAW(sub_827671D0) {
  __imp__sub_827671D0(ctx, base);
  if (Sixty()) ctx.f1.f64 = double(float(ctx.f1.f64) * StepScale());
}

// The stairs: out.y = -drop * gain / dt (a fixed step height), so at 60 the pull doubles and Havok turns part
// of it into forward speed on stair edges; scaled by dt * 30 to the 30 fps velocity.
REX_HOOK_RAW(sub_82208998) {
  const uint32_t out = ctx.r5.u32;
  const float dt = float(ctx.f1.f64);
  const uint32_t before = Load32(base, out + 4);
  __imp__sub_82208998(ctx, base);
  const uint32_t after = Load32(base, out + 4);
  if (after == before || !Sixty()) return;
  if (dt > 0.0f && dt < 1.0f / 32.0f) Store32(base, out + 4, std::bit_cast<uint32_t>(std::bit_cast<float>(after) * dt * 30.0f));
}

namespace {
bool g_scene_targets = false;  // inside sub_825D3870
bool g_one_tile_set = false;

// The tile tables are header statics: the frame's own (count 0x820EE97C, rects 0x830A05CC) and a copy in every
// translation unit that includes them. The game's own per-tile loops (0x8278xxxx, 0x827Axxxx: quarter-res passes,
// SET_BIN_MASK 3 << 2i) read the copies, so all of them become one tile: count 1, tile 0 = (0,0)-(1280,720).
// Canary proof with the same values in a patch file: Extras/CD2-NO-MSAA.md.
constexpr uint32_t kTileRects[] = {
    0x830911E0, 0x830913A8, 0x83091CD0, 0x83092110, 0x83095720, 0x83095BAC, 0x83095CD0, 0x83095CF8, 0x83095D70,
    0x83095E28, 0x830960E8, 0x83096184, 0x83096240, 0x830962B8, 0x830962E4, 0x83096314, 0x83096380, 0x830963F0,
    0x83096438, 0x83096464, 0x830964F8, 0x83096534, 0x83097A90, 0x83097ABC, 0x83097B00, 0x83097F50, 0x83099658,
    0x8309CD90, 0x8309CFA4, 0x8309D388, 0x8309D3F8, 0x830A0508, 0x830A05CC, 0x830A0660, 0x830A0764};
constexpr uint32_t kTileCounts[] = {0x820EE97C, 0x820E692C, 0x820E6980, 0x820E6AD8};

// The tile counts (0x820E....) are in the XEX's read-only data, which the loader maps read-only: a plain store
// faulted on the console (crackdown2_026, 2026-11-13; Canary's patch file writes them before protection).
// Written as Saints Row 2's guest_write does (sr_app.cpp): page made writable, value stored, protection restored.
bool StoreGuarded(uint8_t* base, uint32_t address, uint32_t value) {
  auto* kernel = rex::system::kernel_state();
  auto* memory = kernel ? kernel->memory() : nullptr;
  auto* heap = memory ? memory->LookupHeap(address) : nullptr;
  uint32_t old_protect = 0;
  if (!heap || !heap->QueryProtect(address, &old_protect)) return false;
  if (!heap->Protect(address, 4, rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite)) return false;
  Store32(base, address, value);
  heap->Protect(address, 4, old_protect);
  return true;
}

void OneTile(uint8_t* base) {
  if (g_one_tile_set || !REXCVAR_GET(cd2_no_msaa)) return;
  g_one_tile_set = true;
  int rects = 0, counts = 0;
  for (uint32_t r : kTileRects) {
    // only a table that is still {0,0,1280,484, 0,480,1280,720}
    if (Load32(base, r) == 0 && Load32(base, r + 4) == 0 && Load32(base, r + 8) == 1280 &&
        Load32(base, r + 12) == 484 && Load32(base, r + 20) == 480 && Load32(base, r + 28) == 720) {
      if (StoreGuarded(base, r + 12, 720)) ++rects;
    }
  }
  for (uint32_t c : kTileCounts) {
    if (Load32(base, c) == 2) {
      if (StoreGuarded(base, c, 1)) ++counts;
    }
  }
  REXLOG_INFO("Crackdown 2: one tile (no 2X MSAA): {} of {} tile rect tables, {} of {} tile counts", rects,
              sizeof(kTileRects) / sizeof(kTileRects[0]), counts, sizeof(kTileCounts) / sizeof(kTileCounts[0]));
}
}  // namespace

void Cd2OneTileEarly(uint8_t* base) { OneTile(base); }

REX_HOOK_RAW(sub_825D3870) {
  OneTile(base);
  g_scene_targets = REXCVAR_GET(cd2_no_msaa);
  __imp__sub_825D3870(ctx, base);
  g_scene_targets = false;
}

REX_HOOK_RAW(sub_8291B400) {
  if (g_scene_targets && ctx.r6.u32 == 1) {
    REXLOG_INFO("Crackdown 2: scene target {}x{} format {:08X} made without MSAA (one tile)", ctx.r3.u32,
                ctx.r4.u32, ctx.r5.u32);
    ctx.r6.u64 = 0;
  }
  __imp__sub_8291B400(ctx, base);
}


// The 16 per-frame ms sites (cd2_config.toml): the measured frame ms in place of the 33 just loaded.
#define CD2_FRAME_MS(addr) \
  void Cd2FrameMs_##addr(PPCRegister& r) { if (Sixty()) r.u64 = g_frame_ms.load(std::memory_order_relaxed); }
CD2_FRAME_MS(823AB418) CD2_FRAME_MS(823AB474) CD2_FRAME_MS(823914B0) CD2_FRAME_MS(8252D34C)
CD2_FRAME_MS(8276226C) CD2_FRAME_MS(827EAA24) CD2_FRAME_MS(82381634) CD2_FRAME_MS(82240290)
CD2_FRAME_MS(8233869C) CD2_FRAME_MS(8220C0DC) CD2_FRAME_MS(8278FC88) CD2_FRAME_MS(827837FC)
CD2_FRAME_MS(823397F0) CD2_FRAME_MS(82250438) CD2_FRAME_MS(827AEAE0) CD2_FRAME_MS(827671A4)
#undef CD2_FRAME_MS

// A literal 30 Hz step (1/30 or 0.033) just loaded: crowd animation, timers, dt passed to three updates.
#define CD2_SCALE_STEP(addr) \
  void Cd2ScaleStep_##addr(PPCRegister& f) { if (Sixty()) f.f64 = double(float(f.f64) * StepScale()); }
CD2_SCALE_STEP(825E4CBC) CD2_SCALE_STEP(825E4CEC) CD2_SCALE_STEP(825E4F80) CD2_SCALE_STEP(825E4FB0)
CD2_SCALE_STEP(82733908) CD2_SCALE_STEP(826C9618) CD2_SCALE_STEP(82549968) CD2_SCALE_STEP(8256DFC0)
#undef CD2_SCALE_STEP

// x -= x * 1/30 once a frame: (1 - 1/30)^(step * 30), the same decay per second as at 30.
void Cd2ScaleDecay_82334B2C(PPCRegister& f) {
  if (Sixty()) f.f64 = double(1.0f - std::pow(1.0f - float(f.f64), StepScale()));
}

// The 30 Hz frame counter + 1: + the whole 30 Hz ticks elapsed (fraction carried), so frame-count deadlines
// (speech, respawn, UI) last as long as at 30.
void Cd2FrameCounterTick(PPCRegister& r) {
  if (!Sixty()) return;
  g_counter_carry += StepScale();
  const uint32_t ticks = uint32_t(std::floor(g_counter_carry));
  g_counter_carry -= ticks;
  r.u64 = uint32_t(r.u32 - 1u + ticks);
}

namespace cd2 {

// The light bar, published ten times a second (the runtime drops a game's state that isn't renewed).
void StartLightBar() {
#if REX_PLATFORM_PS5
  std::thread([] {
    namespace sony = rex::input::sony;
    for (;;) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
      const sony::Device device = sony::GetService().ReadDevice(0);
      if (device.model == sony::Model::kNone) continue;
      sony::GameState state;
      state.active = true;
      state.has_color = true;
      state.color = {70, 165, 255};  // (Crackdown 2's blue, the menu's accent)
      sony::GetService().Publish(0, device.generation, state, SDL_GetTicks());
    }
  }).detach();
#endif
}

}  // namespace cd2
