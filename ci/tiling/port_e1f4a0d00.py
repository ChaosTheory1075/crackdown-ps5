#!/usr/bin/env python3
"""Xenia Canary e1f4a0d00 (2026-10-02) "Put tiling window offsets in EDRAM bases", ported to the rexglue framework
(Vulkan + SPIR-V; the D3D12/DXBC parts are left out, the PS5 has no D3D12). Applied by ci/fw_tiling_fixes.py.

Behind cd_window_offset_in_edram (default false; toml: cd_window_offset_in_edram = true). With it off every new path
is skipped (GetWindowOffsetTiles returns 0) and the draws are the same as before, except: transfer/dump shader
address math is now unsigned-wrapped (same results), and the SPIR-V system constants gain param_gen_window_offset
(0 when off). The SPIR-V translator version is NOT bumped, so the pipeline cache on the console stays valid; shaders
already in it lack only the PsParamGen offset add, which matters with the cvar on, for PsParamGen shaders only.

Differences from Canary, where our framework is older:
- RenderTargetCache::Update keeps its virtual signature (the D3D12 override is still compiled): the Vulkan command
  processor sets the tile offset with SetWindowOffsetTiles() before Update.
- GetHostViewportInfo takes registers, not GetViewportInfoArgs: a trailing window_offset_in_edram argument leaves
  PA_SC_WINDOW_OFFSET out of the vertex offset. GetScissor and EstimateMaxY get trailing defaulted arguments.
- Our Update has no keep-aliased-depth / native-scale code, so those hunks have nothing to change.

    port_e1f4a0d00.py <rexglue-sdk-main>
"""
import pathlib
import sys

ROOT = pathlib.Path(sys.argv[1])


def edit(rel, old, new, count=1):
    path = ROOT / rel
    text = path.read_text()
    if text.count(old) != count:
        sys.exit(f"port_e1f4a0d00: expected {count} of {old[:90]!r} in {rel}, found {text.count(old)}")
    path.write_text(text.replace(old, new))


DRAW_H = "include/rex/graphics/util/draw.h"
DRAW = "src/graphics/util/draw.cpp"
EST_H = "include/rex/graphics/util/draw_extent_estimator.h"
EST = "src/graphics/util/draw_extent_estimator.cpp"
RTC_H = "include/rex/graphics/pipeline/render_target/cache.h"
RTC = "src/graphics/pipeline/render_target/cache.cpp"
VRTC_H = "include/rex/graphics/vulkan/render_target_cache.h"
VRTC = "src/graphics/vulkan/render_target_cache.cpp"
VCP_H = "include/rex/graphics/vulkan/command_processor.h"
VCP = "src/graphics/vulkan/command_processor.cpp"
SPV_H = "include/rex/graphics/pipeline/shader/spirv_translator.h"
SPV = "src/graphics/pipeline/shader/spirv_translator.cpp"

# ---- draw_util ----
edit(DRAW, 'REXCVAR_DEFINE_BOOL(half_pixel_offset, true, "GPU", "Enable half pixel offset");\n',
     'REXCVAR_DEFINE_BOOL(half_pixel_offset, true, "GPU", "Enable half pixel offset");\n'
     'REXCVAR_DEFINE_BOOL(cd_window_offset_in_edram, false, "GPU",  // [cd] Xenia Canary e1f4a0d00\n'
     '                    "Predicated tiling: whole-tile negative window offsets go into the EDRAM bases, so each "\n'
     '                    "tile rasterizes like the full-screen depth prepass it tests against (depth garbage in "\n'
     '                    "tiles); FAST keeps sideways offsets in the viewport");\n')
# GetHostViewportInfo: trailing window_offset_in_edram
edit(DRAW_H, "void GetHostViewportInfo(const RegisterFile& regs, uint32_t draw_resolution_scale_x,",
     "void GetHostViewportInfo(const RegisterFile& regs, uint32_t draw_resolution_scale_x,")
h = (ROOT / DRAW_H).read_text()
i = h.index("void GetHostViewportInfo(const RegisterFile& regs, uint32_t draw_resolution_scale_x,")
j = h.index("ViewportInfo& viewport_info_out);", i) + len("ViewportInfo& viewport_info_out")
(ROOT / DRAW_H).write_text(h[:j] + ",\n                         bool window_offset_in_edram = false" + h[j:])
edit(DRAW, "                         bool full_float24_in_0_to_1, bool pixel_shader_writes_depth,\n"
           "                         ViewportInfo& viewport_info_out) {\n",
     "                         bool full_float24_in_0_to_1, bool pixel_shader_writes_depth,\n"
     "                         ViewportInfo& viewport_info_out, bool window_offset_in_edram) {\n")
edit(DRAW, "  float offset_add_xy[2] = {};\n  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {\n",
     "  float offset_add_xy[2] = {};\n"
     "  // [cd] (e1f4a0d00) with the offset in the EDRAM bases the vertices stay at window offset 0\n"
     "  if (pa_su_sc_mode_cntl.vtx_window_offset_enable && !window_offset_in_edram) {\n")
# GetScissor
edit(DRAW_H, "void GetScissor(const RegisterFile& regs, Scissor& scissor_out, bool clamp_to_surface_pitch = true);\n",
     "void GetScissor(const RegisterFile& regs, Scissor& scissor_out, bool clamp_to_surface_pitch = true,\n"
     "                bool window_offset_in_edram = false);\n\n"
     "// [cd] (Canary e1f4a0d00) The window offset in 32bpp tiles, doubled for 64bpp color, when it can be moved\n"
     "// into the render target bases: whole-tile negative offsets only; on host render targets x offsets stay in the\n"
     "// viewport (the target is only the pitch wide). kept_in_viewport_out: the offset could be moved but has to stay\n"
     "// in the viewport (RenderTargetCache keeps the tile's later draws on the same path).\n"
     "int32_t GetWindowOffsetTiles(const RegisterFile& regs, reg::RB_DEPTHCONTROL normalized_depth_control,\n"
     "                             uint32_t normalized_color_mask, bool host_render_targets_used,\n"
     "                             bool& kept_in_viewport_out);\n"
     "// The base with the GetWindowOffsetTiles offset added and wrapped.\n"
     "inline uint32_t AddWindowOffsetToEdramBase(uint32_t base_tiles, int32_t window_offset_tiles, bool is_64bpp) {\n"
     "  return uint32_t(int32_t(base_tiles) + window_offset_tiles * (is_64bpp ? 2 : 1)) &\n"
     "         (xenos::kEdramTileCount - 1);\n"
     "}\n")
