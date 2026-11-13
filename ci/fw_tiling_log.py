#!/usr/bin/env python3
"""Crackdown 2's tile layout, for removing the second tile's replay of the scene (2026-11-06).

crackdown2_017: 4,300-11,000 draws a frame, ~6,400 of them predicated; the 2X MSAA 1280x720 surface is drawn in
two tiles (rows 0-479, then 480-719 at window offset 0,-480), so most of the scene's draws run twice. To merge
the tiles the render target cache needs the real layout: which EDRAM bases each tile's color and depth use, the
scissors, and how many draws each segment has. Written for two heavy frames a run (over 3,000 draws), one line
per change of (bin select, window offset, color 0 / depth base and format, scissor, edram mode), with the draws
since the last line. Log only; nothing drawn changes. Bounded: two frames' lines, at most 400.
"""
import sys
from pathlib import Path

sdk = Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"
cp = sdk / "src/graphics/command_processor.cpp"
s = cp.read_text(encoding="utf-8")
if "cd tile:" in s:
    print("fw_tiling_log: already applied")
    sys.exit(0)


def rep(old, new):
    global s
    if s.count(old) != 1:
        sys.exit(f"fw_tiling_log: expected 1 of {old[:80]!r}, found {s.count(old)}")
    s = s.replace(old, new)


rep("namespace rex::graphics {\n",
    r'''namespace rex::graphics {

namespace {
// [cd] (fw_tiling_log.py) the tile layout of a heavy frame: lines kept per frame, written for two frames
struct CdTileLog {
  std::vector<std::string> lines;
  uint64_t key = ~uint64_t(0);
  uint32_t draws = 0, frame_draws = 0, frames_written = 0;
  bool capture = false;  // after a block of 500+ draws: the packets between it and the next one
  std::vector<std::string> ops;
};
CdTileLog cd_tile_log;
}  // namespace
''')

rep("  const bool tiling_draw = opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2;\n",
    r'''  const bool tiling_draw = opcode == PM4_DRAW_INDX || opcode == PM4_DRAW_INDX_2;
  if (cd_tile_log.frames_written < 2 && cd_tile_log.capture && !tiling_draw && cd_tile_log.ops.size() < 160) {
    // [cd] (fw_tiling_log.py) a packet between two big blocks: opcode, size, predicate, first dwords
    const uint8_t* cd_p = memory_->TranslatePhysical(reader->read_ptr());
    const uint32_t d0 = count > 0 ? memory::load_and_swap<uint32_t>(cd_p) : 0;
    const uint32_t d1 = count > 1 ? memory::load_and_swap<uint32_t>(cd_p + 4) : 0;
    cd_tile_log.ops.push_back(fmt::format("cd tile op: {:02X} n {} pred {} at {:08X}: {:08X} {:08X}", opcode,
                                          count, packet & 1, reader->read_ptr() - 4, d0, d1));
  }
''')

