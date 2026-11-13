#!/usr/bin/env python3
"""Crackdown 1/2 marker hunt, part 2 (2026-11-11). Log only, bounded like fw_marker_log.py (its 15 s capture frames).

hub-crackdown_080 showed the depth-tested overlay draws are ordinary particle shaders (oPos = world x c0..c3, no z
tricks), so a waypoint disc hidden behind nearer geometry means the DEPTH it tests against differs from the Xbox's.
In each capture frame this adds:
  1. the texture of the first draw of every depth-tested overlay state: its fetch constant in the log and its base
     level's guest bytes (up to 256 KB) in /app0/logs/cdtex_<frame>_<n>.bin ("CDTX", the 6 fetch dwords, the bytes),
     decoded on the PC (Extras/tmp/replay/cdtex.py) so the orange disc can be picked out by eye; at most 160 files
     a run, one per texture address;
  2. the order of passes and resolves: every resolve (copy control, source depth/color bases, destination, depth and
     colour clear values) and every change of depth surface or depth control between draws;
  3. on the FAST path, which host depth render target our cache bound (key: EDRAM base, pitch, MSAA, format) and how
     many ownership transfers into it the draw needed, so aliasing with the scene depth shows.

    fw_marker_log2.py <framework>      (after fw_marker_log.py)
"""
import sys
from pathlib import Path

sdk = Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"
cp = sdk / "src/graphics/command_processor.cpp"
vk = sdk / "src/graphics/vulkan/command_processor.cpp"
s = cp.read_text(encoding="utf-8")
if "cd marker seq:" in s:
    print("fw_marker_log2: already applied")
    sys.exit(0)


def rep(text, old, new, what):
    if text.count(old) != 1:
        sys.exit(f"fw_marker_log2: {what}: expected 1 of {old[:80]!r}, found {text.count(old)}")
    return text.replace(old, new)


s = rep(s, "CdMarkerLog cd_marker_log;\n}  // namespace\n",
        r'''CdMarkerLog cd_marker_log;
// [cd] (fw_marker_log2.py) passes, resolves and texture dumps of the capture frame
struct CdMarkerSeq {
  std::vector<std::string> ops;
  uint32_t last_depth_info = 0xFFFFFFFFu, last_depth_control = 0xFFFFFFFFu;
  std::vector<uint32_t> dumped;
  uint32_t files = 0;
};
CdMarkerSeq cd_marker_seq;
}  // namespace
bool cd_marker_capture_active = false;  // [cd] (fw_marker_log2.py) read by the Vulkan command processor
namespace {
}  // namespace
''', "globals")