edit(DRAW, "void GetScissor(const RegisterFile& regs, Scissor& scissor_out, bool clamp_to_surface_pitch) {\n",
     "void GetScissor(const RegisterFile& regs, Scissor& scissor_out, bool clamp_to_surface_pitch,\n"
     "                bool window_offset_in_edram) {\n")
edit(DRAW, "  if (!pa_sc_window_scissor_tl.window_offset_disable) {\n"
           "    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();\n"
           "    tl_x += pa_sc_window_offset.window_x_offset;\n"
           "    tl_y += pa_sc_window_offset.window_y_offset;\n"
           "    br_x += pa_sc_window_offset.window_x_offset;\n"
           "    br_y += pa_sc_window_offset.window_y_offset;\n"
           "  }\n",
     "  int32_t window_x_offset_applied = 0, window_y_offset_applied = 0;  // [cd] (e1f4a0d00)\n"
     "  if (!pa_sc_window_scissor_tl.window_offset_disable) {\n"
     "    auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();\n"
     "    window_x_offset_applied = pa_sc_window_offset.window_x_offset;\n"
     "    window_y_offset_applied = pa_sc_window_offset.window_y_offset;\n"
     "    tl_x += window_x_offset_applied;\n"
     "    tl_y += window_y_offset_applied;\n"
     "    br_x += window_x_offset_applied;\n"
     "    br_y += window_y_offset_applied;\n"
     "  }\n")
edit(DRAW, "  br_x = std::max(br_x, tl_x);\n  br_y = std::max(br_y, tl_y);\n  scissor_out.offset[0] = uint32_t(tl_x);\n",
     "  br_x = std::max(br_x, tl_x);\n  br_y = std::max(br_y, tl_y);\n"
     "  if (window_offset_in_edram) {\n"
     "    // [cd] (e1f4a0d00) move the region back over the vertices at window offset 0\n"
     "    tl_x -= window_x_offset_applied;\n"
     "    tl_y -= window_y_offset_applied;\n"
     "    br_x -= window_x_offset_applied;\n"
     "    br_y -= window_y_offset_applied;\n"
     "  }\n"
     "  scissor_out.offset[0] = uint32_t(tl_x);\n")
edit(DRAW, "uint32_t GetNormalizedColorMask(const RegisterFile& regs,\n",
     """int32_t GetWindowOffsetTiles(const RegisterFile& regs, reg::RB_DEPTHCONTROL normalized_depth_control,
                             uint32_t normalized_color_mask, bool host_render_targets_used,
                             bool& kept_in_viewport_out) {
  // [cd] Xenia Canary e1f4a0d00
  kept_in_viewport_out = false;
  if (!REXCVAR_GET(cd_window_offset_in_edram)) {
    return 0;
  }
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  int32_t window_x_offset = pa_sc_window_offset.window_x_offset;
  int32_t window_y_offset = pa_sc_window_offset.window_y_offset;
  // Only negative offsets go into the bases, tiling moves a tile's region up to the base with them.
  if (!regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable || window_x_offset > 0 ||
      window_y_offset > 0 || !(window_x_offset | window_y_offset)) {
    return 0;
  }
  bool depth_used = normalized_depth_control.z_enable || normalized_depth_control.stencil_enable;
  if (!depth_used && !normalized_color_mask) {
    return 0;
  }
  // From here on a 0 keeps the window offset in the viewport.
  kept_in_viewport_out = true;
  // Keep x offsets in the viewport on the host render target path since the render target is only the pitch wide.
  if (window_x_offset && host_render_targets_used) {
    return 0;
  }
  // A window scissor with window_offset_disable set can't follow the vertices to offset 0.
  if (regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable) {
    return 0;
  }
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch_pixels = rb_surface_info.surface_pitch;
  if (!pitch_pixels) {
    return 0;
  }
  uint32_t msaa_samples_x_log2 = uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X);
  uint32_t msaa_samples_y_log2 = uint32_t(rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X);
  int32_t tile_width_pixels = int32_t(xenos::kEdramTileWidthSamples >> msaa_samples_x_log2);
  int32_t tile_height_pixels = int32_t(xenos::kEdramTileHeightSamples >> msaa_samples_y_log2);
  if (window_x_offset % tile_width_pixels || window_y_offset % tile_height_pixels) {
    return 0;
  }
  uint32_t pitch_tiles_at_32bpp =
      ((pitch_pixels << msaa_samples_x_log2) + (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  int32_t window_offset_tiles = (window_y_offset / tile_height_pixels) * int32_t(pitch_tiles_at_32bpp) +
                                window_x_offset / tile_width_pixels;
  if (host_render_targets_used) {
    // Keep the offset in the viewport when the rows at offset 0 reach past the host render target, it holds one
    // EDRAM addressing period of rows past its base and a wrapped base can put them there.
    int32_t scissor_bottom = std::min(int32_t(regs.Get<reg::PA_SC_WINDOW_SCISSOR_BR>().br_y),
                                      int32_t(regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>().br_y) - window_y_offset);
    uint32_t rows_end_tiles_at_32bpp =
        uint32_t((scissor_bottom + tile_height_pixels - 1) / tile_height_pixels) * pitch_tiles_at_32bpp;
    if (rows_end_tiles_at_32bpp > xenos::kEdramTileCount) {
      return 0;
    }
    // Keep the offset in the viewport when 64bpp rows cross into the next period, the draw is bound to the period
    // its scissor top is in.
    uint32_t rows_end_tiles_at_64bpp = rows_end_tiles_at_32bpp * 2;
    if (rows_end_tiles_at_64bpp > xenos::kEdramTileCount) {
      int32_t scissor_top = std::max(int32_t(regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().tl_y),
                                     int32_t(regs.Get<reg::PA_SC_SCREEN_SCISSOR_TL>().tl_y) - window_y_offset);
      uint32_t rows_start_tiles_at_64bpp =
          uint32_t(scissor_top / tile_height_pixels) * pitch_tiles_at_32bpp * 2;
      if (rows_end_tiles_at_64bpp > xenos::kEdramTileCount * 2 ||
          rows_start_tiles_at_64bpp < xenos::kEdramTileCount) {
        for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
          if (!(normalized_color_mask & (uint32_t(0b1111) << (4 * i)))) {
            continue;
          }
          if (xenos::IsColorRenderTargetFormat64bpp(
                  regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_format)) {
            return 0;
          }
        }
      }
    }
  }
  kept_in_viewport_out = false;
  return window_offset_tiles;
}

uint32_t GetNormalizedColorMask(const RegisterFile& regs,
""")

