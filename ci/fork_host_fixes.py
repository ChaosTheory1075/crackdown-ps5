#!/usr/bin/env python3
"""Host (Linux) build fixes for gta4-ps5's ReXGlue fork, for the codegen CLI.

    fork_host_fixes.py <gta4-ps5 checkout>

Same as mp3-ps5's ci/patch_framework.py host part:
- disc_image_device.cpp includes direct_io.h on every non-Windows host.
- rex::ps5diag counters get a host definition (ps5_diag.cpp is PS5-only).
"""
import pathlib
import sys


def edit(path, old, new):
    text = path.read_text()
    if new in text:
        return
    if text.count(old) != 1:
        sys.exit(f"fork_host_fixes: expected one {old!r} in {path}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


sdk = pathlib.Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"
edit(sdk / "src" / "filesystem" / "devices" / "disc_image_device.cpp",
     "#include <rex/platform.h>\n#if REX_PLATFORM_PS5\n#include <rex/filesystem/direct_io.h>\n#endif\n",
     "#include <rex/platform.h>\n#include <rex/filesystem/direct_io.h>\n")
(sdk / "src" / "core" / "ps5_diag_host.cpp").write_text(
    "#include <rex/ps5_diag.h>\n\n"
    "namespace rex::ps5diag {\n"
    "std::atomic<uint64_t> counters[kCount];\n"
    "uintptr_t guest_arena_lo = 0, guest_arena_hi = 0;\n"
    "}  // namespace rex::ps5diag\n")
edit(sdk / "src" / "core" / "CMakeLists.txt",
     "    target_sources(rexcore PRIVATE ps5_libc_extras.c ps5_diag.cpp)\n",
     "    target_sources(rexcore PRIVATE ps5_libc_extras.c ps5_diag.cpp)\n"
     "else()\n"
     "    target_sources(rexcore PRIVATE ps5_diag_host.cpp)\n"
     "endif()\n"
     "if(PS5)\n")