rep("  if (tiling_draw) {\n    ++GpuLane::Get().tiling_draws;\n",
    r'''  if (tiling_draw) {
    ++GpuLane::Get().tiling_draws;
    if (cd_tile_log.frames_written < 2) {  // [cd] (fw_tiling_log.py)
      const RegisterFile& tr = *register_file_;
      const auto wo = tr.Get<reg::PA_SC_WINDOW_OFFSET>();
      const auto di = tr.Get<reg::RB_DEPTH_INFO>();
      const auto c0 = tr.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]);
      const auto c1 = tr.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[1]);
      const auto si = tr.Get<reg::RB_SURFACE_INFO>();
      const auto mc = tr.Get<reg::RB_MODECONTROL>();
      const auto dc = tr.Get<reg::RB_DEPTHCONTROL>();
      const auto tl = tr.Get<reg::PA_SC_WINDOW_SCISSOR_TL>();
      const auto br = tr.Get<reg::PA_SC_WINDOW_SCISSOR_BR>();
      const uint32_t cmask = tr.Get<reg::RB_COLOR_MASK>().value;
      const uint64_t key = (bin_select_ ^ (uint64_t(wo.value) << 1) ^ (uint64_t(di.value) << 7) ^
                                                     (uint64_t(c0.value) << 13) ^ (uint64_t(c1.value) << 19) ^
                                                     (uint64_t(si.value) << 23) ^ (uint64_t(mc.value) << 29) ^
                                                     (uint64_t(tl.value) << 31) ^ (uint64_t(br.value) << 37) ^
                                                     (uint64_t(cmask) << 41) ^ (uint64_t(dc.z_enable) << 47) ^
                                                     (uint64_t(dc.z_write_enable) << 48));
      ++cd_tile_log.frame_draws;
      if (key != cd_tile_log.key) {
        if (cd_tile_log.draws >= 500 && cd_tile_log.ops.size() < 160) {
          cd_tile_log.capture = true;
          cd_tile_log.ops.push_back(fmt::format("cd tile op: == a block of {} draws ended before {:08X}",
                                                cd_tile_log.draws, reader->read_ptr() - 4));
        }
        if (cd_tile_log.lines.size() < 200) {
          cd_tile_log.lines.push_back(fmt::format(
              "cd tile: {} draws before; select {:X} mask {:X} pred {} | offset {},{} | mode {} pitch {} msaa {} | "
              "color0 base {} fmt {} color1 base {} fmt {} mask {:X} | depth base {} fmt {} z {} write {} func {} | "
              "scissor {},{}-{},{} offset-off {} | at {:08X} viz {:08X} vizq {:08X}",
              cd_tile_log.draws, bin_select_, bin_mask_, packet & 1, int(wo.window_x_offset), int(wo.window_y_offset),
              uint32_t(mc.edram_mode), uint32_t(si.surface_pitch), uint32_t(si.msaa_samples),
              uint32_t(c0.color_base) | (uint32_t(c0.color_base_bit_11) << 11), uint32_t(c0.color_format),
              uint32_t(c1.color_base) | (uint32_t(c1.color_base_bit_11) << 11), uint32_t(c1.color_format), cmask,
              uint32_t(di.depth_base), uint32_t(di.depth_format), uint32_t(dc.z_enable),
              uint32_t(dc.z_write_enable), uint32_t(dc.zfunc), uint32_t(tl.tl_x), uint32_t(tl.tl_y),
              uint32_t(br.br_x), uint32_t(br.br_y), uint32_t(tl.window_offset_disable), reader->read_ptr() - 4,
              memory::load_and_swap<uint32_t>(memory_->TranslatePhysical(reader->read_ptr())),
              tr.Get<reg::PA_SC_VIZ_QUERY>().value));
        }
        cd_tile_log.key = key;
        cd_tile_log.draws = 0;
      }
      ++cd_tile_log.draws;
    }
''')

rep("bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,\n"
    "                                                  uint32_t count) {\n",
    r'''bool CommandProcessor::ExecutePacketType3_XE_SWAP(memory::RingBuffer* reader, uint32_t packet,
                                                  uint32_t count) {
  if (cd_tile_log.frames_written < 2) {  // [cd] (fw_tiling_log.py) a heavy frame's tile layout, written
    if (cd_tile_log.frame_draws > 3000) {
      ++cd_tile_log.frames_written;
      REXGPU_INFO("cd tile: frame of {} draws, {} lines:", cd_tile_log.frame_draws, cd_tile_log.lines.size());
      for (const std::string& line : cd_tile_log.lines) REXGPU_INFO("{}", line);
      REXGPU_INFO("cd tile: {} draws before the swap", cd_tile_log.draws);
      for (const std::string& line : cd_tile_log.ops) REXGPU_INFO("{}", line);
    }
    cd_tile_log.lines.clear();
    cd_tile_log.ops.clear();
    cd_tile_log.capture = false;
    cd_tile_log.key = ~uint64_t(0);
    cd_tile_log.draws = 0;
    cd_tile_log.frame_draws = 0;
  }
''')
cp.write_text(s, encoding="utf-8")
print("patched src/graphics/command_processor.cpp")