# ---- draw extent estimator ----
edit(EST_H, "  uint32_t EstimateVertexMaxY(const Shader& vertex_shader);\n"
            "  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y, const Shader& vertex_shader);\n",
     "  uint32_t EstimateVertexMaxY(const Shader& vertex_shader, bool window_offset_in_edram = false);\n"
     "  uint32_t EstimateMaxY(bool try_to_estimate_vertex_max_y, const Shader& vertex_shader,\n"
     "                        bool window_offset_in_edram = false);\n")
edit(EST, "uint32_t DrawExtentEstimator::EstimateVertexMaxY(const Shader& vertex_shader) {\n",
     "uint32_t DrawExtentEstimator::EstimateVertexMaxY(const Shader& vertex_shader,\n"
     "                                                 bool window_offset_in_edram) {\n")
edit(EST, "  if (pa_su_sc_mode_cntl.vtx_window_offset_enable) {\n",
     "  if (pa_su_sc_mode_cntl.vtx_window_offset_enable && !window_offset_in_edram) {  // [cd] (e1f4a0d00)\n")
e = (ROOT / EST).read_text()
i = e.index("uint32_t DrawExtentEstimator::EstimateMaxY(bool try_to_estimate_vertex_max_y,")
j = e.index("{", i)
sig = e[i:j]
(ROOT / EST).write_text(e[:i] + sig.rstrip().rstrip(")") + ",\n                                           bool window_offset_in_edram) " + e[j:])
edit(EST, "  bool scissor_window_offset = !regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable;\n",
     "  bool scissor_window_offset =\n"
     "      !regs.Get<reg::PA_SC_WINDOW_SCISSOR_TL>().window_offset_disable && !window_offset_in_edram;\n"
     "  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();\n"
     "  int32_t screen_scissor_bottom = int32_t(pa_sc_screen_scissor_br.br_y);\n")
edit(EST, "    scissor_bottom += window_y_offset;\n  }\n"
          "  auto pa_sc_screen_scissor_br = regs.Get<reg::PA_SC_SCREEN_SCISSOR_BR>();\n"
          "  scissor_bottom = std::min(scissor_bottom, int32_t(pa_sc_screen_scissor_br.br_y));\n",
     "    scissor_bottom += window_y_offset;\n"
     "  } else if (window_offset_in_edram) {\n"
     "    screen_scissor_bottom -= window_y_offset;  // [cd] (e1f4a0d00)\n"
     "  }\n"
     "  scissor_bottom = std::min(scissor_bottom, screen_scissor_bottom);\n")
edit(EST, "          int32_t scissor_right = int32_t(pa_sc_window_scissor_br.br_x);\n",
     "          int32_t scissor_right = int32_t(pa_sc_window_scissor_br.br_x);\n"
     "          int32_t screen_scissor_right = int32_t(pa_sc_screen_scissor_br.br_x);\n")
edit(EST, "            scissor_right += pa_sc_window_offset.window_x_offset;\n          }\n"
          "          scissor_right = std::min(scissor_right, int32_t(pa_sc_screen_scissor_br.br_x));\n",
     "            scissor_right += pa_sc_window_offset.window_x_offset;\n"
     "          } else if (window_offset_in_edram) {\n"
     "            screen_scissor_right -= pa_sc_window_offset.window_x_offset;  // [cd] (e1f4a0d00)\n"
     "          }\n"
     "          scissor_right = std::min(scissor_right, screen_scissor_right);\n")
edit(EST, "        max_y = std::min(max_y, EstimateVertexMaxY(vertex_shader));\n",
     "        max_y = std::min(max_y, EstimateVertexMaxY(vertex_shader, window_offset_in_edram));\n")
edit(EST, "    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable) {\n      viewport_bottom += float(window_y_offset);\n",
     "    if (regs.Get<reg::PA_SU_SC_MODE_CNTL>().vtx_window_offset_enable && !window_offset_in_edram) {\n"
     "      viewport_bottom += float(window_y_offset);\n")

# ---- render target cache (shared) ----
edit(RTC_H, "  virtual void BeginFrame();\n",
     "  virtual void BeginFrame();\n\n"
     "  // [cd] (Canary e1f4a0d00) Keeps each tile on one window offset path: if a draw has to leave its offset in the\n"
     "  // viewport, later draws with the same offset, surface pitch and MSAA do the same until a frame passes without\n"
     "  // one (a tiled depth prepass and the passes using it stay on one transform).\n"
     "  int32_t GetWindowOffsetTiles(const RegisterFile& regs, reg::RB_DEPTHCONTROL normalized_depth_control,\n"
     "                               uint32_t normalized_color_mask, uint64_t frame);\n"
     "  // [cd] The offset the next Update puts in the EDRAM bases (the command processor sets it before Update and\n"
     "  // clears it after; Update keeps its virtual signature for the D3D12 override).\n"
     "  void SetWindowOffsetTiles(int32_t window_offset_tiles) { window_offset_tiles_ = window_offset_tiles; }\n")
edit(RTC_H, "      uint32_t resource_format : xenos::kRenderTargetFormatBits;  // 26\n    };\n",
     "      uint32_t resource_format : xenos::kRenderTargetFormatBits;  // 26\n"
     "      // [cd] (e1f4a0d00) The rows are in the next EDRAM addressing period past the base (64bpp color rows\n"
     "      // span two); the rows in the first period at the same base are another render target.\n"
     "      uint32_t next_period : 1;  // 27\n"
     "    };\n")
edit(RTC_H, "    uint32_t GetPitchTiles() const { return pitch_tiles_at_32bpp << uint32_t(Is64bpp()); }\n",
     "    uint32_t GetPitchTiles() const { return pitch_tiles_at_32bpp << uint32_t(Is64bpp()); }\n"
     "    // [cd] (e1f4a0d00) The tile index, past the tile count for the tail wrapped around the end of EDRAM and again\n"
     "    // for the next period; minus the base it's the offset into the render target's rows.\n"
     "    uint32_t GetNonWrappedTileIndex(uint32_t tile_index) const {\n"
     "      return tile_index + (tile_index < base_tiles ? xenos::kEdramTileCount : 0) +\n"
     "             (uint32_t(next_period) << xenos::kEdramBaseTilesBits);\n"
     "    }\n")
edit(RTC_H, "      return fmt::format(\"RT @ {}t, <{}t>, {}xMSAA, {}\", base_tiles, GetPitchTiles(),\n"
            "                         uint32_t(1) << uint32_t(msaa_samples), GetFormatName());\n",
     "      return fmt::format(\"RT @ {}t, <{}t>, {}xMSAA, {}{}\", base_tiles, GetPitchTiles(),\n"
     "                         uint32_t(1) << uint32_t(msaa_samples), GetFormatName(),\n"
     "                         next_period ? \", next period\" : \"\");\n")
