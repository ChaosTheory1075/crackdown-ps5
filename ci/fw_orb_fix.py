#!/usr/bin/env python3
"""How many frames the game runs ahead of the GPU command thread, and a limit on it (2026-11-11, UNPROVEN).

The game reads small resolved targets on the CPU and places the orbs, supply-point beams and waypoints with
them: a frame late (readback_resolve "fast") they slid with the camera (2026-11-05). "full" gives the data when
the GPU THREAD reaches the resolve; if the game is 2+ frames ahead of the GPU thread the data is as stale as
"fast" made it. Nothing in a log says how far ahead it is. This adds:
- "cd frame lead: avg/max" every 10 s (frames the game was ahead at each swap), always on;
- cd_frame_lead_max (default 0 = no limit, as before): VdSwap waits (100 ms at most) until the game is at most
  that many frames ahead. 1 = the game prepares one frame while the GPU thread draws the last.

    fw_orb_fix.py <framework>     (after fw_frame_sync.py)
"""
import pathlib
import sys

sdk = pathlib.Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"


def edit(rel, old, new, count=1):
    p = sdk / rel
    s = p.read_text(encoding="utf-8")
    if new in s:
        print(f"already {rel}")
        return
    n = s.count(old)
    if n != count:
        sys.exit(f"fw_orb_fix: {rel}: expected {count} of {old[:90]!r}, found {n}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print(f"patched {rel}")


h = "include/rex/cd_frame_sync.h"
edit(h, "}  // namespace rex::cd_frame_sync\n",
     "inline std::atomic<int32_t> lead_max{0};    // cd_frame_lead_max, set by the GPU thread (0 = no limit)\n"
     "}  // namespace rex::cd_frame_sync\n")

kv = "src/kernel/xboxkrnl/xboxkrnl_video.cpp"
edit(kv,
     "  rex::cd_frame_sync::cpu_swaps.fetch_add(1, std::memory_order_relaxed);  // [cd] the game's frame count\n",
     "  if (const int32_t cd_lead = rex::cd_frame_sync::lead_max.load(std::memory_order_relaxed); cd_lead > 0) {\n"
     "    // [cd] (fw_orb_fix.py) at most cd_lead frames ahead of the GPU thread; never longer than 100 ms\n"
     "    const auto cd_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);\n"
     "    while (int32_t(rex::cd_frame_sync::cpu_swaps.load(std::memory_order_relaxed) -\n"
     "                   rex::cd_frame_sync::gpu_swaps.load(std::memory_order_relaxed)) >= cd_lead &&\n"
     "           std::chrono::steady_clock::now() < cd_until) {\n"
     "      std::this_thread::sleep_for(std::chrono::microseconds(200));\n"
     "    }\n"
     "  }\n"
     "  rex::cd_frame_sync::cpu_swaps.fetch_add(1, std::memory_order_relaxed);  // [cd] the game's frame count\n")
edit(kv, "#include <rex/cd_frame_sync.h>  // [cd]\n",
     "#include <rex/cd_frame_sync.h>  // [cd]\n#include <chrono>\n#include <thread>\n")

vk = "src/graphics/vulkan/command_processor.cpp"
edit(vk, "REXCVAR_DEFINE_BOOL(cd_fence_deferred, false, \"GPU\",  // [cd] (fw_fence_fixes.py)\n",
     "REXCVAR_DEFINE_INT32(cd_frame_lead_max, 0, \"GPU\",  // [cd] (fw_orb_fix.py)\n"
     "                     \"The game waits at its swap until it is at most this many frames ahead of the GPU \"\n"
     "                     \"command thread (0 = no limit)\");\n"
     "REXCVAR_DEFINE_BOOL(cd_fence_deferred, false, \"GPU\",  // [cd] (fw_fence_fixes.py)\n")
edit(vk,
     "  rex::cd_frame_sync::gpu_swaps.fetch_add(1, std::memory_order_relaxed);  // [cd] the frame now on screen\n"
     "  CdFlushDeferredReadbacks(kCdFlushSwap);  // [cd] (fw_readback_fixes.py)\n",
     "  CdFlushDeferredReadbacks(kCdFlushSwap);  // [cd] (fw_readback_fixes.py)\n"
     "  rex::cd_frame_sync::gpu_swaps.fetch_add(1, std::memory_order_relaxed);  // [cd] the frame now on screen\n"
     "  {  // [cd] (fw_orb_fix.py) how far ahead the game is, every 10 s\n"
     "    rex::cd_frame_sync::lead_max.store(REXCVAR_GET(cd_frame_lead_max), std::memory_order_relaxed);\n"
     "    static uint64_t cd_lead_sum = 0, cd_lead_n = 0;\n"
     "    static int32_t cd_lead_top = 0;\n"
     "    static auto cd_lead_last = std::chrono::steady_clock::now();\n"
     "    const int32_t cd_lead = int32_t(rex::cd_frame_sync::cpu_swaps.load(std::memory_order_relaxed) -\n"
     "                                    rex::cd_frame_sync::gpu_swaps.load(std::memory_order_relaxed));\n"
     "    cd_lead_sum += uint64_t(std::max(cd_lead, 0));\n"
     "    ++cd_lead_n;\n"
     "    cd_lead_top = std::max(cd_lead_top, cd_lead);\n"
     "    const auto cd_lead_now = std::chrono::steady_clock::now();\n"
     "    if (cd_lead_now - cd_lead_last >= std::chrono::seconds(10)) {\n"
     "      REXGPU_INFO(\"cd frame lead: the game ahead of the GPU thread by {:.2f} frames on average, {} at most \"\n"
     "                  \"(limit {})\", double(cd_lead_sum) / double(cd_lead_n), cd_lead_top,\n"
     "                  REXCVAR_GET(cd_frame_lead_max));\n"
     "      cd_lead_sum = cd_lead_n = 0;\n"
     "      cd_lead_top = 0;\n"
     "      cd_lead_last = cd_lead_now;\n"
     "    }\n"
     "  }\n")

# With the limit on, the swap's flush waits for the kept fence groups, so the frame's read-backs are in guest
# memory before the game is let past its swap.
edit(vk,
     "const bool ordered = reason != kCdFlushReadPointer && reason != kCdFlushWaitWriteback && reason != kCdFlushSwap;",
     "const bool ordered = reason != kCdFlushReadPointer && reason != kCdFlushWaitWriteback &&\n"
     "                         (reason != kCdFlushSwap || REXCVAR_GET(cd_frame_lead_max) > 0);")
