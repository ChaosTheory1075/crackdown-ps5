#!/usr/bin/env python3
"""No profiling, sampling or in-play cache save in the user's build (supervisor order 2026-11-06, as
The Darkness's port_nodiag.py / MGR 4292ac5 and 70795c4). Run last, after fw_readback_fixes.py.

1. rex/cd_perf.h: kProfile = false. Every Scope timer is a no-op (no clock read), the per-draw count
   is compiled out, and IssueSwap's 10 s "cd perf" report and 100 ms frame report are compiled out.
2. The Vulkan pipeline cache is loaded at start and NEVER saved in play: the periodic save
   (vkGetPipelineCacheData of 13-21 MB and the write) was a ~300 ms hitch (MGR mgr_025/026, our
   memory note pipeline-cache-saver-hitch). Pipelines made since are kept by the driver's own disk
   cache and the .xpso pipeline descriptions, as in MGR, The Darkness and Saints Row.
3. The resolve timers' 10 s "cd copy" / "cd readback batched" report: compiled out with kProfile.
(The CPU sampler, ps5_cpu_sampler, is turned off by the apps' OnPreSetup.)
"""
import sys
from pathlib import Path

sdk = Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"


def edit(rel, old, new, count=1):
    p = sdk / rel
    s = p.read_text(encoding="utf-8")
    n = s.count(old)
    if n != count:
        sys.exit(f"fw_release_fixes: {rel}: expected {count} of {old[:80]!r}, found {n}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print(f"patched {rel}")


perf = "include/rex/cd_perf.h"
if "kProfile" not in (sdk / perf).read_text(encoding="utf-8"):
    edit(perf, "namespace rex::cd_perf {\n",
         "namespace rex::cd_perf {\n\n"
         "// false in the user's build (ci/fw_release_fixes.py): no timer reads the clock, no report is logged\n"
         "inline constexpr bool kProfile = false;\n")
    edit(perf, "  explicit Scope(Counter c) : c(c), start(NowNs()) {}\n  void Stop() {\n    if (!on) return;\n",
         "  explicit Scope(Counter c) : c(c), start(kProfile ? NowNs() : 0), on(kProfile) {}\n"
         "  void Stop() {\n    if (!kProfile || !on) return;\n")
    edit(perf, "  void Next(Counter next) {\n    Stop();\n",
         "  void Next(Counter next) {\n    if constexpr (!kProfile) return;\n    Stop();\n")

    vcp = "src/graphics/vulkan/command_processor.cpp"
    edit(vcp, "  {\n    // [cd] the GPU thread's time per frame, every 10 s (rex/cd_perf.h)\n",
         "  if constexpr (rex::cd_perf::kProfile) {\n"
         "    // [cd] the GPU thread's time per frame, every 10 s (rex/cd_perf.h; off in the user's build)\n")
    edit(vcp, "      SrSavePipelineCache();  // (pipeline_cache.cpp)\n",
         "      // (the pipeline cache: loaded at start, never saved in play - ci/fw_release_fixes.py)\n")
    edit(vcp, "  rex::cd_perf::g_n[rex::cd_perf::kDraw].fetch_add(1, std::memory_order_relaxed);\n",
         "  if constexpr (rex::cd_perf::kProfile) rex::cd_perf::g_n[rex::cd_perf::kDraw].fetch_add(1, std::memory_order_relaxed);\n")
    edit(vcp, "  static auto cd_last = std::chrono::steady_clock::now();\n  const auto cd_now = std::chrono::steady_clock::now();\n"
              "  if (cd_now - cd_last < std::chrono::seconds(10)) {\n    return;\n  }\n",
         # (2026-11-07: the copy report stays in the user's build. Its counters run anyway and are per resolve and
         # per drain, not per draw; crackdown2_017's lines showed the EVENT_WRITE_SHD drains were the 10-13 ms of
         # GPU fences a frame, and 018/019 had no line to say whether cd_fence_deferred was on. It now says.)
         "  static auto cd_last = std::chrono::steady_clock::now();\n  const auto cd_now = std::chrono::steady_clock::now();\n"
         "  if (cd_now - cd_last < std::chrono::seconds(10)) {\n    return;\n  }\n"
         "  REXGPU_INFO(\"cd copy settings: readback_resolve {}, cd_readback_batched {}, cd_fence_deferred {}\",\n"
         "              REXCVAR_GET(readback_resolve), REXCVAR_GET(cd_readback_batched), REXCVAR_GET(cd_fence_deferred));\n")
    # The per-resolve readback log (a hash map entry each resolve, a line every 10 s): compiled out.
    edit(vcp, "  {  // [cd] the readback log\n",
         "  if constexpr (rex::cd_perf::kProfile) {  // [cd] the readback log (off in the user's build)\n")
    # SrSavePipelineCache is now unused: keep its definition (no warning as error for an unused extern function)
else:
    print("fw_release_fixes: already applied")