r = (ROOT / RTC_H).read_text()
i = r.index("    static uint32_t GetRangeRectangles(uint32_t start_tiles, uint32_t end_tiles,")
j = r.index("    bool AreSourcesSame", i)
r = r[:i] + ("    static uint32_t GetRangeRectangles(uint32_t start_tiles, uint32_t end_tiles, RenderTargetKey key,\n"
             "                                       Rectangle* rectangles_out, const Rectangle* cutout = nullptr);\n"
             "    uint32_t GetRectangles(RenderTargetKey key, Rectangle* rectangles_out,\n"
             "                           const Rectangle* cutout = nullptr) const {\n"
             "      return GetRangeRectangles(start_tiles, end_tiles, key, rectangles_out, cutout);\n"
             "    }\n") + r[j:]
(ROOT / RTC_H).write_text(r)
edit(RTC_H, "  std::map<uint32_t, OwnershipRange> ownership_ranges_;\n",
     "  std::map<uint32_t, OwnershipRange> ownership_ranges_;\n"
     "  // [cd] (e1f4a0d00) PA_SC_WINDOW_OFFSET of the last interlock draw with the offset in the EDRAM bases, 0 for one\n"
     "  // with it in the viewport, UINT32_MAX after a full barrier.\n"
     "  uint32_t interlock_last_window_offset_ = UINT32_MAX;\n"
     "  // [cd] Tiles with the window offset kept in the viewport, with the last frame a draw was at them.\n"
     "  struct WindowOffsetKeptInViewport {\n"
     "    uint32_t window_offset;\n"
     "    uint32_t surface_pitch;\n"
     "    xenos::MsaaSamples msaa_samples;\n"
     "    uint64_t frame;\n"
     "  };\n"
     "  std::vector<WindowOffsetKeptInViewport> window_offsets_kept_in_viewport_;\n"
     "  int32_t window_offset_tiles_ = 0;  // [cd] (SetWindowOffsetTiles)\n")

c = (ROOT / RTC).read_text()
i = c.index("uint32_t RenderTargetCache::Transfer::GetRangeRectangles(uint32_t start_tiles, uint32_t end_tiles,")
j = c.index("const Rectangle* cutout) {", i) + len("const Rectangle* cutout) {")
c = c[:i] + ("uint32_t RenderTargetCache::Transfer::GetRangeRectangles(uint32_t start_tiles, uint32_t end_tiles,\n"
             "                                                         RenderTargetKey key, Rectangle* rectangles_out,\n"
             "                                                         const Rectangle* cutout) {\n"
             "  uint32_t base_tiles = key.base_tiles;\n"
             "  uint32_t pitch_tiles = key.GetPitchTiles();\n"
             "  xenos::MsaaSamples msaa_samples = key.msaa_samples;\n"
             "  bool is_64bpp = key.Is64bpp();") + c[j:]
(ROOT / RTC).write_text(c)
edit(RTC, "  uint32_t local_offset = start_tiles < base_tiles ? xenos::kEdramTileCount : 0;\n"
          "  uint32_t local_start = local_offset + start_tiles - base_tiles;\n"
          "  uint32_t local_end = local_offset + end_tiles - base_tiles;\n",
     "  // [cd] (e1f4a0d00) and the next period's render target holds its rows a period further\n"
     "  uint32_t local_start = key.GetNonWrappedTileIndex(start_tiles) - base_tiles;\n"
     "  uint32_t local_end = local_start + (end_tiles - start_tiles);\n")
edit(RTC, "void RenderTargetCache::BeginFrame() {\n  ResetAccumulatedRenderTargets();\n}\n",
     """void RenderTargetCache::BeginFrame() {
  ResetAccumulatedRenderTargets();
}

int32_t RenderTargetCache::GetWindowOffsetTiles(const RegisterFile& regs,
                                                reg::RB_DEPTHCONTROL normalized_depth_control,
                                                uint32_t normalized_color_mask, uint64_t frame) {
  // [cd] Xenia Canary e1f4a0d00
  bool kept_in_viewport;
  int32_t window_offset_tiles =
      draw_util::GetWindowOffsetTiles(regs, normalized_depth_control, normalized_color_mask,
                                      GetPath() == Path::kHostRenderTargets, kept_in_viewport);
  if (!window_offset_tiles && !kept_in_viewport) {
    return 0;
  }
  window_offsets_kept_in_viewport_.erase(
      std::remove_if(window_offsets_kept_in_viewport_.begin(), window_offsets_kept_in_viewport_.end(),
                     [frame](const WindowOffsetKeptInViewport& kept) { return kept.frame + 1 < frame; }),
      window_offsets_kept_in_viewport_.end());
  auto pa_sc_window_offset = regs.Get<reg::PA_SC_WINDOW_OFFSET>();
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto kept_it = std::find_if(window_offsets_kept_in_viewport_.begin(), window_offsets_kept_in_viewport_.end(),
                              [&](const WindowOffsetKeptInViewport& kept) {
                                return kept.window_offset == pa_sc_window_offset.value &&
                                       kept.surface_pitch == rb_surface_info.surface_pitch &&
                                       kept.msaa_samples == rb_surface_info.msaa_samples;
                              });
  if (kept_it != window_offsets_kept_in_viewport_.end()) {
    // Refresh on every draw of the tile, so a reason that comes and goes doesn't flip the tile between the paths.
    kept_it->frame = frame;
    return 0;
  }
  if (kept_in_viewport) {
    window_offsets_kept_in_viewport_.push_back(
        {pa_sc_window_offset.value, rb_surface_info.surface_pitch, rb_surface_info.msaa_samples, frame});
    return 0;
  }
  return window_offset_tiles;
}
""")
edit(RTC, "  const RegisterFile& regs = register_file();\n  bool interlock_barrier_only = GetPath() == Path::kPixelShaderInterlock;\n\n"
          "  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();\n",
     "  const RegisterFile& regs = register_file();\n  bool interlock_barrier_only = GetPath() == Path::kPixelShaderInterlock;\n"
     "  const int32_t window_offset_tiles = window_offset_tiles_;  // [cd] (e1f4a0d00, SetWindowOffsetTiles)\n\n"
     "  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();\n")
