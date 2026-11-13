#!/usr/bin/env python3
"""Texture descriptor sets reused within a frame when their contents repeat (cvar cd_texture_set_reuse, framework
default false; turned on in Crackdown 2's app only once proven).

Why (2026-11-11, Crackdown 2 trace 4D5308BC_49176 replayed through this renderer): a frame writes ~2,280
texture/sampler descriptor sets, one for every draw whose bindings differ from the draw just before, and 91% of
them have exactly the contents of a set already written earlier in the same frame: the second EDRAM tile
replays every draw of the first with the same textures, and tile 1 itself repeats material bindings. Each one
costs a descriptor set allocation plus a vkUpdateDescriptorSets (on the GPU lane when it runs). Steady-state,
on the PC, that step was 2.2 of the ~6.1 us the GPU command thread spends per draw (lavapipe's cost; RADV's is
lower, so the PS5 saving is smaller and unmeasured).

How: per frame, a map from (set layout, image views and layouts, samplers) to the set already written with
them. A hit binds that set and writes nothing. The map is cleared whenever frame_current_ changes (transient
sets go back to the free list once their frame has finished on the GPU, so they are never reused across frames)
and whenever texture_binding_reset_generation changes (the same reset the per-stage binding memo obeys).
A descriptor set's contents are its whole effect on a draw, so a draw bound to an equal set renders exactly as
with a fresh one: the picture can't change.

    fw_tile_replay.py <framework dir with glue/rexglue-sdk-main>   or   fw_tile_replay.py --sdk <sdk dir>
"""
import sys
from pathlib import Path

if sys.argv[1] == "--sdk":
    sdk = Path(sys.argv[2])
else:
    sdk = Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"


