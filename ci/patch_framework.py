#!/usr/bin/env python3
"""Adapt a gta4-ps5 checkout to build Crackdown instead of GTA IV.

    patch_framework.py <gta4-ps5 checkout> [--host]

- glue/CMakeLists.txt adds crackdown-recomp (copied in by the workflow) instead of
  gta4-recomp.
- src/graphics/CMakeLists.txt: on the PS5, the generic Xenos renderer
  (rexgpu-xenos) is built as a static library and linked in, and GTA IV's own
  renderer (rexgpu-gta4-native, its shader caches and FSR archive) is left out.
- src/filesystem/devices/disc_image_device.cpp includes direct_io.h on every
  non-Windows host (only the PS5 build had it).

Each edit checks that it applied, so a change upstream fails the build here
instead of somewhere later.
"""
import pathlib
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        sys.exit(f"patch_framework: expected {count} of {old!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    fw = pathlib.Path(sys.argv[1])
    sdk = fw / "glue" / "rexglue-sdk-main"

    edit(fw / "glue" / "CMakeLists.txt",
         'add_subdirectory("${REXSDK_DIR}/gta4-recomp" "${CMAKE_CURRENT_BINARY_DIR}/gta4-recomp")',
         'add_subdirectory("${REXSDK_DIR}/crackdown-recomp" "${CMAKE_CURRENT_BINARY_DIR}/crackdown-recomp")')

    gfx = sdk / "src" / "graphics" / "CMakeLists.txt"
    edit(gfx,
         "if(NOT PS5)\nadd_library(rexgpu-xenos SHARED\n",
         "if(PS5)\n"
         "# [crackdown] the generic Xenos renderer, linked into the PS5 program\n"
         "add_library(rexgpu-xenos STATIC\n"
         "    plugin_main.cpp\n"
         "    ${REXGPU_XENOS_SOURCES}\n"
         ")\n"
         "add_library(rex::gpu-xenos ALIAS rexgpu-xenos)\n"
         "rexgpu_configure_plugin(rexgpu-xenos gpu-xenos)\n"
         "else()\n"
         "add_library(rexgpu-xenos SHARED\n")
    # GTA IV's renderer block: off for this game.
    edit(gfx,
         "if(REXGLUE_USE_VULKAN)\n    if(REXGLUE_BUILD_TESTS)\n",
         "if(REXGLUE_USE_VULKAN AND NOT CD_NO_GTA4_NATIVE)\n    if(REXGLUE_BUILD_TESTS)\n")

    edit(sdk / "src" / "filesystem" / "devices" / "disc_image_device.cpp",
         "#include <rex/platform.h>\n#if REX_PLATFORM_PS5\n#include <rex/filesystem/direct_io.h>\n#endif\n",
         "#include <rex/platform.h>\n#include <rex/filesystem/direct_io.h>\n")

    # The bring-up counters (rex/ps5_diag.h) are counted outside PS5-only code
    # too; elsewhere than the PS5 nothing defines them (ps5_diag.cpp is PS5's).
    (sdk / "src" / "core" / "ps5_diag_host.cpp").write_text(
        "// [crackdown] rex/ps5_diag.h's counters off the PS5 (ps5_diag.cpp is PS5-only).\n"
        "#include <rex/ps5_diag.h>\n\n"
        "namespace rex::ps5diag {\n"
        "std::atomic<uint64_t> counters[kCount];\n"
        "uintptr_t guest_arena_lo = 0, guest_arena_hi = 0;\n"
        "}  // namespace rex::ps5diag\n")
    edit(sdk / "src" / "core" / "CMakeLists.txt",
         "    target_sources(rexcore PRIVATE ps5_libc_extras.c ps5_diag.cpp)\n",
         "    target_sources(rexcore PRIVATE ps5_libc_extras.c ps5_diag.cpp)\n"
         "else()\n"
         "    target_sources(rexcore PRIVATE ps5_diag_host.cpp)  # [crackdown]\n"
         "endif()\n"
         "if(PS5)\n")

    # XGetLanguage, which Crackdown asks its language from, always said English:
    # it says the user_language cvar now (the menu's LANGUAGE), as XCONFIG does.
    info = sdk / "src" / "kernel" / "xam" / "xam_info.cpp"
    edit(info, "#include <rex/cvar.h>\n", "#include <rex/cvar.h>\n#include <rex/system/flags.h>\n")
    edit(info,
         "u32 XGetLanguage_entry() {\n"
         "  auto desired_language = XLanguage::kEnglish;\n"
         "\n"
         "  // Switch the language based on game region.\n"
         "  // TODO(benvanik): pull from xex header.\n"
         "  uint32_t game_region = XEX_REGION_NTSCU;\n"
         "  if (game_region & XEX_REGION_NTSCU) {\n"
         "    desired_language = XLanguage::kEnglish;\n"
         "  } else if (game_region & XEX_REGION_NTSCJ) {\n"
         "    desired_language = XLanguage::kJapanese;\n"
         "  }\n"
         "  // Add more overrides?\n"
         "\n"
         "  return uint32_t(desired_language);\n"
         "}\n",
         "u32 XGetLanguage_entry() {\n"
         "  // [crackdown] the console's language, as XCONFIG_USER_LANGUAGE has it\n"
         "  const uint32_t language = REXCVAR_GET(user_language);\n"
         "  return language && language < uint32_t(XLanguage::kMaxLanguages) ? language\n"
         "                                                                    : uint32_t(XLanguage::kEnglish);\n"
         "}\n")

    # Every submission's end built the whole pipeline creation queue on the GPU thread and then
    # waited for the creation threads' pipelines in flight: with async compilation the draws that
    # need them are recorded with a placeholder anyway. PROTOTYPE: ReEvolution's every gameplay
    # hitch (100-650 ms in submit, each after a burst of new pipelines) was this; with async
    # compilation the queue is left to the creation threads.
    edit(sdk / "src" / "graphics" / "vulkan" / "pipeline_cache.cpp",
         "    if (startup_loading) {\n"
         "      creation_request_cond_.notify_one();\n"
         "    } else {\n",
         "    if (startup_loading || REXCVAR_GET(async_shader_compilation)) {  // [crackdown]\n"
         "      creation_request_cond_.notify_one();\n"
         "    } else {\n")

    # The DualSense's light bar: a colour of the game's own (Crackdown: the Agency's green, red when
    # hit), not only GTA IV's episode and wanted colours.
    sony_h = sdk / "include" / "rex" / "input" / "sony_feedback.h"
    edit(sony_h,
         "  uint32_t player_identity = 0;\n"
         "  uint32_t context_identity = 0;\n"
         "};\n",
         "  uint32_t player_identity = 0;\n"
         "  uint32_t context_identity = 0;\n"
         "  // [crackdown] the light bar's colour, when the game picks one\n"
         "  bool has_color = false;\n"
         "  std::array<uint8_t, 3> color{};\n"
         "};\n")
    edit(sdk / "src" / "input" / "sony_feedback.cpp",
         "  if (slot.game.wanted > 0 && now_ms >= slot.wanted_started_ms) {\n",
         "  if (slot.game.has_color) {  // [crackdown]\n"
         "    output.color = slot.game.color;\n"
         "  } else if (slot.game.wanted > 0 && now_ms >= slot.wanted_started_ms) {\n")


if __name__ == "__main__":
    main()