s = rep(s, "      ++cd_marker_log.frame_draws;\n",
        r'''      ++cd_marker_log.frame_draws;
      {  // [cd] (fw_marker_log2.py) the order of passes and resolves
        const auto mc2 = mr.Get<reg::RB_MODECONTROL>();
        const uint32_t di2 = mr.values[XE_GPU_REG_RB_DEPTH_INFO];
        const uint32_t dc2 = mr.values[XE_GPU_REG_RB_DEPTHCONTROL];
        const auto wo2 = mr.Get<reg::PA_SC_WINDOW_OFFSET>();
        if (mc2.edram_mode == xenos::EdramMode::kCopy) {
          if (cd_marker_seq.ops.size() < 300)
            cd_marker_seq.ops.push_back(fmt::format(
                "resolve at draw {}: copy_control {:08X} surface {:08X} depth_info {:08X} (base {}) color_info {:08X} "
                "(base {}) dest {:08X} dest_info {:08X} depth_clear {:08X} color_clear {:08X}{:08X} offset {},{} "
                "bin {:X} pred {}",
                cd_marker_log.frame_draws, mr.values[XE_GPU_REG_RB_COPY_CONTROL],
                mr.values[XE_GPU_REG_RB_SURFACE_INFO], di2, di2 & 0xFFF, mr.values[XE_GPU_REG_RB_COLOR_INFO],
                mr.values[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF, mr.values[XE_GPU_REG_RB_COPY_DEST_BASE],
                mr.values[XE_GPU_REG_RB_COPY_DEST_INFO], mr.values[XE_GPU_REG_RB_DEPTH_CLEAR],
                mr.values[XE_GPU_REG_RB_COLOR_CLEAR], mr.values[XE_GPU_REG_RB_COLOR_CLEAR_LO],
                int(wo2.window_x_offset), int(wo2.window_y_offset), bin_select_, packet & 1));
        } else if (di2 != cd_marker_seq.last_depth_info || dc2 != cd_marker_seq.last_depth_control) {
          cd_marker_seq.last_depth_info = di2;
          cd_marker_seq.last_depth_control = dc2;
          if (cd_marker_seq.ops.size() < 300)
            cd_marker_seq.ops.push_back(fmt::format(
                "pass at draw {}: surface {:08X} depth_info {:08X} (base {}) depthcontrol {:08X} (z {} write {} "
                "func {}) color_info {:08X} (base {}) mode {} offset {},{} bin {:X} pred {} vs {:016X} ps {:016X}",
                cd_marker_log.frame_draws, mr.values[XE_GPU_REG_RB_SURFACE_INFO], di2, di2 & 0xFFF, dc2,
                (dc2 >> 1) & 1, (dc2 >> 2) & 1, (dc2 >> 4) & 7, mr.values[XE_GPU_REG_RB_COLOR_INFO],
                mr.values[XE_GPU_REG_RB_COLOR_INFO] & 0xFFF, uint32_t(mc2.edram_mode),
                int(wo2.window_x_offset), int(wo2.window_y_offset), bin_select_, packet & 1,
                active_vertex_shader_ ? active_vertex_shader_->ucode_data_hash() : 0,
                active_pixel_shader_ ? active_pixel_shader_->ucode_data_hash() : 0));
        }
      }
''', "sequence")

s = rep(s, "          cd_marker_log.keys.push_back(key);\n",
        r'''          cd_marker_log.keys.push_back(key);
          if (dc.z_enable && tex0 && cd_marker_seq.files < 160 &&
              std::find(cd_marker_seq.dumped.begin(), cd_marker_seq.dumped.end(), tex0) ==
                  cd_marker_seq.dumped.end()) {  // [cd] (fw_marker_log2.py) the texture, for the PC
            cd_marker_seq.dumped.push_back(tex0);
            uint32_t fetch[6];
            for (uint32_t fi = 0; fi < 6; ++fi) fetch[fi] = mr.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fi];
            const uint32_t len = std::min<uint32_t>(256u * 1024u, 0x20000000u - std::min(tex0, 0x20000000u));
            const uint8_t* src = len ? memory_->TranslatePhysical(tex0) : nullptr;
            const std::string name = fmt::format("/app0/logs/cdtex_{}_{}.bin", cd_marker_log.frames + 1,
                                                 cd_marker_seq.files);
            if (src) {
              if (FILE* tf = std::fopen(name.c_str(), "wb")) {
                std::fwrite("CDTX", 1, 4, tf);
                std::fwrite(fetch, 4, 6, tf);
                std::fwrite(src, 1, len, tf);
                std::fclose(tf);
                ++cd_marker_seq.files;
                if (cd_marker_seq.ops.size() < 300)
                  cd_marker_seq.ops.push_back(fmt::format(
                      "texture at draw {}: tex0 {:08X} fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} -> {} | vs "
                      "{:016X} ps {:016X} depth base {} func {}",
                      cd_marker_log.frame_draws, tex0, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5],
                      name, vs, ps, uint32_t(di.depth_base), uint32_t(dc.zfunc)));
              }
            }
          }
''', "texture dump")

s = rep(s, "      cd_marker_log.lines.clear();\n",
        r'''      cd_marker_log.lines.clear();
      for (const std::string& op : cd_marker_seq.ops) REXGPU_INFO("cd marker seq: {}", op);  // [cd] (fw_marker_log2.py)
      cd_marker_seq.ops.clear();
      cd_marker_seq.last_depth_info = cd_marker_seq.last_depth_control = 0xFFFFFFFFu;
      cd_marker_capture_active = false;
''', "frame end")

