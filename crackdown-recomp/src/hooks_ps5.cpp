// The PC recomp's codegen hooks that guard against crashes, ported as they are
// (saulob/crackdown-xbox360-recompiled src/, Title Update 6 addresses in
// config/overrides.toml). The rest are PC tweaks (frame pacing, mouse look,
// quit menu, ...): ci/filter_overrides.py leaves them out of codegen, so the
// original code runs there.

#include <atomic>

#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "audio_pcm_firewall.h"
#include "guest_memory.h"
#include "massive_server_guard.h"
#include "visibility_grid_guard.h"

namespace {
uint8_t* GuestBase() { return rex::system::kernel_state()->memory()->virtual_membase(); }
}  // namespace

// Before the load of the count XamEnumerate returned: r1+100 holds it.
void crackdown_massive_server_count_guard(PPCRegister& stack) {
  uint8_t* base = GuestBase();
  const uint32_t address = stack.u32 + 100;
  const uint32_t count = crackdown::guest::Load32(base, address);
  const uint32_t safe_count = crackdown::ClampMassiveServerCount(count);
  if (safe_count == count) return;
  crackdown::guest::Store32(base, address, safe_count);
  static std::atomic_flag logged;
  if (!logged.test_and_set())
    REXLOG_WARN("[XAM_GUARD] Clamped enumeration count from 0x{:08X} to {}", count, safe_count);
}

// Before the first column is shifted into a grid byte offset.
void crackdown_visibility_grid_guard(PPCRegister& first_column, PPCRegister& last_column, PPCRegister& first_row,
                                     PPCRegister& last_row) {
  const crackdown::VisibilityGridSpan span{static_cast<int32_t>(first_row.u32), static_cast<int32_t>(last_row.u32),
                                           static_cast<int32_t>(first_column.u32),
                                           static_cast<int32_t>(last_column.u32)};
  if (crackdown::VisibilityGridSpanNearTable(span)) return;
  const crackdown::VisibilityGridSpan safe = crackdown::ClampVisibilityGridSpan(span);
  first_row.u64 = static_cast<uint32_t>(safe.first_row);
  last_row.u64 = static_cast<uint32_t>(safe.last_row);
  first_column.u64 = static_cast<uint32_t>(safe.first_column);
  last_column.u64 = static_cast<uint32_t>(safe.last_column);
  static std::atomic_flag logged;
  if (!logged.test_and_set())
    REXLOG_WARN("[GRID_GUARD] Clamped audio grid rows {}..{} columns {}..{}", span.first_row, span.last_row,
                span.first_column, span.last_column);
}

// At the spin of the XAudio rendezvous: lets the audio workers leave when the title is terminated.
bool crackdown_audio_barrier_escape() {
  const auto* kernel = rex::system::kernel_state();
  return kernel && kernel->is_terminating_title();
}

// Before XAudioSubmitRenderDriverFrame: NaN/inf/out-of-range samples become silence or are clamped.
void crackdown_audio_pcm_firewall(PPCRegister& samples) {
  uint8_t* frame = rex::memory::GuestPtr<uint8_t*>(GuestBase(), samples.u32);
  crackdown::PcmFirewallCounts counts;
  crackdown::SanitizeBigEndianPcm(frame, crackdown::kPcmFirewallSamples, counts);
}
