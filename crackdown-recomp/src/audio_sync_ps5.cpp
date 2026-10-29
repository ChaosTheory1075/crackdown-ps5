// The PC recomp's XAudio fixes (saulob/crackdown-xbox360-recompiled src/audio_guest_sync.cpp and
// title_compat.cpp), ported for the PS5 with their Title Update 6 bindings. The PS5 port had
// none of them: its streamed voices (the music) stopped while the sound effects played.
//
// - AudioBarrier (sub_82AD8EB8): the XAudio mix workers' rendezvous once per audio pass. The
//   title's version marks its processor's byte in a guest word and spins until all are in or the
//   word is reset, so a worker preempted in between misses the reset and spins forever with its
//   partner, and the voices those workers mix stop. Kept on the host here (xaudio_barrier.h).
// - AudioResetEvent (sub_821512C0): the job dispatcher's inline ResetEvent (`stw 0,4(r3)`) never
//   reaches the kernel, so the host event the runtime waits on stays signaled.
// - AudioReleaseHandle (sub_82AD2860): one audio slot teardown passes a null object; it takes the
//   no-release path instead.

#include <atomic>
#include <cstdint>
#include <thread>

#include <immintrin.h>

#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>

#include "guest_memory.h"
#include "xaudio_barrier.h"

bool crackdown_audio_barrier_escape();  // hooks_ps5.cpp

REX_EXTERN(__imp__KeResetEvent);

namespace {

using GuestFunction = void (*)(PPCContext&, uint8_t*);
using crackdown::guest::Load32;
using crackdown::guest::Load8;
using crackdown::guest::Store32;
using crackdown::guest::Store8;

constexpr uint32_t kEngineActiveOffset = 304;
constexpr uint32_t kEngineProcessorSlotsOffset = 308;
constexpr uint32_t kEngineProcessorSlots = 6;
constexpr uint32_t kPcrProcessorNumberOffset = 268;
constexpr uint32_t kSpinsBeforeYield = 4096;

// Guest KEVENT: type byte at +0, SignalState at +4; the runtime marks the ones it shadows with "REX\0" at +8.
constexpr uint32_t kShadowedObjectSignature = 0x52455800;
constexpr uint8_t kSynchronizationEventType = 1;

// The slot teardown that passes a null object returns here (Title Update 6).
constexpr uint32_t kNullAudioReturn = 0x82282388;

// XAudio has one engine with two barrier words that alternate per pass, so a few are enough.
struct BarrierSlot {
  std::atomic<uint32_t> word{0};
  crackdown::XAudioBarrier barrier;
};
BarrierSlot g_barriers[8];

crackdown::XAudioBarrier* BarrierFor(uint32_t word) {
  for (BarrierSlot& slot : g_barriers) {
    uint32_t current = slot.word.load(std::memory_order_acquire);
    if (current == 0 && slot.word.compare_exchange_strong(current, word, std::memory_order_acq_rel)) current = word;
    if (current == word) return &slot.barrier;
  }
  return nullptr;
}

uint64_t ExpectedProcessors(uint8_t* base, uint32_t engine) {
  uint64_t expected = 0;
  for (uint32_t i = 0; i < kEngineProcessorSlots; ++i)
    if (Load32(base, engine + kEngineProcessorSlotsOffset + 4 * i)) expected |= uint64_t(1) << i;
  return expected;
}

void WaitForRelease(const crackdown::XAudioBarrier& barrier, uint64_t round) {
  uint64_t spins = 0;
  while (barrier.RoundOpen(round)) {
    if (++spins % kSpinsBeforeYield == 0) {
      if (crackdown_audio_barrier_escape()) break;
      std::this_thread::yield();
    } else {
      _mm_pause();
    }
  }
}

// (engine, word); only this function uses the word.
void AudioBarrier(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  const uint32_t engine = ctx.r3.u32;
  if (!Load32(base, engine + kEngineActiveOffset)) return;
  const uint8_t processor = Load8(base, ctx.r13.u32 + kPcrProcessorNumberOffset);
  crackdown::XAudioBarrier* barrier = processor < 8 ? BarrierFor(ctx.r4.u32) : nullptr;
  if (!barrier) {
    original(ctx, base);
    return;
  }
  const crackdown::XAudioBarrier::Arrival arrival = barrier->Arrive(processor, ExpectedProcessors(base, engine));
  if (!arrival.released_round) WaitForRelease(*barrier, arrival.round);
}

// Identical-code folding also puts this body in C++ vtables, where r3 is not an event: only events
// the runtime already shadows are reset on the host too.
void AudioResetEvent(PPCContext& ctx, uint8_t* base) {
  const uint32_t object = ctx.r3.u32;
  ctx.r11.s64 = 0;
  Store32(base, object + 4, 0);
  if (Load8(base, object) > kSynchronizationEventType || Load32(base, object + 8) != kShadowedObjectSignature) return;
  __imp__KeResetEvent(ctx, base);
  ctx.r3.u64 = object;
  ctx.r11.s64 = 0;
}

// The slot teardown selects its no-release path through the out byte, which also skips the slot's
// own idle reset (offsets 64, 70-73), so the reset is done here. Other callers keep the original.
void AudioReleaseHandle(PPCContext& ctx, uint8_t* base, GuestFunction original) {
  if (!ctx.r3.u32 && uint32_t(ctx.lr) == kNullAudioReturn) {
    const uint32_t slot = ctx.r31.u32;
    Store8(base, ctx.r4.u32, 1);
    Store32(base, slot + 12, 0);
    Store8(base, slot + 64, 0);
    Store8(base, slot + 70, 0);
    Store8(base, slot + 71, 0);
    Store8(base, slot + 72, 0);
    Store8(base, slot + 73, 1);
    ctx.r3.u64 = 0;
    return;
  }
  original(ctx, base);
}

}  // namespace

#define CD_BIND(address, handler)   \
  REX_EXTERN(__imp__sub_##address); \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base, __imp__sub_##address); }
#define CD_BIND_REPLACE(address, handler)                                    \
  REX_EXTERN(__imp__sub_##address);                                          \
  [[gnu::used]] static const auto bound_##address = &__imp__sub_##address; \
  REX_HOOK_RAW(sub_##address) { handler(ctx, base); }

CD_BIND(82AD8EB8, AudioBarrier)
CD_BIND_REPLACE(821512C0, AudioResetEvent)
CD_BIND(82AD2860, AudioReleaseHandle)
