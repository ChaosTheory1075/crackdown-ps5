#!/usr/bin/env python3
"""Crackdown 1/2: what the waypoint and orb markers' draws ask the GPU for (2026-11-11). Log only.

The user's capture-card frames (Crackdown 1, FAST and ACCURATE alike): the waypoint disc lines up with its beacon
pillar but is cut off by whatever is nearer, even the player's own head - it is depth-tested against the scene, while
on the Xbox 360 it shows on top. To see why, every 15 s one whole frame is captured (at most 24 times a run), and each
distinct state of its overlay-like draws (blending on, alpha test on, or depth writes off) is written once with how
many draws used it: shader hashes, first texture, depth/stencil control, blend, alpha test, viewport transform and Z
scale/offset, clipping, polygon mode, depth base, window offset, bin select and predication, draw initiator.

    fw_marker_log.py <framework>      (after fw_tiling_log.py)
"""
import sys
from pathlib import Path

sdk = Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"
cp = sdk / "src/graphics/command_processor.cpp"
s = cp.read_text(encoding="utf-8")
if "cd marker:" in s:
    print("fw_marker_log: already applied")
    sys.exit(0)


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f"fw_marker_log: expected 1 of {old[:80]!r}, found {s.count(old)}")
    s = s.replace(old, new)


rep("CdTileLog cd_tile_log;\n}  // namespace\n",
    r'''CdTileLog cd_tile_log;
// [cd] (fw_marker_log.py) one frame's overlay-like draw states every 15 s
struct CdMarkerLog {
  std::vector<std::string> lines;
  std::vector<uint64_t> keys;
  std::vector<uint32_t> counts;
  bool capture = false;
  uint32_t frames = 0, draws = 0, frame_draws = 0;
  std::chrono::steady_clock::time_point next = std::chrono::steady_clock::now() + std::chrono::seconds(20);
};
CdMarkerLog cd_marker_log;
}  // namespace
''')

