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

    # Occlusion queries on the ACCURATE renderer (fragment shader interlock): the pixel shader does
    # the depth / stencil test itself and only skips its EDRAM writes for failing samples, so the
    # pixel stays alive and the host occlusion query counted every pixel drawn: light flares and
    # glows that games hide with queries showed through walls (Metal Gear Rising, The Darkness II).
    # Kill the pixel at the end of the shader (after its EDRAM writes) when no sample passed, as
    # the console's depth test would: the query then counts only the samples that passed. Same as
    # mgr-ps5 7aa9915; Crackdown gets it before anyone has to report it.
    rb_occlusion = sdk / "src" / "graphics" / "pipeline" / "shader" / "spirv_translator_rb.cpp"
    edit(rb_occlusion,
         "void SpirvShaderTranslator::CompleteFragmentShaderInMain() {\n"
         "  // Loaded if needed.\n"
         "  spv::Id msaa_samples = spv::NoResult;\n",
         "void SpirvShaderTranslator::CompleteFragmentShaderInMain() {\n"
         "  // Loaded if needed.\n"
         "  spv::Id msaa_samples = spv::NoResult;\n"
         "\n"
         "  // [crackdown] Whether any sample passed the FSI depth / stencil test (occlusion queries).\n"
         "  spv::Id var_fsi_any_sample_passed = spv::NoResult;\n"
         "  if (edram_fragment_shader_interlock_) {\n"
         "    var_fsi_any_sample_passed = builder_->createVariable(\n"
         "        spv::NoPrecision, spv::StorageClassFunction, type_bool_,\n"
         "        \"xe_var_fsi_any_sample_passed\", builder_->makeBoolConstant(false));\n"
         "  }\n")
    fsi_store_passed = (
        "      builder_->createStore(\n"
        "          builder_->createBinOp(\n"
        "              spv::OpINotEqual, type_bool_,\n"
        "              builder_->createBinOp(spv::OpBitwiseAnd, type_uint_, main_fsi_sample_mask_,\n"
        "                                    builder_->makeUintConstant((uint32_t(1) << 4) - 1)),\n"
        "              const_uint_0_),\n"
        "          var_fsi_any_sample_passed);  // [crackdown]\n")
    edit(rb_occlusion,
         "    if (FSI_IsDepthStencilEarly()) {\n"
         "      // Perform late depth / stencil writes for samples not discarded.\n",
         "    if (FSI_IsDepthStencilEarly()) {\n" + fsi_store_passed +
         "      // Perform late depth / stencil writes for samples not discarded.\n")
    edit(rb_occlusion,
         "      FSI_DepthStencilTest(msaa_samples,\n"
         "                           fsi_pixel_potentially_killed || (color_targets_written & 0b1));\n",
         "      FSI_DepthStencilTest(msaa_samples,\n"
         "                           fsi_pixel_potentially_killed || (color_targets_written & 0b1));\n"
         + fsi_store_passed)
    edit(rb_occlusion,
         "    builder_->createNoResultOp(spv::OpEndInvocationInterlockEXT);\n"
         "  }\n"
         "}\n",
         "    builder_->createNoResultOp(spv::OpEndInvocationInterlockEXT);\n"
         "\n"
         "    // [crackdown] Not counted by occlusion queries when every sample failed (see ci/patch_framework.py).\n"
         "    SpirvBuilder::IfBuilder fsi_no_sample_passed_if(\n"
         "        builder_->createUnaryOp(\n"
         "            spv::OpLogicalNot, type_bool_,\n"
         "            builder_->createLoad(var_fsi_any_sample_passed, spv::NoPrecision)),\n"
         "        spv::SelectionControlDontFlattenMask, *builder_);\n"
         "    builder_->createNoResultOp(spv::OpKill);\n"
         "    // OpKill terminates the block.\n"
         "    fsi_no_sample_passed_if.makeEndIf(false);\n"
         "  }\n"
         "}\n")

    # The game's rumble in the log (the completeness checklist's proof): the first non-zero
    # XInputSetState of the session, then every 10 s how many there were and the strongest motors.
    # "rumble mode 0x0" at connect is only scePadSetVibrationMode's success code.
    edit(sdk / "src" / "input" / "ps5" / "ps5_input_driver.cpp",
         "  if (vibration) {\n"
         "    pad->native_low = vibration->left_motor_speed;\n"
         "    pad->native_high = vibration->right_motor_speed;\n"
         "    SendVibration(*pad);\n"
         "  }\n",
         "  if (vibration) {\n"
         "    pad->native_low = vibration->left_motor_speed;\n"
         "    pad->native_high = vibration->right_motor_speed;\n"
         "    SendVibration(*pad);\n"
         "    // [cd] the game's rumble, for the log\n"
         "    static uint64_t cd_rumbles = 0, cd_last_ms = 0;\n"
         "    static uint32_t cd_max_low = 0, cd_max_high = 0;\n"
         "    static bool cd_first_logged = false;\n"
         "    if (vibration->left_motor_speed || vibration->right_motor_speed) {\n"
         "      if (!cd_first_logged && (cd_first_logged = true))\n"
         "        REXLOG_INFO(\"PS5 input: the game's first rumble (left {}, right {})\",\n"
         "                    uint32_t(vibration->left_motor_speed), uint32_t(vibration->right_motor_speed));\n"
         "      ++cd_rumbles;\n"
         "      cd_max_low = std::max<uint32_t>(cd_max_low, vibration->left_motor_speed);\n"
         "      cd_max_high = std::max<uint32_t>(cd_max_high, vibration->right_motor_speed);\n"
         "    }\n"
         "    const uint64_t cd_now = SDL_GetTicks();\n"
         "    if (cd_rumbles && cd_now - cd_last_ms >= 10000) {\n"
         "      REXLOG_INFO(\"PS5 input: rumble {} times in 10 s (strongest left {}, right {})\", cd_rumbles,\n"
         "                  cd_max_low, cd_max_high);\n"
         "      cd_rumbles = 0, cd_max_low = cd_max_high = 0, cd_last_ms = cd_now;\n"
         "    }\n"
         "  }\n")


if __name__ == "__main__":
    main()
