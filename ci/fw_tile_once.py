#!/usr/bin/env python3
"""Predicated tiling drawn once (Crackdown 2, 2026-11-11): cvar cd_tile_once, default false.

NOT WIRED IN, DOES NOT WORK FOR CRACKDOWN 2: the PC replay of a real frame differs (see
Downloads\Crackdown\Extras\CD2-TILE-ONCE.md). The game resolves into per-tile scratch textures (tile 2 writes the
same texture rows as tile 1) that its draws sample at window-relative positions, so one pass can't see both
tiles' contents. Kept as a record; never turn the cvar on.

Crackdown 2 draws its 1280x720 2X MSAA scene in two predicated tiles because colour and depth don't both fit the
360's 10 MB EDRAM: tile 1 (bin select 80000003) at window offset 0,0 scissored to rows 0-483, then the same command
buffers again as tile 2 (bin select C) at window offset 0,-480. Per-tile state is in predicated packets of the shared
stream: SET_CONSTANTs with bin mask 3 (tile 1) / C (tile 2) set the window offset, scissor and resolve destinations,
and every resolve appears twice, mask 3 then mask C. Tile 2's draws are about 38% of the frame's draws.

Our host render targets have no 10 MB limit, so with this on (host render targets, window offset in the EDRAM bases):
- Learn the tiles on the first frame (rendered as before): tile 2's bin select and y offset.
- Tile 1: the draws tile 2 repeats are drawn to the viewport's bottom (their scissor extended), into the same host
  images. Tile 2's predicated SET_CONSTANTs, skipped in tile 1, are kept as tile 2's register values. Tile 2's
  resolves, skipped in tile 1, are run where they stand in tile 2's context: tile 1's rows below the offset are
  copied into tile 2's render targets (which take those EDRAM tiles), tile 2's registers are swapped in, the resolve
  runs as it would in tile 2 (copy and clears), cleared rows are copied back into tile 1's images, and tile 1's
  render targets take their tiles back. So everything drawn later in tile 1 sees what tile 2 would have.
- Tile 2: on its first packet tile 1's rows are copied into tile 2's render targets (they own those tiles after it,
  as on the 360), and every passing predicated draw and resolve is dropped (all done in tile 1).
Anything it can't do exactly (a draw only tile 2 runs, an x offset, a third tile, another constant type, the
ACCURATE renderer, a missing source) leaves that frame's tile 2 rendered as before.

    fw_tile_once.py <framework or sdk dir>     (after fw_orb_fix.py)
"""
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
sdk = root / "glue" / "rexglue-sdk-main" if (root / "glue" / "rexglue-sdk-main").is_dir() else root