rep("    ++GpuLane::Get().tiling_draws;\n    if (cd_tile_log.frames_written < 2) {  // [cd] (fw_tiling_log.py)\n",
    r'''    ++GpuLane::Get().tiling_draws;
    if (cd_marker_log.capture) {  // [cd] (fw_marker_log.py)
      const RegisterFile& mr = *register_file_;
      ++cd_marker_log.frame_draws;
      const auto dc = mr.Get<reg::RB_DEPTHCONTROL>();
      const uint32_t blend = mr.Get<reg::RB_BLENDCONTROL>(reg::RB_BLENDCONTROL::rt_register_indices[0]).value;
      const auto cc = mr.Get<reg::RB_COLORCONTROL>();
      // opaque = ONE, ADD, ZERO for color and alpha
      if ((blend & 0x1FFF1FFFu) != 0x00010001u || cc.alpha_test_enable || !dc.z_write_enable) {
        const uint64_t vs = active_vertex_shader_ ? active_vertex_shader_->ucode_data_hash() : 0;
        const uint64_t ps = active_pixel_shader_ ? active_pixel_shader_->ucode_data_hash() : 0;
        const auto vte = mr.Get<reg::PA_CL_VTE_CNTL>();
        const auto clip = mr.Get<reg::PA_CL_CLIP_CNTL>();
        const auto mode = mr.Get<reg::PA_SU_SC_MODE_CNTL>();
        const auto di = mr.Get<reg::RB_DEPTH_INFO>();
        const auto wo = mr.Get<reg::PA_SC_WINDOW_OFFSET>();
        const uint32_t zs_bits = mr.values[XE_GPU_REG_PA_CL_VPORT_ZSCALE];
        const uint32_t zo_bits = mr.values[XE_GPU_REG_PA_CL_VPORT_ZOFFSET];
        const uint32_t tex0 = uint32_t(mr.GetTextureFetch(0).base_address) << 12;
        const uint8_t* mp = memory_->TranslatePhysical(reader->read_ptr());
        const uint32_t d0 = count > 0 ? memory::load_and_swap<uint32_t>(mp) : 0;
        const uint32_t d1 = count > 1 ? memory::load_and_swap<uint32_t>(mp + 4) : 0;
        const uint32_t initiator = opcode == PM4_DRAW_INDX ? d1 : d0;
        const uint64_t key = vs ^ (ps * 31) ^ (uint64_t(dc.value) << 3) ^ (uint64_t(blend) << 17) ^
                             (uint64_t(cc.value) << 29) ^ (uint64_t(vte.value) << 37) ^
                             (uint64_t(clip.value) << 11) ^ (uint64_t(mode.value) << 41) ^
                             (uint64_t(zs_bits) << 7) ^ (uint64_t(zo_bits) << 23) ^ (uint64_t(di.value) << 47) ^
                             (uint64_t(wo.value) << 13) ^ (uint64_t(tex0) << 5) ^ bin_select_ ^
                             (uint64_t(packet & 1) << 63) ^ (uint64_t(initiator & 0x3F) << 53);
        size_t at = 0;
        while (at < cd_marker_log.keys.size() && cd_marker_log.keys[at] != key) ++at;
        if (at < cd_marker_log.keys.size()) {
          ++cd_marker_log.counts[at];
        } else if (cd_marker_log.keys.size() < 120) {
          float zs, zo;
          std::memcpy(&zs, &zs_bits, 4);
          std::memcpy(&zo, &zo_bits, 4);
          cd_marker_log.keys.push_back(key);
          cd_marker_log.counts.push_back(1);
          cd_marker_log.lines.push_back(fmt::format(
              "draw {} | vs {:016X} ps {:016X} tex0 {:08X} | depth {:08X} (z {} write {} func {} stencil {}) "
              "base {} fmt {} | blend {:08X} alphatest {} func {} | vte {:03X} zscale {} zoffset {} | clip {:08X} "
              "(disable {} dx {}) | scmode {:08X} | offset {},{} | bin {:X} pred {} | init {:08X} prim {} n {} | "
              "viz {:08X}",
              cd_marker_log.frame_draws, vs, ps, tex0, dc.value, uint32_t(dc.z_enable), uint32_t(dc.z_write_enable),
              uint32_t(dc.zfunc), uint32_t(dc.stencil_enable), uint32_t(di.depth_base), uint32_t(di.depth_format),
              blend, uint32_t(cc.alpha_test_enable), uint32_t(cc.alpha_func), vte.value, zs, zo, clip.value,
              uint32_t(clip.clip_disable), uint32_t(clip.dx_clip_space_def), mode.value,
              int(wo.window_x_offset), int(wo.window_y_offset), bin_select_, packet & 1, initiator,
              initiator & 0x3F, initiator >> 16, mr.Get<reg::PA_SC_VIZ_QUERY>().value));
        }
      }
    }
    if (cd_tile_log.frames_written < 2) {  // [cd] (fw_tiling_log.py)
''')

rep("  if (cd_tile_log.frames_written < 2) {  // [cd] (fw_tiling_log.py) a heavy frame's tile layout, written\n",
    r'''  {  // [cd] (fw_marker_log.py) write the captured frame; arm the next one every 15 s
    if (cd_marker_log.capture) {
      cd_marker_log.capture = false;
      ++cd_marker_log.frames;
      REXGPU_INFO("cd marker: frame {} of the run: {} draws, {} overlay-like states (count: state):",
                  cd_marker_log.frames, cd_marker_log.frame_draws, cd_marker_log.lines.size());
      for (size_t i = 0; i < cd_marker_log.lines.size(); ++i)
        REXGPU_INFO("cd marker: {:4}x {}", cd_marker_log.counts[i], cd_marker_log.lines[i]);
      cd_marker_log.lines.clear();
      cd_marker_log.keys.clear();
      cd_marker_log.counts.clear();
    }
    const auto now = std::chrono::steady_clock::now();
    if (cd_marker_log.frames < 24 && now >= cd_marker_log.next) {
      cd_marker_log.capture = true;
      cd_marker_log.frame_draws = 0;
      cd_marker_log.next = now + std::chrono::seconds(15);
    }
  }
  if (cd_tile_log.frames_written < 2) {  // [cd] (fw_tiling_log.py) a heavy frame's tile layout, written
''')
if "#include <chrono>" not in s:
    s = s.replace("#include <algorithm>\n", "#include <algorithm>\n#include <chrono>\n", 1)
cp.write_text(s, encoding="utf-8")
print("patched src/graphics/command_processor.cpp (cd marker log)")
