#!/usr/bin/env python3
"""Predicated tiling, rasterized as Xenia Canary does it now (Crackdown 2: 32 tiles a frame; the agent heads' bar of
static and the menu static, in some frames, in one part of the screen only).

Canary 6579cc2a4 (2026-10-01) "Let the host viewport start at a negative coordinate": GetHostViewportInfo cropped the
host viewport's origin at 0 and made up for it in the NDC scale and offset. A tile drawn with a negative window offset
(PA_SC_WINDOW_OFFSET, how predicated tiling moves each tile to the EDRAM base) then got a different NDC transform from
its neighbour, and the same geometry rasterized differently in one tile (Blue Dragon: a flat wedge from the tile
boundary). The origin may now go down to -max (Vulkan guarantees viewportBoundsRange covers -2 * maxViewportDimensions);
the extent stays within max. ViewportInfo::xy_offset is signed; our only user of it (the Vulkan viewport) takes it as a
float. cd_viewport_negative_origin (default false; toml: cd_viewport_negative_origin = true).

Canary e1f4a0d00 (2026-10-02) "Put tiling window offsets in EDRAM bases" (+690 -247): whole-tile negative window
offsets go into the render targets' EDRAM bases, so every tile rasterizes like the full-screen depth prepass it
depth-tests against ("depth garbage in the bottom tiles" of 4D530919, 425607E5, 5345080C). Ported (Vulkan + SPIR-V)
in ci/tiling/port_e1f4a0d00.py; cvar cd_window_offset_in_edram (default false; toml: cd_window_offset_in_edram = true).
See Extras/CD2-TILING.md.

    fw_tiling_fixes.py <framework>     (after fw_xma_fixes.py)
"""
import pathlib
import subprocess
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        sys.exit(f"fw_tiling_fixes: expected {count} of {old!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    fw = pathlib.Path(sys.argv[1])
    sdk = fw / "glue" / "rexglue-sdk-main"
    draw = sdk / "src" / "graphics" / "util" / "draw.cpp"
    edit(draw, 'REXCVAR_DEFINE_BOOL(half_pixel_offset, true, "GPU", "Enable half pixel offset");\n',
         'REXCVAR_DEFINE_BOOL(half_pixel_offset, true, "GPU", "Enable half pixel offset");\n'
         'REXCVAR_DEFINE_BOOL(cd_viewport_negative_origin, false, "GPU",  // [cd] Xenia Canary 6579cc2a4\n'
         '                    "The host viewport may start at a negative coordinate (predicated tiling tiles with a "\n'
         '                    "negative window offset get the same NDC transform as their neighbours)");\n')
    edit(draw,
         "      uint32_t axis_0_int =\n"
         "          uint32_t(rex::clamp_float(offset_axis - scale_axis_abs, 0.0f, axis_max_unscaled_float));\n"
         "      uint32_t axis_1_int =\n"
         "          uint32_t(rex::clamp_float(offset_axis + scale_axis_abs, 0.0f, axis_max_unscaled_float));\n"
         "      uint32_t axis_extent_int = axis_1_int - axis_0_int;\n"
         "      viewport_info_out.xy_offset[i] = axis_0_int * axis_resolution_scale;\n",
         "      int32_t axis_0_int, axis_1_int;\n"
         "      uint32_t axis_extent_int;\n"
         "      if (REXCVAR_GET(cd_viewport_negative_origin)) {\n"
         "        // [cd] Canary 6579cc2a4: the start may be negative, down to -max, instead of cropped at 0 (a tile\n"
         "        // drawn at a negative window offset then gets the same NDC scale and offset as its neighbours);\n"
         "        // Vulkan's viewportBoundsRange covers -2 * maxViewportDimensions. The extent stays within max.\n"
         "        axis_1_int = int32_t(std::floor(rex::clamp_float(offset_axis + scale_axis_abs, 0.0f,\n"
         "                                                         axis_max_unscaled_float)));\n"
         "        axis_0_int = int32_t(std::floor(rex::clamp_float(offset_axis - scale_axis_abs,\n"
         "                                                         float(axis_1_int) - axis_max_unscaled_float,\n"
         "                                                         axis_max_unscaled_float)));\n"
         "        axis_extent_int = axis_1_int > 0 ? uint32_t(axis_1_int - axis_0_int) : 0;\n"
         "      } else {\n"
         "        axis_0_int = int32_t(\n"
         "            uint32_t(rex::clamp_float(offset_axis - scale_axis_abs, 0.0f, axis_max_unscaled_float)));\n"
         "        axis_1_int = int32_t(\n"
         "            uint32_t(rex::clamp_float(offset_axis + scale_axis_abs, 0.0f, axis_max_unscaled_float)));\n"
         "        axis_extent_int = uint32_t(axis_1_int - axis_0_int);\n"
         "      }\n"
         "      viewport_info_out.xy_offset[i] = axis_0_int * int32_t(axis_resolution_scale);\n")
    # [cd] Which tiles: each distinct negative window offset is logged once ("cd tiling offset"), with the surface
    # pitch, MSAA, depth enable/func and whether the scissor ignores the offset. Tiles depth-testing EQUAL (or
    # LEQUAL with writes off) against a full-screen prepass are the case Canary e1f4a0d00 fixes. Log only.
    edit(draw,
         "  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {\n"
         "    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();\n"
         "    offset_add_xy[0] += float(pa_sc_window_offset.window_x_offset);\n"
         "    offset_add_xy[1] += float(pa_sc_window_offset.window_y_offset);\n"
         "  }\n",
         "  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {\n"
         "    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();\n"
         "    offset_add_xy[0] += float(pa_sc_window_offset.window_x_offset);\n"
         "    offset_add_xy[1] += float(pa_sc_window_offset.window_y_offset);\n"
         "    if (pa_sc_window_offset.window_x_offset < 0 || pa_sc_window_offset.window_y_offset < 0) {\n"
         "      // [cd] (fw_tiling_fixes.py) each distinct negative offset once, up to 32\n"
         "      static std::atomic<uint32_t> cd_seen[32];\n"
         "      static std::atomic<uint32_t> cd_seen_count{0};\n"
         "      const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();\n"
         "      const auto rb_depthcontrol = regs.Get<reg::RB_DEPTHCONTROL>();\n"
         "      const uint32_t key = (pa_sc_window_offset.value & 0x7FFF7FFFu) ^\n"
         "                           (uint32_t(rb_depthcontrol.zfunc) << 15) ^ (uint32_t(rb_depthcontrol.z_enable) << 31);\n"
         "      bool seen = false;\n"
         "      const uint32_t n = std::min<uint32_t>(cd_seen_count.load(std::memory_order_relaxed), 32);\n"
         "      for (uint32_t s = 0; s < n && !seen; ++s) {\n"
         "        seen = cd_seen[s].load(std::memory_order_relaxed) == key;\n"
         "      }\n"
         "      if (!seen) {\n"
         "        const uint32_t slot = cd_seen_count.fetch_add(1, std::memory_order_relaxed);\n"
         "        if (slot < 32) {\n"
         "          cd_seen[slot].store(key, std::memory_order_relaxed);\n"
         "          REXGPU_INFO(\"cd tiling offset {},{}: pitch {}, msaa {}, depth {} func {} write {}, scissor \"\n"
         "                      \"ignores offset {}\",\n"
         "                      int(pa_sc_window_offset.window_x_offset), int(pa_sc_window_offset.window_y_offset),\n"
         "                      uint32_t(rb_surface_info.surface_pitch), uint32_t(rb_surface_info.msaa_samples),\n"
         "                      uint32_t(rb_depthcontrol.z_enable), uint32_t(rb_depthcontrol.zfunc),\n"
         "                      uint32_t(rb_depthcontrol.z_write_enable),\n"
         "                      uint32_t(regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable));\n"
         "        }\n"
         "      }\n"
         "    }\n"
         "  }\n")
    edit(sdk / "include" / "rex" / "graphics" / "util" / "draw.h", "  uint32_t xy_offset[2];\n",
         "  int32_t xy_offset[2];  // [cd] (signed: Canary 6579cc2a4, cd_viewport_negative_origin)\n")
    # Canary e1f4a0d00, the window offset in the EDRAM bases (cd_window_offset_in_edram, default false): the port
    # is its own script (ci/tiling/port_e1f4a0d00.py, against the tree as the edits above leave it).
    if "cd_window_offset_in_edram" in draw.read_text():
        print(f"already  {draw} (e1f4a0d00)")
    else:
        subprocess.run([sys.executable, str(pathlib.Path(__file__).parent / "tiling" / "port_e1f4a0d00.py"), str(sdk)],
                       check=True)


if __name__ == "__main__":
    main()
