#!/usr/bin/env python3
"""The game frame on screen, for overlays tied to the game's own frame (rex/cd_frame_sync.h; the orb
map's minimap dots, crackdown-recomp/src/orb_map.cpp through ci/local_build.sh).

    fw_frame_sync.py <framework>     (after fw_fence_fixes.py)
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
        sys.exit(f"fw_frame_sync: {rel}: expected {count} of {old[:90]!r}, found {n}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print(f"patched {rel}")


kv = "src/kernel/xboxkrnl/xboxkrnl_video.cpp"
edit(kv, "#include <rex/ui/flags.h>\n", "#include <rex/ui/flags.h>\n#include <rex/cd_frame_sync.h>  // [cd]\n")
edit(kv,
     "                  mapped_u32 height) {\n"
     "  // All of these parameters are REQUIRED.\n",
     "                  mapped_u32 height) {\n"
     "  rex::cd_frame_sync::cpu_swaps.fetch_add(1, std::memory_order_relaxed);  // [cd] the game's frame count\n"
     "  // All of these parameters are REQUIRED.\n")
vk = "src/graphics/vulkan/command_processor.cpp"
edit(vk, "#include <rex/cd_perf.h>\n", "#include <rex/cd_perf.h>\n#include <rex/cd_frame_sync.h>  // [cd]\n")
edit(vk,
     "                                       uint32_t frontbuffer_height) {\n"
     "  SCOPE_profile_cpu_f(\"gpu\");\n"
     "  CdFlushDeferredReadbacks(kCdFlushSwap);  // [cd] (fw_readback_fixes.py)\n",
     "                                       uint32_t frontbuffer_height) {\n"
     "  SCOPE_profile_cpu_f(\"gpu\");\n"
     "  rex::cd_frame_sync::gpu_swaps.fetch_add(1, std::memory_order_relaxed);  // [cd] the frame now on screen\n"
     "  CdFlushDeferredReadbacks(kCdFlushSwap);  // [cd] (fw_readback_fixes.py)\n")