def edit(rel, old, new, count=1):
    p = sdk / rel
    s = p.read_text(encoding="utf-8")
    if new in s:
        print(f"already {rel}")
        return
    n = s.count(old)
    if n != count:
        sys.exit(f"fw_tile_replay: {rel}: expected {count} of {old[:90]!r}, found {n}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print(f"patched {rel}")


cp = "src/graphics/vulkan/command_processor.cpp"

edit(cp, 'REXCVAR_DEFINE_BOOL(cd_fence_deferred, false, "GPU",  // [cd] (fw_fence_fixes.py)\n',
     'REXCVAR_DEFINE_BOOL(cd_texture_set_reuse, false, "GPU",  // [cd] (fw_tile_replay.py)\n'
     '                    "Reuse a texture descriptor set written earlier in the same frame with the same "\n'
     '                    "contents instead of writing a new one");\n'
     'REXCVAR_DEFINE_BOOL(cd_fence_deferred, false, "GPU",  // [cd] (fw_fence_fixes.py)\n')

edit(cp, "\nbool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,",
     r'''
// [cd] (fw_tile_replay.py) this frame's texture descriptor sets by contents
namespace {
struct CdTexSetKeyHash {
  size_t operator()(const std::vector<uint64_t>& k) const {
    uint64_t h = 1469598103934665603ull;
    for (uint64_t v : k) h = (h ^ v) * 1099511628211ull;
    return size_t(h ^ (h >> 29));
  }
};
struct CdTexSetCache {
  uint64_t frame = ~uint64_t(0);
  uint32_t generation = 0;
  std::unordered_map<std::vector<uint64_t>, VkDescriptorSet, CdTexSetKeyHash> sets;
  std::vector<uint64_t> key;
  uint64_t hits = 0, misses = 0;
  void Key(VkDescriptorSetLayout layout, uint32_t texture_count, uint32_t sampler_count,
           const VkDescriptorImageInfo* textures, const VkDescriptorImageInfo* samplers) {
    key.clear();
    key.push_back(uint64_t(uintptr_t(layout)));
    key.push_back(uint64_t(texture_count) | (uint64_t(sampler_count) << 32));
    for (uint32_t i = 0; i < texture_count; ++i) {
      key.push_back(uint64_t(uintptr_t(textures[i].imageView)));
      key.push_back(uint64_t(textures[i].imageLayout));
    }
    for (uint32_t i = 0; i < sampler_count; ++i) key.push_back(uint64_t(uintptr_t(samplers[i].sampler)));
  }
};
CdTexSetCache cd_tex_set_cache;
}  // namespace

bool VulkanCommandProcessor::UpdateBindings(const VulkanShader* vertex_shader,''')

# Per call: start a new map when the frame or the binding reset generation changed.
edit(cp, '''  const VkDescriptorSetLayout textures_layout_vertex =
      current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref();''',
     '''  const bool cd_reuse_sets = REXCVAR_GET(cd_texture_set_reuse);  // [cd] (fw_tile_replay.py)
  if (cd_reuse_sets) {
    const uint32_t cd_generation = texture_binding_reset_generation.load(std::memory_order_relaxed);
    if (cd_tex_set_cache.frame != frame_current_ || cd_tex_set_cache.generation != cd_generation) {
      if (cd_tex_set_cache.frame != ~uint64_t(0) && (frame_current_ % 600) == 0) {
        REXGPU_INFO("cd texture sets: {} reused, {} written (last frame)", cd_tex_set_cache.hits,
                    cd_tex_set_cache.misses);
      }
      cd_tex_set_cache.sets.clear();
      cd_tex_set_cache.hits = cd_tex_set_cache.misses = 0;
      cd_tex_set_cache.frame = frame_current_;
      cd_tex_set_cache.generation = cd_generation;
    }
  }
  const VkDescriptorSetLayout textures_layout_vertex =
      current_guest_graphics_pipeline_layout_->descriptor_set_layout_textures_vertex_ref();''')

for stage, name, vs in ((0, "Vertex", "vertex"), (1, "Pixel", "pixel")):
    edit(cp, f'''  // {name} shader textures and samplers.
  if (write_{vs}_textures) {{
''', f'''  // {name} shader textures and samplers.
  if (write_{vs}_textures && cd_reuse_sets) {{  // [cd] (fw_tile_replay.py) a set with these contents this frame
    cd_tex_set_cache.Key(textures_layout_{vs}, texture_count_{vs}, sampler_count_{vs},
                         descriptor_write_image_info_.data() + {vs}_texture_image_info_offset,
                         descriptor_write_image_info_.data() + {vs}_sampler_image_info_offset);
    auto cd_it = cd_tex_set_cache.sets.find(cd_tex_set_cache.key);
    if (cd_it != cd_tex_set_cache.sets.end()) {{
      ++cd_tex_set_cache.hits;
      write_{vs}_textures = false;
      write_descriptor_set_bits |= UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTextures{name};
      current_graphics_descriptor_sets_[SpirvShaderTranslator::kDescriptorSetTextures{name}] = cd_it->second;
      current_graphics_descriptor_sets_bound_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kDescriptorSetTextures{name});
      remember_texture_bindings({stage}, cd_it->second, textures_layout_{vs}, texture_count_{vs},
                                sampler_count_{vs}, {vs}_texture_image_info_offset,
                                {vs}_sampler_image_info_offset);
    }}
  }}
  if (write_{vs}_textures) {{
''')
    edit(cp, f'''    remember_texture_bindings({stage}, write_textures[0].dstSet, textures_layout_{vs},
                              texture_count_{vs}, sampler_count_{vs},
                              {vs}_texture_image_info_offset, {vs}_sampler_image_info_offset);
  }}''', f'''    remember_texture_bindings({stage}, write_textures[0].dstSet, textures_layout_{vs},
                              texture_count_{vs}, sampler_count_{vs},
                              {vs}_texture_image_info_offset, {vs}_sampler_image_info_offset);
    if (cd_reuse_sets) {{  // [cd] (fw_tile_replay.py)
      ++cd_tex_set_cache.misses;
      cd_tex_set_cache.Key(textures_layout_{vs}, texture_count_{vs}, sampler_count_{vs},
                           descriptor_write_image_info_.data() + {vs}_texture_image_info_offset,
                           descriptor_write_image_info_.data() + {vs}_sampler_image_info_offset);
      cd_tex_set_cache.sets.emplace(cd_tex_set_cache.key, write_textures[0].dstSet);
    }}
  }}''')

s = (sdk / cp).read_text(encoding="utf-8")
if "#include <unordered_map>" not in s:
    edit(cp, "#include <algorithm>\n", "#include <algorithm>\n#include <unordered_map>\n")
