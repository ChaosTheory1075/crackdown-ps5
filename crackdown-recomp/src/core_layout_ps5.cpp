// The PS5 CPU layout for Crackdown. The game gets CPUs 0-12, a core's two hardware threads
// numbered side by side. crackdown_018.log: the GPU command thread was busy all the time and set
// the frame rate, the game's main thread spent half its time waiting on it, and every thread ran
// anywhere - so either could share a core with a busy thread and run at a fraction of its speed.
//
// - The GPU command thread: CPU 0 (ci/fw_perf_fixes.py pins it when it starts), CPU 1 empty.
// - The game's main thread: CPU 2, CPU 3 empty.
// - The GPU's second lane (gpu_lane.h in the framework): CPU 4 (it pins itself), CPU 5 empty.
// - Every other thread: CPUs 6-12.
//
// Applied from the main thread's frame loop: once at its first frame, then every 300 frames for
// threads started since (KeepOtherThreadsOn moves a thread only when its CPUs change).

#include <atomic>
#include <cstdint>

#include <pthread.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ps5_diag.h>

REXCVAR_DEFINE_BOOL(crackdown_core_layout, true, "Crackdown",
                    "Give the GPU command thread and the game's main thread a core each");

extern "C" int32_t scePthreadSetaffinity(pthread_t thread, uint64_t mask);
extern "C" int32_t sceKernelGetCurrentCpu(void);

namespace crackdown {
namespace {

constexpr uint64_t kGpuCpu = uint64_t(1) << 0;
constexpr uint64_t kMainCpu = uint64_t(1) << 2;
constexpr uint64_t kOtherCpus = 0x1FC0;  // CPUs 6-12
constexpr uint32_t kRecheckFrames = 300;

uint32_t g_frames = 0;
bool g_main_pinned = false;
bool g_logged = false;

}  // namespace

// Once a frame, on the game's main thread.
void KeepCoreLayout() {
  if (!REXCVAR_GET(crackdown_core_layout)) return;
  if (!g_main_pinned) {
    g_main_pinned = true;
    const int32_t result = scePthreadSetaffinity(pthread_self(), kMainCpu);
    REXLOG_INFO("Core layout: main thread on CPU 2 ({:#x}), GPU commands on CPU 0, its second lane on CPU 4, the rest on CPUs 6-12",
                uint32_t(result));
  }
  if (g_frames++ % kRecheckFrames) return;
  const uintptr_t gpu = rex::ps5diag::gpu_commands_thread.load(std::memory_order_acquire);
  rex::ps5diag::KeepOtherThreadsOn(kOtherCpus, gpu, reinterpret_cast<uintptr_t>(pthread_self()), kMainCpu);
  if (!g_logged && gpu) {
    g_logged = true;
    REXLOG_INFO("Core layout: applied (main thread now on CPU {})", sceKernelGetCurrentCpu());
  }
}

}  // namespace crackdown