def edit(rel, old, new, count=1):
    p = sdk / rel
    s = p.read_text(encoding="utf-8")
    if new in s:
        print(f"already {rel}")
        return
    n = s.count(old)
    if n != count:
        sys.exit(f"fw_tile_once: {rel}: expected {count} of {old[:90]!r}, found {n}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print(f"patched {rel}")


# --- the cvar and the decisions, in the common command processor ------------------------------------------------
cp = "src/graphics/command_processor.cpp"
edit(cp, 'REXCVAR_DEFINE_BOOL(crackdown_gpu_lane, true, "GPU",',
     'REXCVAR_DEFINE_BOOL(cd_tile_once, false, "GPU",  // [cd] (fw_tile_once.py)\n'
     '                    "Predicated tiling drawn once: the first tile drawn to the bottom, the later tile\'s "\n'
     '                    "resolves run in its context, its repeated draws dropped (host render targets)");\n'
     'REXCVAR_DEFINE_BOOL(crackdown_gpu_lane, true, "GPU",')
edit(cp, """  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
""", """  cd_tile_once_extend_ = false;  // [cd] (fw_tile_once.py) per packet
  cd_tile_once_emulate_ = false;
  cd_tile_once_clip_ = false;
  if (tiling_draw && !(packet & 1) && cd_t1_extended_ && cd_t1_valid_ && bin_select_ == cd_t1_select_) {
    // [cd] (fw_tile_once.py) A draw both tiles run unpredicated: in tile 1 only above the tile boundary, tile 2
    // (rendered as before for these) draws the rest.
    cd_tile_once_clip_ = true;
  }
  if (packet & 1) {
    bool any_pass = (bin_select_ & bin_mask_) != 0;
    if (REXCVAR_GET(cd_tile_once)) {  // [cd] (fw_tile_once.py)
      switch (CdTileOncePredicate(opcode, tiling_draw, any_pass, reader, count)) {
        case 1:
          any_pass = false;
          break;
        case 2:
          any_pass = true;
          break;
        default:
          break;
      }
    }
""")
edit(cp, """  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:""", """  if (opcode == PM4_XE_SWAP) {  // [cd] (fw_tile_once.py) the tiles are per frame
    cd_t1_valid_ = false;
    cd_t1_extended_ = false;
    cd_t2_state_ = 0;
  }

  bool result = false;
  switch (opcode) {
    case PM4_ME_INIT:""")
edit(cp, """bool CommandProcessor::ExecutePacketType3(""", """int CommandProcessor::CdTileOncePredicate(uint32_t opcode, bool tiling_draw, bool any_pass,
                                          memory::RingBuffer* reader, uint32_t count) {
  // [cd] (fw_tile_once.py) A predicated packet. 0: as the bin test says, 1: skip, 2: run here in tile 2's context
  // (tile 2's own resolve or draw, skipped by tile 1's bin test).
  const RegisterFile& r = *register_file_;
  const auto wo = r.Get<reg::PA_SC_WINDOW_OFFSET>();
  const int32_t wx = int32_t(wo.window_x_offset), wy = int32_t(wo.window_y_offset);
  const uint32_t si = r.Get<reg::RB_SURFACE_INFO>().value;
  const uint32_t di = r.Get<reg::RB_DEPTH_INFO>().value;
  const uint32_t c0 = r.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[0]).value;
  const bool is_copy = tiling_draw && r.Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy;
  const bool in_t1 = cd_t1_valid_ && bin_select_ == cd_t1_select_;
  if (!any_pass) {
    // Skipped in this tile. In an extended tile 1, tile 2's own packets are kept or run here.
    if (!in_t1 || !cd_t1_extended_ || !(bin_mask_ & cd_learn_t2_select_) || (bin_mask_ & cd_t1_select_)) {
      return 0;
    }
    if (tiling_draw) {
      if (cd_t2_bad_) {
        return 0;
      }
      cd_tile_once_emulate_ = true;
      return 2;
    }
    if (opcode == PM4_SET_CONSTANT || opcode == PM4_SET_CONSTANT2) {
      const uint8_t* p = memory_->TranslatePhysical(reader->read_ptr());
      const uint32_t offset_type = memory::load_and_swap<uint32_t>(p);
      uint32_t base;
      if (opcode == PM4_SET_CONSTANT2) {
        base = offset_type & 0xFFFF;
      } else if (((offset_type >> 16) & 0xFF) == 4) {
        base = 0x2000 + (offset_type & 0x7FF);
      } else {
        if (!cd_t2_bad_ && cd_tile_once_refused_ < 4) {
          REXGPU_INFO("cd tile once: tile {:X} sets constants of type {}: tile 2 rendered as before",
                      cd_learn_t2_select_, (offset_type >> 16) & 0xFF);
        }
        cd_t2_bad_ = true;  // Shader constants per tile: not kept.
        return 0;
      }
      for (uint32_t i = 1; i < count; ++i) {
        const uint32_t reg_index = base + i - 1, value = memory::load_and_swap<uint32_t>(p + 4 * i);
        auto it = std::find_if(cd_t2_shadow_.begin(), cd_t2_shadow_.end(),
                               [reg_index](const auto& e) { return e.first == reg_index; });
        if (it != cd_t2_shadow_.end()) {
          it->second = value;
        } else if (cd_t2_shadow_.size() < 256) {
          cd_t2_shadow_.emplace_back(reg_index, value);
        } else {
          cd_t2_bad_ = true;
        }
      }
    }
    return 0;
  }
  if (!tiling_draw) {
    return 0;
  }
  const bool same_surface = cd_t1_valid_ && si == cd_t1_surface_ && di == cd_t1_depth_ && c0 == cd_t1_color0_;
  if (wx == 0 && wy == 0) {
    if (uint32_t(bin_select_) == 0xFFFFFFFFu) {
      return 0;
    }
    if (!same_surface || bin_select_ != cd_t1_select_) {
      cd_t1_valid_ = true;
      cd_t1_select_ = bin_select_;
      cd_t1_surface_ = si;
      cd_t1_depth_ = di;
      cd_t1_color0_ = c0;
      cd_t2_state_ = 0;
      cd_t2_bad_ = false;
      cd_t2_shadow_.clear();
      cd_t1_bottom_row_ = 0;
      cd_t1_extended_ = cd_learn_valid_ && si == cd_learn_surface_ && di == cd_learn_depth_ &&
                        c0 == cd_learn_color0_ && bin_select_ == cd_learn_t1_select_;
    }
    // A draw tile 2 repeats: draw it to the bottom now. One only tile 1 makes: only above the boundary (tile 2
    // makes its own, run here in its context).
    if (cd_t1_extended_ && !is_copy && bin_select_ == cd_t1_select_) {
      cd_tile_once_extend_ = (bin_mask_ & cd_learn_t2_select_) != 0;
      cd_tile_once_clip_ = !cd_tile_once_extend_;
    }
    return 0;
  }
  if (wx != 0 || wy >= 0 || !same_surface || (bin_select_ & cd_t1_select_)) {
    return 0;
  }
  if (!cd_t1_extended_) {
    // Tile 1 wasn't drawn to the bottom in this frame: learn the tiles for the next one.
    cd_learn_valid_ = true;
    cd_learn_t1_select_ = cd_t1_select_;
    cd_learn_t2_select_ = bin_select_;
    cd_learn_surface_ = si;
    cd_learn_depth_ = di;
    cd_learn_color0_ = c0;
    cd_learn_rows_ = uint32_t(-wy);
    return 0;
  }
  if (cd_t2_state_ == 0) {
    const bool ok = !cd_t2_bad_ && bin_select_ == cd_learn_t2_select_ && uint32_t(-wy) == cd_learn_rows_ &&
                    cd_t1_bottom_row_ > cd_learn_rows_ && CdTileOnceBeginLaterTile();
    cd_t2_state_ = ok ? 2 : 3;
    ++(ok ? cd_tile_once_copies_ : cd_tile_once_refused_);
    if (static int cd_logged = 0; cd_logged++ < 4) {
      REXGPU_INFO("cd tile once: tile {:X} at y {}: {} (bad {}, bottom {}, {} kept writes)", bin_select_, wy,
                  ok ? "drawn in tile 1, its draws dropped" : "rendered as before", cd_t2_bad_,
                  cd_t1_bottom_row_, cd_t2_shadow_.size());
    }
  }
  if (cd_t2_state_ == 2) {
    ++cd_tile_once_skipped_;
    return 1;
  }
  return 0;
}

bool CommandProcessor::ExecutePacketType3(""")

h = "include/rex/graphics/command_processor.h"
edit(h, """  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;
""", """  uint64_t bin_select_ = 0xFFFFFFFFull;
  uint64_t bin_mask_ = 0xFFFFFFFFull;

  // [cd] (fw_tile_once.py) Predicated tiling drawn once.
  int CdTileOncePredicate(uint32_t opcode, bool tiling_draw, bool any_pass, memory::RingBuffer* reader,
                          uint32_t count);
  // The later tile's render targets get the first tile's rows (they own them after it); false if it can't be done.
  virtual bool CdTileOnceBeginLaterTile() { return false; }
  bool cd_t1_valid_ = false, cd_t1_extended_ = false;
  uint64_t cd_t1_select_ = 0;
  uint32_t cd_t1_surface_ = 0, cd_t1_depth_ = 0, cd_t1_color0_ = 0;
  uint32_t cd_t1_bottom_row_ = 0;  // where the extended draws end (the viewport's bottom)
  uint32_t cd_t2_state_ = 0;       // 0 not reached, 2 dropped, 3 rendered as before
  bool cd_t2_bad_ = false;
  std::vector<std::pair<uint32_t, uint32_t>> cd_t2_shadow_;  // tile 2's predicated register writes, in order
  bool cd_learn_valid_ = false;
  uint64_t cd_learn_t1_select_ = 0, cd_learn_t2_select_ = 0;
  uint32_t cd_learn_surface_ = 0, cd_learn_depth_ = 0, cd_learn_color0_ = 0, cd_learn_rows_ = 0;
  bool cd_tile_once_extend_ = false, cd_tile_once_emulate_ = false, cd_tile_once_clip_ = false;
  uint64_t cd_tile_once_skipped_ = 0, cd_tile_once_copies_ = 0, cd_tile_once_refused_ = 0;
""")

# --- a deferred vkCmdCopyImage ---------------------------------------------------------------------------------
dh = "include/rex/graphics/vulkan/deferred_command_buffer.h"
edit(dh, """  void CmdVkCopyQueryPoolResults(VkQueryPool query_pool,""", """  void CmdVkCopyImage(VkImage src_image, VkImageLayout src_image_layout, VkImage dst_image,
                      VkImageLayout dst_image_layout, uint32_t region_count, const VkImageCopy* regions) {
    // [cd] (fw_tile_once.py)
    const size_t header_size = rex::align(sizeof(ArgsVkCopyImage), alignof(VkImageCopy));
    uint8_t* args_ptr = reinterpret_cast<uint8_t*>(
        WriteCommand(Command::kVkCopyImage, header_size + sizeof(VkImageCopy) * region_count));
    auto& args = *reinterpret_cast<ArgsVkCopyImage*>(args_ptr);
    args.src_image = src_image;
    args.src_image_layout = src_image_layout;
    args.dst_image = dst_image;
    args.dst_image_layout = dst_image_layout;
    args.region_count = region_count;
    std::memcpy(args_ptr + header_size, regions, sizeof(VkImageCopy) * region_count);
  }

  void CmdVkCopyQueryPoolResults(VkQueryPool query_pool,""")
edit(dh, """    kVkCopyBufferToImage,
""", """    kVkCopyBufferToImage,
    kVkCopyImage,  // [cd] (fw_tile_once.py)
""")
edit(dh, """  struct ArgsVkCopyBufferToImage {""", """  struct ArgsVkCopyImage {  // [cd] (fw_tile_once.py)
    VkImage src_image;
    VkImageLayout src_image_layout;
    VkImage dst_image;
    VkImageLayout dst_image_layout;
    uint32_t region_count;
    static_assert(alignof(VkImageCopy) <= alignof(uintmax_t));
  };

  struct ArgsVkCopyBufferToImage {""")
dc = "src/graphics/vulkan/deferred_command_buffer.cpp"
edit(dc, """      case Command::kVkCopyQueryPoolResults: {""", """      case Command::kVkCopyImage: {  // [cd] (fw_tile_once.py)
        auto& args = *reinterpret_cast<const ArgsVkCopyImage*>(stream);
        dfn.vkCmdCopyImage(command_buffer, args.src_image, args.src_image_layout, args.dst_image,
                           args.dst_image_layout, args.region_count,
                           reinterpret_cast<const VkImageCopy*>(
                               reinterpret_cast<const uint8_t*>(stream) +
                               rex::align(sizeof(ArgsVkCopyImage), alignof(VkImageCopy))));
      } break;

      case Command::kVkCopyQueryPoolResults: {""")

# --- the copies, in the Vulkan render target cache -------------------------------------------------------------
rch = "include/rex/graphics/pipeline/render_target/cache.h"
edit(rch, "namespace rex::graphics {\n",
     "namespace rex::graphics {\nnamespace vulkan {\nclass VulkanRenderTargetCache;  // [cd] (fw_tile_once.py)\n}\n")
edit(rch, "class RenderTargetCache {\n",
     "class RenderTargetCache {\n  friend class vulkan::VulkanRenderTargetCache;  // [cd] (fw_tile_once.py) CdTileOnceCopy\n\n")

rh = "include/rex/graphics/vulkan/render_target_cache.h"
edit(rh, """  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;""",
     """  bool IsHostDepthEncodingDifferent(xenos::DepthRenderTargetFormat format) const override;

 public:
  // [cd] (fw_tile_once.py) Between the first tile's render targets (the current depth and colour bases) and the
  // later tile's (the bases moved by first_row rows), host image rows [first_row, end_row):
  // kCdTileToLater copies first -> later and the later ones own those EDRAM tiles; kCdTileBack copies
  // later -> first and the first ones own them again; kCdTileReclaim only gives them back. False: nothing done.
  enum CdTileOnceMode { kCdTileToLater, kCdTileBack, kCdTileReclaim };
  bool CdTileOnceCopy(uint32_t first_row, uint32_t end_row, CdTileOnceMode mode);

 protected:""")

rc = "src/graphics/vulkan/render_target_cache.cpp"
edit(rc, """  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
""", """  image_create_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
  if (REXCVAR_GET(cd_tile_once)) {  // [cd] (fw_tile_once.py) copied between tiles
    image_create_info.usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
""")
edit(rc, """RenderTargetCache::RenderTarget* VulkanRenderTargetCache::CreateRenderTarget(RenderTargetKey key) {""",
     """bool VulkanRenderTargetCache::CdTileOnceCopy(uint32_t first_row, uint32_t end_row, CdTileOnceMode mode) {
  // [cd] (fw_tile_once.py)
  if (GetPath() != Path::kHostRenderTargets || !first_row || end_row <= first_row) {
    return false;
  }
  const RegisterFile& regs = register_file();
  const auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  const xenos::MsaaSamples msaa_samples = rb_surface_info.msaa_samples;
  const uint32_t msaa_y_log2 = uint32_t(msaa_samples >= xenos::MsaaSamples::k2X);
  if (((first_row << msaa_y_log2) % xenos::kEdramTileHeightSamples) ||
      ((end_row << msaa_y_log2) % xenos::kEdramTileHeightSamples)) {
    return false;
  }
  const uint32_t msaa_x_log2 = uint32_t(msaa_samples >= xenos::MsaaSamples::k4X);
  const uint32_t pitch_tiles_at_32bpp =
      ((rb_surface_info.surface_pitch << msaa_x_log2) + (xenos::kEdramTileWidthSamples - 1)) /
      xenos::kEdramTileWidthSamples;
  if (!pitch_tiles_at_32bpp || end_row > GetRenderTargetHeight(pitch_tiles_at_32bpp, msaa_samples)) {
    return false;
  }
  const int32_t window_offset_tiles =
      -int32_t(((first_row << msaa_y_log2) / xenos::kEdramTileHeightSamples) * pitch_tiles_at_32bpp);
  // The first tile's render targets: depth and the colour ones written.
  RenderTargetKey sources[1 + xenos::kMaxColorRenderTargets];
  uint32_t source_count = 0;
  auto add_source = [&](RenderTargetKey key) {
    for (uint32_t i = 0; i < source_count; ++i) {
      if (sources[i] == key) return;
    }
    sources[source_count++] = key;
  };
  {
    const auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    RenderTargetKey key;
    key.base_tiles = rb_depth_info.depth_base;
    key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    key.msaa_samples = msaa_samples;
    key.is_depth = 1;
    key.resource_format = uint32_t(rb_depth_info.depth_format);
    add_source(key);
  }
  const uint32_t color_mask = regs.Get<reg::RB_COLOR_MASK>().value;
  for (uint32_t i = 0; i < xenos::kMaxColorRenderTargets; ++i) {
    if (i && !(color_mask & (uint32_t(0b1111) << (4 * i)))) {
      continue;
    }
    const auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    RenderTargetKey key;
    key.base_tiles = color_info.color_base;
    key.pitch_tiles_at_32bpp = pitch_tiles_at_32bpp;
    key.msaa_samples = msaa_samples;
    key.resource_format =
        uint32_t(GetColorResourceFormat(xenos::GetStorageColorFormat(color_info.color_format)));
    add_source(key);
  }
  struct Pair {
    VulkanRenderTarget* first;
    VulkanRenderTarget* later;
    uint32_t later_start_tiles, first_start_tiles, length_tiles;
  };
  Pair pairs[1 + xenos::kMaxColorRenderTargets];
  uint32_t pair_count = 0;
  for (uint32_t i = 0; i < source_count; ++i) {
    auto first_it = render_targets_.find(sources[i]);
    if (first_it == render_targets_.end() || !first_it->second) {
      continue;  // Not drawn by the first tile.
    }
    RenderTargetKey later_key = sources[i];
    const uint32_t is_64bpp = uint32_t(later_key.Is64bpp());
    later_key.base_tiles =
        draw_util::AddWindowOffsetToEdramBase(sources[i].base_tiles, window_offset_tiles, is_64bpp);
    later_key.next_period = 0;
    RenderTarget* later = GetOrCreateRenderTarget(later_key);
    if (!later) {
      return false;
    }
    const uint32_t row_tiles = pitch_tiles_at_32bpp << is_64bpp;
    const uint32_t start = ((first_row << msaa_y_log2) / xenos::kEdramTileHeightSamples) * row_tiles;
    const uint32_t end = ((end_row << msaa_y_log2) / xenos::kEdramTileHeightSamples) * row_tiles;
    if (start >= xenos::kEdramTileCount || end - start > xenos::kEdramTileCount) {
      return false;
    }
    // The same EDRAM tiles relative to the first tile's base.
    const uint32_t first_start =
        (later_key.base_tiles + start + xenos::kEdramTileCount - sources[i].base_tiles) &
        (xenos::kEdramTileCount - 1);
    pairs[pair_count++] = {static_cast<VulkanRenderTarget*>(first_it->second),
                           static_cast<VulkanRenderTarget*>(later), start, first_start, end - start};
  }
  if (!pair_count) {
    return false;
  }
  for (uint32_t i = 0; i < pair_count; ++i) {
    // The tiles change hands with nothing to transfer: the copy below fills them.
    if (mode == kCdTileToLater) {
      ChangeOwnership(pairs[i].later->key(), pairs[i].later_start_tiles, pairs[i].length_tiles, nullptr);
    } else {
      ChangeOwnership(pairs[i].first->key(), pairs[i].first_start_tiles, pairs[i].length_tiles, nullptr);
    }
  }
  if (mode == kCdTileReclaim) {
    return true;
  }
  command_processor_.EndRenderPass();
  auto aspects = [](const VulkanRenderTarget* rt) -> VkImageAspectFlags {
    return rt->key().is_depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                              : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT);
  };
  for (uint32_t i = 0; i < pair_count; ++i) {
    VulkanRenderTarget* src = mode == kCdTileToLater ? pairs[i].first : pairs[i].later;
    VulkanRenderTarget* dst = mode == kCdTileToLater ? pairs[i].later : pairs[i].first;
    for (VulkanRenderTarget* rt : {src, dst}) {
      const bool is_source = rt == src;
      VkImageSubresourceRange range = {aspects(rt), 0, 1, 0, 1};
      const VkImageLayout layout =
          is_source ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      const VkAccessFlags access = is_source ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
      command_processor_.PushImageMemoryBarrier(rt->image(), range, rt->current_stage_mask(),
                                                VK_PIPELINE_STAGE_TRANSFER_BIT, rt->current_access_mask(), access,
                                                rt->current_layout(), layout);
      rt->SetUsage(VK_PIPELINE_STAGE_TRANSFER_BIT, access, layout);
    }
  }
  command_processor_.SubmitBarriers(true);
  DeferredCommandBuffer& command_buffer = command_processor_.deferred_command_buffer();
  for (uint32_t i = 0; i < pair_count; ++i) {
    VulkanRenderTarget* src = mode == kCdTileToLater ? pairs[i].first : pairs[i].later;
    VulkanRenderTarget* dst = mode == kCdTileToLater ? pairs[i].later : pairs[i].first;
    VkImageCopy region = {};
    region.srcSubresource = {aspects(src), 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.srcOffset = {0, int32_t(first_row * draw_resolution_scale_y()), 0};
    region.dstOffset = region.srcOffset;
    region.extent = {src->key().GetWidth() * draw_resolution_scale_x(),
                     (end_row - first_row) * draw_resolution_scale_y(), 1};
    command_buffer.CmdVkCopyImage(src->image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image(),
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  }
  // Back to drawing, as the render target cache left them.
  for (uint32_t i = 0; i < pair_count; ++i) {
    for (VulkanRenderTarget* rt : {pairs[i].first, pairs[i].later}) {
      VkPipelineStageFlags stage;
      VkAccessFlags access;
      VkImageLayout layout;
      rt->GetDrawUsage(&stage, &access, &layout);
      VkImageSubresourceRange range = {aspects(rt), 0, 1, 0, 1};
      command_processor_.PushImageMemoryBarrier(rt->image(), range, rt->current_stage_mask(), stage,
                                                rt->current_access_mask(), access, rt->current_layout(), layout);
      rt->SetUsage(stage, access, layout);
    }
  }
  command_processor_.SubmitBarriers(true);
  return true;
}

RenderTargetCache::RenderTarget* VulkanRenderTargetCache::CreateRenderTarget(RenderTargetKey key) {""")
edit(rc, "#include <algorithm>\n", "#include <algorithm>\n#include <initializer_list>\n")
edit(rc, 'REXCVAR_DEFINE_STRING(render_target_path_vulkan, "", "GPU/Vulkan",',
     'REXCVAR_DECLARE(bool, cd_tile_once);  // [cd] (fw_tile_once.py)\n'
     'REXCVAR_DEFINE_STRING(render_target_path_vulkan, "", "GPU/Vulkan",')

# --- the Vulkan command processor: extended draws, tile 2's resolves in tile 1, tile 2's start --------------------
vh = "include/rex/graphics/vulkan/command_processor.h"
edit(vh, """  bool IssueCopy() override;""", """  bool IssueCopy() override;
  bool CdTileOnceBeginLaterTile() override;  // [cd] (fw_tile_once.py)
  // [cd] (fw_tile_once.py) Runs tile 2's packet in tile 1, in tile 2's context.
  bool CdTileOnceRunInLaterTile(bool is_draw, const std::function<bool()>& run);
  bool cd_tile_once_in_later_ = false;""")
edit(vh, "#include <array>\n", "#include <array>\n#include <functional>\n")

vk = "src/graphics/vulkan/command_processor.cpp"
edit(vk, """  draw_util::GetScissor(regs, scissor, true, window_offset_in_edram);  // [cd] (e1f4a0d00)
""", """  draw_util::GetScissor(regs, scissor, true, window_offset_in_edram);  // [cd] (e1f4a0d00)
  if (cd_tile_once_extend_) {
    // [cd] (fw_tile_once.py) A first-tile draw the later tile repeats: its scissor ends at the tile's bottom
    // (with the game's guard rows); draw it to the viewport's bottom, as the later tile would.
    const float cd_bottom = regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YOFFSET) +
                            std::abs(regs.Get<float>(XE_GPU_REG_PA_CL_VPORT_YSCALE));
    const uint32_t cd_bottom_row = cd_bottom > 0.0f && cd_bottom < 8192.0f ? uint32_t(std::ceil(cd_bottom)) : 0u;
    const uint32_t cd_scissor_bottom = scissor.offset[1] + scissor.extent[1];
    if (cd_bottom_row > cd_learn_rows_) {
      cd_t1_bottom_row_ = std::max(cd_t1_bottom_row_, cd_bottom_row);
      if (cd_scissor_bottom >= cd_learn_rows_ && cd_scissor_bottom < cd_bottom_row) {
        scissor.extent[1] = cd_bottom_row - scissor.offset[1];
      }
    }
  } else if (cd_tile_once_clip_ && scissor.offset[1] + scissor.extent[1] > cd_learn_rows_) {
    // [cd] (fw_tile_once.py) Only tile 1's: its rows from the boundary down are tile 2's.
    scissor.extent[1] = scissor.offset[1] < cd_learn_rows_ ? cd_learn_rows_ - scissor.offset[1] : 0;
  }
""")
edit(vk, """bool VulkanCommandProcessor::IssueCopy() {
""", """bool VulkanCommandProcessor::CdTileOnceBeginLaterTile() {
  // [cd] (fw_tile_once.py) Tile 2's render targets get tile 1's rows below the offset and own them after it.
  return render_target_cache_->CdTileOnceCopy(cd_learn_rows_, cd_t1_bottom_row_,
                                              VulkanRenderTargetCache::kCdTileToLater);
}

bool VulkanCommandProcessor::CdTileOnceRunInLaterTile(bool is_draw, const std::function<bool()>& run) {
  // [cd] (fw_tile_once.py) Tile 2's own resolve or draw, skipped by tile 1's bin test, run here in tile 2's context:
  // its render targets get tile 1's rows, its predicated register writes are swapped in, and the rows it changed
  // (a draw, or a resolve's clear) come back to tile 1's images, which own their tiles again.
  cd_tile_once_emulate_ = false;
  if (!render_target_cache_->CdTileOnceCopy(cd_learn_rows_, cd_t1_bottom_row_,
                                            VulkanRenderTargetCache::kCdTileToLater)) {
    cd_t2_bad_ = true;  // Tile 2 renders as before, this packet with it.
    return true;
  }
  RegisterFile& regs = *register_file_;
  std::vector<std::pair<uint32_t, uint32_t>> saved;
  saved.reserve(cd_t2_shadow_.size());
  for (const auto& [reg_index, value] : cd_t2_shadow_) {
    saved.emplace_back(reg_index, regs.values[reg_index]);
    regs.values[reg_index] = value;
  }
  const auto copy_control = regs.Get<reg::RB_COPY_CONTROL>();
  const bool changed = is_draw || copy_control.depth_clear_enable || copy_control.color_clear_enable;
  cd_tile_once_in_later_ = true;
  const bool result = run();
  cd_tile_once_in_later_ = false;
  for (auto it = saved.rbegin(); it != saved.rend(); ++it) {
    regs.values[it->first] = it->second;
  }
  render_target_cache_->CdTileOnceCopy(cd_learn_rows_, cd_t1_bottom_row_,
                                       changed ? VulkanRenderTargetCache::kCdTileBack
                                               : VulkanRenderTargetCache::kCdTileReclaim);
  return result;
}

bool VulkanCommandProcessor::IssueCopy() {
  if (cd_tile_once_emulate_ && !cd_tile_once_in_later_) {
    return CdTileOnceRunInLaterTile(false, [this]() { return IssueCopy(); });  // [cd] (fw_tile_once.py)
  }
""")
edit(vk, """bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
""", """bool VulkanCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  if (cd_tile_once_emulate_ && !cd_tile_once_in_later_) {  // [cd] (fw_tile_once.py)
    return CdTileOnceRunInLaterTile(true, [&]() {
      return IssueDraw(prim_type, index_count, index_buffer_info, major_mode_explicit);
    });
  }
""")
edit(vk, "#include <algorithm>\n", "#include <algorithm>\n#include <cmath>\n#include <functional>\n")