s = rep(s, "      cd_marker_log.capture = true;\n",
        "      cd_marker_log.capture = true;\n      cd_marker_capture_active = true;  // [cd] (fw_marker_log2.py)\n",
        "arm")
if "#include <algorithm>" not in s:
    s = "#include <algorithm>\n" + s
cp.write_text(s, encoding="utf-8")
print("patched src/graphics/command_processor.cpp (cd marker seq / textures)")

hdr = sdk / "include/rex/graphics/pipeline/render_target/cache.h"
h = hdr.read_text(encoding="utf-8")
if "CdLastDepthKey" not in h:
    h = rep(h, "  virtual Path GetPath() const = 0;\n",
            "  virtual Path GetPath() const = 0;\n"
            "  // [cd] (fw_marker_log2.py) the host depth target and transfers of the last update (host RT path)\n"
            "  uint32_t CdLastDepthKey() const;\n"
            "  size_t CdLastDepthTransfers() const;\n", "cache.h decl")
    hdr.write_text(h, encoding="utf-8")
cc = sdk / "src/graphics/pipeline/render_target/cache.cpp"
c = cc.read_text(encoding="utf-8")
if "CdLastDepthKey" not in c:
    c = c.rstrip("\n") + "\n" + (
        "namespace rex::graphics {  // [cd] (fw_marker_log2.py)\n"
        "uint32_t RenderTargetCache::CdLastDepthKey() const {\n"
        "  return GetPath() == Path::kHostRenderTargets && last_update_accumulated_render_targets_[0]\n"
        "             ? last_update_accumulated_render_targets_[0]->key().key\n"
        "             : 0u;\n"
        "}\n"
        "size_t RenderTargetCache::CdLastDepthTransfers() const {\n"
        "  return GetPath() == Path::kHostRenderTargets ? last_update_transfers_[0].size() : 0u;\n"
        "}\n"
        "}  // namespace rex::graphics\n")
    cc.write_text(c, encoding="utf-8")
    print("patched render target cache (CdLastDepthKey)")

v = vk.read_text(encoding="utf-8")
if "cd marker host depth" not in v:
    v = rep(v, "namespace rex::graphics::vulkan {\n",
            "namespace rex::graphics {\nextern bool cd_marker_capture_active;  // [cd] (fw_marker_log2.py)\n}\n"
            "namespace rex::graphics::vulkan {\n", "vk extern")
    v = rep(v, '''  render_target_cache_->SetWindowOffsetTiles(0);
  if (!render_targets_updated) {
    return draw_fail("render_target_update");
  }
''', '''  render_target_cache_->SetWindowOffsetTiles(0);
  if (!render_targets_updated) {
    return draw_fail("render_target_update");
  }
  if (cd_marker_capture_active &&
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    // [cd] (fw_marker_log2.py) which host depth target the draw got, and the transfers into it
    static uint32_t cd_last_key = 0xFFFFFFFFu, cd_host_lines = 0;
    const uint32_t cd_key = render_target_cache_->CdLastDepthKey();
    if (cd_key != cd_last_key && cd_host_lines < 400) {
      cd_last_key = cd_key;
      ++cd_host_lines;
      REXGPU_INFO("cd marker host depth: key {:08X} base {} pitch {} msaa {} depth {} format {} | transfers in {} | "
                  "guest depth_info {:08X} depthcontrol {:08X}",
                  cd_key, cd_key & 0x7FFu, (cd_key >> 11) & 0xFFu, (cd_key >> 19) & 3u, (cd_key >> 21) & 1u,
                  (cd_key >> 22) & 0xFu, render_target_cache_->CdLastDepthTransfers(),
                  regs.values[XE_GPU_REG_RB_DEPTH_INFO], regs.values[XE_GPU_REG_RB_DEPTHCONTROL]);
    }
  }
''', "vk host depth")
    vk.write_text(v, encoding="utf-8")
    print("patched src/graphics/vulkan/command_processor.cpp (cd marker host depth)")
