// Crackdown: ReActivated (PS5) - an abort, an int3 or a floating-point trap gets the [ps5 crash] line
// (The Darkness: ReIncarnated's dark_diag.cpp). Only faults and (ci/fw_perf_fixes.py) SIGILL reached
// the crash report, so these ended a game with nothing in its log. The report's stack scan starts at
// this handler's own frame, so it shows the callers.
#include <rex/platform.h>
#if REX_PLATFORM_PS5

#include <signal.h>

#include <cstdint>
#include <cstdlib>

#include <rex/ps5_diag.h>

namespace cdps5 {
namespace {

void OnAbort(int, siginfo_t*, void*) {
  volatile uintptr_t here = 0;
  rex::ps5diag::UnhandledHostFault(0, 0xAB0127, reinterpret_cast<uintptr_t>(&here));
  signal(SIGABRT, SIG_DFL);
  abort();
}

const bool g_abort_handler = [] {
  struct sigaction sa {};
  sa.sa_sigaction = OnAbort;
  sa.sa_flags = SA_SIGINFO;
  sigaction(SIGABRT, &sa, nullptr);
  sigaction(SIGTRAP, &sa, nullptr);  // (an int3: __builtin_debugtrap)
  sigaction(SIGFPE, &sa, nullptr);
  return true;
}();

}  // namespace
}  // namespace cdps5

#endif  // REX_PLATFORM_PS5