edit(RTC, "  uint32_t edram_bases[1 + xenos::kMaxColorRenderTargets];\n  uint32_t resource_formats[1 + xenos::kMaxColorRenderTargets];\n",
     "  uint32_t edram_bases[1 + xenos::kMaxColorRenderTargets];\n"
     "  // [cd] (e1f4a0d00) Guest bases for the aliasing tests: the window offset in the bases moves 64bpp ones twice\n"
     "  // as far and can bring render targets that don't overlap to one base.\n"
     "  uint32_t guest_bases[1 + xenos::kMaxColorRenderTargets];\n"
     "  uint32_t resource_formats[1 + xenos::kMaxColorRenderTargets];\n")
edit(RTC, "      edram_bases[0] = rb_depth_info.depth_base;\n",
     "      edram_bases[0] =\n"
     "          draw_util::AddWindowOffsetToEdramBase(rb_depth_info.depth_base, window_offset_tiles, false);\n"
     "      guest_bases[0] = rb_depth_info.depth_base;\n")
edit(RTC, "      edram_bases[rt_bit_index] = color_info.color_base;\n"
          "      xenos::ColorRenderTargetFormat color_format =\n"
          "          regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]).color_format;\n"
          "      bool is_64bpp = xenos::IsColorRenderTargetFormat64bpp(color_format);\n",
     "      xenos::ColorRenderTargetFormat color_format = color_info.color_format;\n"
     "      bool is_64bpp = xenos::IsColorRenderTargetFormat64bpp(color_format);\n"
     "      edram_bases[rt_bit_index] =\n"
     "          draw_util::AddWindowOffsetToEdramBase(color_info.color_base, window_offset_tiles, is_64bpp);\n"
     "      guest_bases[rt_bit_index] = color_info.color_base;\n")
edit(RTC, "    uint32_t edram_base = edram_bases[rt_index];\n", "    uint32_t guest_base = guest_bases[rt_index];\n")
edit(RTC, "      if (edram_bases[rt_other_index] == edram_base) {\n", "      if (guest_bases[rt_other_index] == guest_base) {\n")
c = (ROOT / RTC).read_text()
i = c.index("  uint32_t height_used =\n      std::min(GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples),")
j = c.index("vertex_shader));\n", i) + len("vertex_shader));\n")
c = c[:i] + (
    "  uint32_t height_used = draw_extent_estimator_.EstimateMaxY(\n"
    "      interlock_barrier_only ? REXCVAR_GET(execute_unclipped_draw_vs_on_cpu_for_psi_render_backend) : true,\n"
    "      vertex_shader, window_offset_tiles != 0);\n"
    "  if (!(interlock_barrier_only && window_offset_tiles)) {\n"
    "    // [cd] (e1f4a0d00) Don't clamp an interlock range with the window offset in the bases to the period, it runs\n"
    "    // on into the next one and the shader wraps the addresses. Host render targets keep the offset in the\n"
    "    // viewport past the period.\n"
    "    height_used = std::min(height_used, GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples));\n"
    "  }\n") + c[j:]
(ROOT / RTC).write_text(c)
edit(RTC, "  if (REXCVAR_GET(mrt_edram_used_range_clamp_to_min) && edram_bases_sorted_count >= 2) {\n",
     "  if (!window_offset_tiles && REXCVAR_GET(mrt_edram_used_range_clamp_to_min) &&\n"
     "      edram_bases_sorted_count >= 2) {\n")
edit(RTC, "  uint32_t rt_lengths_tiles[1 + xenos::kMaxColorRenderTargets];\n"
          "  uint32_t length_used_tiles_at_32bpp =\n"
          "      ((height_used << uint32_t(msaa_samples >= xenos::MsaaSamples::k2X)) +\n"
          "       (xenos::kEdramTileHeightSamples - 1)) /\n"
          "      xenos::kEdramTileHeightSamples * pitch_tiles_at_32bpp;\n",
     "  uint32_t rt_lengths_tiles[1 + xenos::kMaxColorRenderTargets];\n"
     "  uint32_t rt_starts_tiles[1 + xenos::kMaxColorRenderTargets];\n"
     "  uint32_t length_used_tiles_at_32bpp =\n"
     "      ((height_used << uint32_t(msaa_samples >= xenos::MsaaSamples::k2X)) +\n"
     "       (xenos::kEdramTileHeightSamples - 1)) /\n"
     "      xenos::kEdramTileHeightSamples * pitch_tiles_at_32bpp;\n"
     "  // [cd] (e1f4a0d00) Start the ranges at the scissor with the window offset in the bases, without clamping\n"
     "  // between the bases: the draw touches nothing between a base and the scissor top. An x offset shifts the\n"
     "  // whole range by its tiles, the scissor's left tile is past the pitch in the next row and the last row runs\n"
     "  // that far past it.\n"
     "  uint32_t start_used_tiles_at_32bpp = 0;\n"
     "  if (window_offset_tiles) {\n"
     "    draw_util::Scissor scissor;\n"
     "    draw_util::GetScissor(regs, scissor, true, true);\n"
     "    start_used_tiles_at_32bpp =\n"
     "        (scissor.offset[1] << uint32_t(msaa_samples >= xenos::MsaaSamples::k2X)) /\n"
     "            xenos::kEdramTileHeightSamples * pitch_tiles_at_32bpp +\n"
     "        (scissor.offset[0] << msaa_samples_x_log2) / xenos::kEdramTileWidthSamples;\n"
     "    uint32_t right_tiles_at_32bpp =\n"
     "        (((scissor.offset[0] + scissor.extent[0]) << msaa_samples_x_log2) +\n"
     "         (xenos::kEdramTileWidthSamples - 1)) /\n"
     "        xenos::kEdramTileWidthSamples;\n"
     "    if (right_tiles_at_32bpp > pitch_tiles_at_32bpp) {\n"
     "      length_used_tiles_at_32bpp += right_tiles_at_32bpp - pitch_tiles_at_32bpp;\n"
     "    }\n"
     "  }\n")
