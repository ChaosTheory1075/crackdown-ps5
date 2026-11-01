// Skip the intro: straight to the title screen, past the logos and the opening movie (the menu's
// SKIP INTRO, crackdown_skip_intro). The PC recomp's skip_intro.cpp with Title Update 6's addresses
// (saulob/crackdown-xbox360-recompiled, guest_bindings_tu6.cpp and guest_layout.h):
//
// - the engine's own logo movies (sub_82730A70 with r3 0 or 1) aren't started;
// - a movie played through the video player (sub_82696DA0, r4 the name object) that comes before the
//   title (MSGS, the Realtime Worlds logo, the Crackdown logo, the opening) finishes at once: the
//   player's "videos enabled" byte is cleared for that call, its disabled branch ends the video;
// - the start sequence's splash states (6 to 8) are stepped past (sub_826CD158's frame callback calls
//   the sequence's advance, sub_826CD5E0, instead) until it reaches the title's start prompt.

#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "guest_memory.h"

REXCVAR_DEFINE_BOOL(crackdown_skip_intro, true, "Crackdown", "Start at the title screen, past the logos and the opening");

REX_EXTERN(__imp__sub_82363DE8);  // the video player singleton
REX_EXTERN(__imp__sub_826CD5E0);  // the start sequence's advance

namespace {

using crackdown::guest::Load32;
using crackdown::guest::Load8;
using GuestFunction = void (*)(PPCContext&, uint8_t*);

constexpr uint32_t kGuestLow = 0x10000, kGuestHigh = 0xFFFF0000;
constexpr uint32_t kMovieNameLimit = 128;
constexpr uint32_t kVideosEnabled = 152;   // in the video player singleton
constexpr uint32_t kSequenceObject = 136;  // the frame callback gets the sequence + this
constexpr uint32_t kSequenceState = 140;   // from the address the frame callback gets
constexpr uint32_t kSequenceDone = 24;
constexpr uint32_t kStartPrompt = 10, kSplashFirst = 6, kSplashLast = 8;
constexpr uint32_t kPresentationFlag = 0x82E7BA07;

std::atomic<bool> g_start_reached{false};

bool Guest(uint32_t address) { return address >= kGuestLow && address < kGuestHigh; }

// A guest string object: length at +20 in UTF-16 units, data at +4.
std::string MovieName(uint8_t* base, uint32_t name) {
  if (!Guest(name)) return {};
  const uint32_t data = Load32(base, name + 4), length = Load32(base, name + 20);
  if (!Guest(data) || !length || length > kMovieNameLimit) return {};
  std::string out;
  for (uint32_t i = 0; i < length; ++i) {
    const uint16_t unit = uint16_t(Load8(base, data + 2 * i) << 8 | Load8(base, data + 2 * i + 1));
    out += unit < 0x80 ? char(std::tolower(unit)) : '?';
  }
  return out;
}

bool BeforeTitle(uint8_t* base, uint32_t name) {
  const std::string movie = MovieName(base, name);
  for (std::string_view m : {"msgs.bik", "rtw_logo.bik", "crackdownhd.bik", "maingameintro.bik"})
    if (std::string_view(movie).ends_with(m)) return true;
  return false;
}

void EngineMovie(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (REXCVAR_GET(crackdown_skip_intro) && ctx.r3.u32 <= 1) return;
  original(ctx, base);
}

void PlayVideo(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (!REXCVAR_GET(crackdown_skip_intro) || !BeforeTitle(base, ctx.r4.u32)) {
    original(ctx, base);
    return;
  }
  const uint64_t r3 = ctx.r3.u64, r4 = ctx.r4.u64, r5 = ctx.r5.u64, r6 = ctx.r6.u64, r7 = ctx.r7.u64, lr = ctx.lr;
  __imp__sub_82363DE8(ctx, base);
  const uint32_t player = ctx.r3.u32;
  ctx.r3.u64 = r3, ctx.r4.u64 = r4, ctx.r5.u64 = r5, ctx.r6.u64 = r6, ctx.r7.u64 = r7, ctx.lr = lr;
  if (!Guest(player)) {
    original(ctx, base);
    return;
  }
  uint8_t* enabled = rex::memory::GuestPtr<uint8_t*>(base, player + kVideosEnabled);
  const uint8_t was = *enabled;
  *enabled = 0;
  original(ctx, base);
  *enabled = was;
}

void SequenceFrame(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (!REXCVAR_GET(crackdown_skip_intro) || g_start_reached.load(std::memory_order_relaxed)) {
    original(ctx, base);
    return;
  }
  const uint32_t frame = ctx.r3.u32;
  const uint32_t state = Load32(base, frame + kSequenceState);
  if (state == kStartPrompt && Load8(base, kPresentationFlag) == 0) {
    g_start_reached.store(true, std::memory_order_relaxed);
    REXLOG_INFO("Skip intro: at the title screen");
  }
  if (Load8(base, frame + kSequenceDone) || state < kSplashFirst || state > kSplashLast) {
    original(ctx, base);
    return;
  }
  ctx.r3.u64 = frame - kSequenceObject;
  __imp__sub_826CD5E0(ctx, base);
}

}  // namespace

#define CD_BIND(address, handler)   \
  REX_EXTERN(__imp__sub_##address); \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }

CD_BIND(82730A70, EngineMovie)
CD_BIND(82696DA0, PlayVideo)
CD_BIND(826CD158, SequenceFrame)