c = (ROOT / RTC).read_text()
i = c.index("    rt_key.resource_format = resource_formats[rt_bit_index];\n    if (!interlock_barrier_only) {\n")
j = c.index("                                       rt_base);\n  }\n", i) + len("                                       rt_base);\n  }\n")
c = c[:i] + """    rt_key.resource_format = resource_formats[rt_bit_index];
    uint32_t rt_is_64bpp = (rts_are_64bpp >> rt_bit_index) & 1;
    uint32_t range_start_tiles = 0;
    uint32_t range_end_tiles = 0;
    if (window_offset_tiles) {
      // [cd] (e1f4a0d00)
      range_start_tiles = start_used_tiles_at_32bpp << rt_is_64bpp;
      range_end_tiles = length_used_tiles_at_32bpp << rt_is_64bpp;
      if (!interlock_barrier_only) {
        // Take the render target of the period the scissor top is in and end the range with it, 64bpp color rows
        // span two.
        rt_key.next_period = std::min(range_start_tiles >> xenos::kEdramBaseTilesBits, uint32_t(1));
        range_end_tiles =
            std::min(range_end_tiles, (uint32_t(rt_key.next_period) + 1) << xenos::kEdramBaseTilesBits);
      }
    }
    if (!interlock_barrier_only) {
      RenderTarget* render_target = GetOrCreateRenderTarget(rt_key);
      if (!render_target) {
        return false;
      }
      rts[rt_bit_index] = render_target;
    }
    if (window_offset_tiles) {
      // Wrap the start into the period and take at most one period from it. ChangeOwnership wraps a range crossing
      // the end of EDRAM.
      rt_starts_tiles[i] = range_start_tiles & (xenos::kEdramTileCount - 1);
      rt_lengths_tiles[i] = std::min(
          range_end_tiles > range_start_tiles ? range_end_tiles - range_start_tiles : uint32_t(0),
          xenos::kEdramTileCount);
    } else {
      rt_starts_tiles[i] = 0;
      // The last render target can occupy the EDRAM until the base of the first
      // render target (itself in case of 1 render target) with EDRAM addressing
      // wrapping.
      rt_lengths_tiles[i] = std::min(std::min(length_used_tiles_at_32bpp << rt_is_64bpp,
                                              rt_max_distance_tiles_at_64bpp >> (rt_is_64bpp ^ 1)),
                                     ((i + 1 < edram_bases_sorted_count)
                                          ? edram_bases_sorted[i + 1].first
                                          : (xenos::kEdramTileCount + edram_bases_sorted[0].first)) -
                                         rt_base);
    }
  }
""" + c[j:]
(ROOT / RTC).write_text(c)
edit(RTC, "    bool interlock_barrier_needed = false;\n"
          "    for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {\n"
          "      const std::pair<uint32_t, uint32_t>& rt_base_index = edram_bases_sorted[i];\n"
          "      if (WouldOwnershipChangeRequireTransfers(rt_keys[rt_base_index.second], 0,\n"
          "                                               rt_lengths_tiles[i])) {\n"
          "        interlock_barrier_needed = true;\n"
          "        break;\n"
          "      }\n"
          "    }\n"
          "    if (interlock_barrier_needed) {\n"
          "      RequestPixelShaderInterlockBarrier();\n"
          "    }\n",
     "    // [cd] (e1f4a0d00) Take a barrier when the offset in the bases differs from the last draw's: two pixels can\n"
     "    // reach one word under one key then, and the interlock only orders a pixel against itself.\n"
     "    uint32_t interlock_window_offset =\n"
     "        window_offset_tiles ? regs.Get<reg::PA_SC_WINDOW_OFFSET>().value : 0;\n"
     "    bool interlock_barrier_needed = interlock_last_window_offset_ != UINT32_MAX &&\n"
     "                                    interlock_window_offset != interlock_last_window_offset_;\n"
     "    if (!interlock_barrier_needed) {\n"
     "      for (uint32_t i = 0; i < edram_bases_sorted_count; ++i) {\n"
     "        const std::pair<uint32_t, uint32_t>& rt_base_index = edram_bases_sorted[i];\n"
     "        if (WouldOwnershipChangeRequireTransfers(rt_keys[rt_base_index.second], rt_starts_tiles[i],\n"
     "                                                 rt_lengths_tiles[i])) {\n"
     "          interlock_barrier_needed = true;\n"
     "          break;\n"
     "        }\n"
     "      }\n"
     "    }\n"
     "    if (interlock_barrier_needed) {\n"
     "      RequestPixelShaderInterlockBarrier();\n"
     "    }\n"
     "    interlock_last_window_offset_ = interlock_window_offset;\n")
edit(RTC, "    ChangeOwnership(rt_keys[rt_bit_index], 0, rt_lengths_tiles[i],\n",
     "    ChangeOwnership(rt_keys[rt_bit_index], rt_starts_tiles[i], rt_lengths_tiles[i],\n")
edit(RTC, "void RenderTargetCache::PixelShaderInterlockFullEdramBarrierPlaced() {\n"
          "  assert_true(GetPath() == Path::kPixelShaderInterlock);\n",
     "void RenderTargetCache::PixelShaderInterlockFullEdramBarrierPlaced() {\n"
     "  assert_true(GetPath() == Path::kPixelShaderInterlock);\n"
     "  interlock_last_window_offset_ = UINT32_MAX;  // [cd] (e1f4a0d00)\n")
edit(RTC, "  uint32_t dest_pitch_tiles = dest.GetPitchTiles();\n  bool dest_is_64bpp = dest.Is64bpp();\n", "")
edit(RTC, "              Transfer::GetRangeRectangles(it->first, transfer_end_tiles, dest.base_tiles,\n"
          "                                           dest_pitch_tiles, dest.msaa_samples, dest_is_64bpp,\n"
          "                                           nullptr, resolve_clear_cutout)) {\n",
     "              Transfer::GetRangeRectangles(it->first, transfer_end_tiles, dest, nullptr,\n"
     "                                           resolve_clear_cutout)) {\n")

# ---- Vulkan render target cache ----
edit(VRTC_H, "      int32_t source_to_dest : xenos::kEdramBaseTilesBits + 1;\n",
     "      // [cd] (e1f4a0d00) wrapped (the shader wraps the sum anyway), and whether the source render target is in\n"
     "      // the next period (64bpp color only)\n"
     "      uint32_t source_to_dest : xenos::kEdramBaseTilesBits;\n"
     "      uint32_t source_next_period : 1;\n")
edit(VRTC_H, "      uint32_t dispatch_first_tile : xenos::kEdramBaseTilesBits + 1;\n      uint32_t source_base_tiles",
     "      // [cd] (e1f4a0d00) + 2 bits: past the tile count for wrapping, and again for the next period\n"
     "      uint32_t dispatch_first_tile : xenos::kEdramBaseTilesBits + 2;\n      uint32_t source_base_tiles")
edit(VRTC, """  spv::Id source_tile_index = builder.createBinOp(
      spv::OpBitwiseAnd, type_uint,
      builder.createUnaryOp(
          spv::OpBitcast, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_int,
              builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
              builder.createTriOp(spv::OpBitFieldSExtract, type_int,
                                  builder.createUnaryOp(spv::OpBitcast, type_int, address_constant),
                                  builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                                  builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
      builder.makeUintConstant(xenos::kEdramTileCount - 1));
""", """  // [cd] (e1f4a0d00) the base difference is stored wrapped; sources in the next period hold their tiles a period
  // of rows down
  spv::Id source_tile_index = builder.createBinOp(
      spv::OpIAdd, type_uint,
      builder.createBinOp(
          spv::OpBitwiseAnd, type_uint,
          builder.createBinOp(
              spv::OpIAdd, type_uint, dest_tile_index,
              builder.createTriOp(spv::OpBitFieldUExtract, type_uint, address_constant,
                                  builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                                  builder.makeUintConstant(xenos::kEdramBaseTilesBits))),
          builder.makeUintConstant(xenos::kEdramTileCount - 1)),
      builder.createBinOp(
          spv::OpIMul, type_uint,
          builder.createTriOp(
              spv::OpBitFieldUExtract, type_uint, address_constant,
              builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2 + xenos::kEdramBaseTilesBits),
              builder.makeUintConstant(1)),
          builder.makeUintConstant(xenos::kEdramTileCount)));
""")
edit(VRTC, """            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createUnaryOp(
                    spv::OpBitcast, type_uint,
                    builder.createBinOp(
                        spv::OpIAdd, type_int,
                        builder.createUnaryOp(spv::OpBitcast, type_int, dest_tile_index),
                        builder.createTriOp(
                            spv::OpBitFieldSExtract, type_int,
                            builder.createUnaryOp(spv::OpBitcast, type_int,
                                                  host_depth_address_constant),
                            builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                            builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1)))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
""", """            // [cd] (e1f4a0d00) wrapped
            spv::Id host_depth_source_tile_index = builder.createBinOp(
                spv::OpBitwiseAnd, type_uint,
                builder.createBinOp(
                    spv::OpIAdd, type_uint, dest_tile_index,
                    builder.createTriOp(spv::OpBitFieldUExtract, type_uint, host_depth_address_constant,
                                        builder.makeUintConstant(xenos::kEdramPitchTilesBits * 2),
                                        builder.makeUintConstant(xenos::kEdramBaseTilesBits))),
                builder.makeUintConstant(xenos::kEdramTileCount - 1));
""")
edit(VRTC, "      uint32_t transfer_rectangle_count = transfer.GetRectangles(\n"
           "          dest_rt_key.base_tiles, dest_rt_key.pitch_tiles_at_32bpp, dest_rt_key.msaa_samples, false,\n"
           "          transfer_rectangles, resolve_clear_rectangle);\n",
     "      uint32_t transfer_rectangle_count =\n"
     "          transfer.GetRectangles(dest_rt_key, transfer_rectangles, resolve_clear_rectangle);\n")
edit(VRTC, "dest_rt_key.base_tiles, dest_pitch_tiles, dest_rt_key.msaa_samples, dest_is_64bpp,\n",
     "dest_rt_key,\n", count=5)
edit(VRTC, "      bool dest_is_64bpp = dest_rt_key.Is64bpp();\n", "")
edit(VRTC, "          host_depth_address_constant.source_to_dest =\n"
           "              int32_t(dest_rt_key.base_tiles) - int32_t(host_depth_source_rt_key.base_tiles);\n",
     "          host_depth_address_constant.source_to_dest =\n"
     "              (uint32_t(dest_rt_key.base_tiles) - uint32_t(host_depth_source_rt_key.base_tiles)) &\n"
     "              (xenos::kEdramTileCount - 1);\n")
edit(VRTC, "          address_constant.source_to_dest =\n"
           "              int32_t(dest_rt_key.base_tiles) - int32_t(source_rt_key.base_tiles);\n",
     "          address_constant.source_to_dest =\n"
     "              (uint32_t(dest_rt_key.base_tiles) - uint32_t(source_rt_key.base_tiles)) &\n"
     "              (xenos::kEdramTileCount - 1);\n"
     "          address_constant.source_next_period = source_rt_key.next_period;  // [cd] (e1f4a0d00)\n")
edit(VRTC, "  spv::Id const_edram_base_tiles_bits_plus_1 =\n      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 1);\n",
     "  spv::Id const_edram_base_tiles_bits_plus_2 =  // [cd] (e1f4a0d00)\n"
     "      builder.makeUintConstant(xenos::kEdramBaseTilesBits + 2);\n")
edit(VRTC, "const_edram_base_tiles_bits_plus_1", "const_edram_base_tiles_bits_plus_2", count=2)
edit(VRTC, "      offsets.dispatch_first_tile = dump_base + dispatch.offset;\n",
     "      offsets.dispatch_first_tile = rt_key.GetNonWrappedTileIndex(dump_base + dispatch.offset);  // [cd]\n")

# ---- SPIR-V translator: PsParamGen gets the offset back ----
s = (ROOT / SPV_H).read_text()
i = s.index("  struct SystemConstants {")
j = s.index("    float tessellation_factor_range_max;\n  };\n", i) + len("    float tessellation_factor_range_max;\n")
s = s[:j] + ("\n    // [cd] (Canary e1f4a0d00) PA_SC_WINDOW_OFFSET the PsParamGen position needs added when the offset is\n"
             "    // carried in the EDRAM bases rather than the viewport; 0 when it's in the viewport.\n"
             "    float param_gen_window_offset[2];\n"
             "    uint32_t param_gen_window_offset_padding[2];\n") + s[j:]
(ROOT / SPV_H).write_text(s)
edit(SPV_H, "    kSystemConstantEdramBlendConstant,\n  };\n",
     "    kSystemConstantEdramBlendConstant,\n    kSystemConstantParamGenWindowOffset,  // [cd]\n  };\n")
edit(SPV, "      {\"edram_blend_constant\", offsetof(SystemConstants, edram_blend_constant), type_float4_},\n  };\n",
     "      {\"edram_blend_constant\", offsetof(SystemConstants, edram_blend_constant), type_float4_},\n"
     "      {\"param_gen_window_offset\", offsetof(SystemConstants, param_gen_window_offset), type_float2_},\n"
     "  };\n")
edit(SPV, "    spv::Id const_sign_bit = builder_->makeUintConstant(UINT32_C(1) << 31);\n"
          "    // X - pixel X .0 in the magnitude, is back-facing in the sign bit.\n",
     "    spv::Id const_sign_bit = builder_->makeUintConstant(UINT32_C(1) << 31);\n"
     "    // [cd] (e1f4a0d00) Add the window offset back when it's carried in the EDRAM bases (PsParamGen includes it).\n"
     "    id_vector_temp_.clear();\n"
     "    id_vector_temp_.push_back(builder_->makeIntConstant(kSystemConstantParamGenWindowOffset));\n"
     "    spv::Id param_gen_window_offset = builder_->createLoad(\n"
     "        builder_->createAccessChain(spv::StorageClassUniform, uniform_system_constants_, id_vector_temp_),\n"
     "        spv::NoPrecision);\n"
     "    // X - pixel X .0 in the magnitude, is back-facing in the sign bit.\n")
edit(SPV, "    param_gen_x = builder_->createUnaryBuiltinCall(type_float_, ext_inst_glsl_std_450_,\n"
          "                                                   GLSLstd450FAbs, param_gen_x);\n",
     "    param_gen_x = builder_->createBinOp(\n"
     "        spv::OpFAdd, type_float_, param_gen_x,\n"
     "        builder_->createCompositeExtract(param_gen_window_offset, type_float_, 0));  // [cd]\n"
     "    param_gen_x = builder_->createUnaryBuiltinCall(type_float_, ext_inst_glsl_std_450_,\n"
     "                                                   GLSLstd450FAbs, param_gen_x);\n")
edit(SPV, "    param_gen_y = builder_->createUnaryBuiltinCall(type_float_, ext_inst_glsl_std_450_,\n"
          "                                                   GLSLstd450FAbs, param_gen_y);\n",
     "    param_gen_y = builder_->createBinOp(\n"
     "        spv::OpFAdd, type_float_, param_gen_y,\n"
     "        builder_->createCompositeExtract(param_gen_window_offset, type_float_, 1));  // [cd]\n"
     "    param_gen_y = builder_->createUnaryBuiltinCall(type_float_, ext_inst_glsl_std_450_,\n"
     "                                                   GLSLstd450FAbs, param_gen_y);\n")

# ---- Vulkan command processor ----
edit(VCP_H, "  void UpdateDynamicState(const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,\n"
            "                          reg::RB_DEPTHCONTROL normalized_depth_control);\n",
     "  void UpdateDynamicState(const draw_util::ViewportInfo& viewport_info, bool primitive_polygonal,\n"
     "                          reg::RB_DEPTHCONTROL normalized_depth_control,\n"
     "                          bool window_offset_in_edram = false);\n")
edit(VCP_H, "      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask);\n",
     "      reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,\n"
     "      int32_t window_offset_tiles = 0);\n")
edit(VCP, "  if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,\n"
          "                                    normalized_color_mask, *vertex_shader)) {\n"
          "    return draw_fail(\"render_target_update\");\n  }\n",
     "  // [cd] (Canary e1f4a0d00) put the window offset into the EDRAM bases when possible\n"
     "  const int32_t window_offset_tiles = render_target_cache_->GetWindowOffsetTiles(\n"
     "      regs, normalized_depth_control, normalized_color_mask, frame_current_);\n"
     "  render_target_cache_->SetWindowOffsetTiles(window_offset_tiles);\n"
     "  const bool render_targets_updated = render_target_cache_->Update(\n"
     "      is_rasterization_done, normalized_depth_control, normalized_color_mask, *vertex_shader);\n"
     "  render_target_cache_->SetWindowOffsetTiles(0);\n"
     "  if (!render_targets_updated) {\n"
     "    return draw_fail(\"render_target_update\");\n  }\n")
edit(VCP, "      host_render_targets_used, pixel_shader && pixel_shader->writes_depth(), viewport_info);\n\n"
          "  // Update dynamic graphics pipeline state.\n"
          "  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control);\n",
     "      host_render_targets_used, pixel_shader && pixel_shader->writes_depth(), viewport_info,\n"
     "      window_offset_tiles != 0);\n\n"
     "  // Update dynamic graphics pipeline state.\n"
     "  UpdateDynamicState(viewport_info, primitive_polygonal, normalized_depth_control,\n"
     "                     window_offset_tiles != 0);\n")
edit(VCP, "                             normalized_depth_control, normalized_color_mask);\n",
     "                             normalized_depth_control, normalized_color_mask, window_offset_tiles);\n")
v = (ROOT / VCP).read_text()
i = v.index("void VulkanCommandProcessor::UpdateDynamicState(const draw_util::ViewportInfo& viewport_info,")
j = v.index(") {", i)
v = v[:i] + v[i:j] + ",\n                                                bool window_offset_in_edram" + v[j:]
i = v.index("void VulkanCommandProcessor::UpdateSystemConstantValues(")
j = v.index(") {", i)
v = v[:i] + v[i:j] + ",\n    int32_t window_offset_tiles" + v[j:]
(ROOT / VCP).write_text(v)
edit(VCP, "  draw_util::GetScissor(regs, scissor);\n",
     "  draw_util::GetScissor(regs, scissor, true, window_offset_in_edram);  // [cd] (e1f4a0d00)\n")
edit(VCP, "  // Point size.\n",
     "  // [cd] (Canary e1f4a0d00) window offset carried in the EDRAM bases, for PsParamGen\n"
     "  {\n"
     "    float param_gen_window_offset[2] = {0.0f, 0.0f};\n"
     "    if (window_offset_tiles) {\n"
     "      auto pa_sc_window_offset_pg = regs.Get<reg::PA_SC_WINDOW_OFFSET>();\n"
     "      param_gen_window_offset[0] = float(pa_sc_window_offset_pg.window_x_offset);\n"
     "      param_gen_window_offset[1] = float(pa_sc_window_offset_pg.window_y_offset);\n"
     "    }\n"
     "    for (uint32_t i = 0; i < 2; ++i) {\n"
     "      dirty |= system_constants_.param_gen_window_offset[i] != param_gen_window_offset[i];\n"
     "      system_constants_.param_gen_window_offset[i] = param_gen_window_offset[i];\n"
     "    }\n"
     "  }\n\n"
     "  // Point size.\n")
edit(VCP, "        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;\n",
     "        uint32_t rt_base_dwords_scaled =\n"
     "            draw_util::AddWindowOffsetToEdramBase(\n"
     "                color_info.color_base, window_offset_tiles,\n"
     "                xenos::IsColorRenderTargetFormat64bpp(color_info.color_format)) *\n"
     "            edram_tile_dwords_scaled;  // [cd] (e1f4a0d00)\n")
edit(VCP, "    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;\n",
     "    uint32_t depth_base_dwords_scaled =\n"
     "        draw_util::AddWindowOffsetToEdramBase(rb_depth_info.depth_base, window_offset_tiles, false) *\n"
     "        edram_tile_dwords_scaled;  // [cd] (e1f4a0d00)\n")
print("port_e1f4a0d00: applied")
