#!/usr/bin/env python3
"""Same-frame resolve readback without a GPU drain per resolve (Crackdown 2: crackdown2_016, 17:09, in play every 10 s
14-22 fps; per frame on the GPU thread copies 21.6-26.8 ms, GPU fences 12.4-14.3, submit 10.5-14.7 at 8.5-9.6
submits a frame; "cd readback: 2970-4411 resolves read back in 10 s (same-frame, a GPU drain each)").

readback_resolve "full" (Crackdown's: "fast", one frame late, made the orbs, supply-point beams and waypoints slide
with the camera, 2026-11-05) ends the submission and waits for the GPU at EVERY resolve the CPU may read
(AwaitAllQueueOperationsCompletion in IssueCopy_ReadbackResolvePath), ~10-15 a frame, most of them a chain of small
ones (16FB0000-16FD0000: 128, 32, 16, 4 KB...).

The CPU can't read a resolve's result before it learns the GPU got there, and it learns that only from what the GPU
writes where the CPU sees it: the ring's read pointer write-back, a scratch register write-back, an interrupt,
MEM_WRITE, REG_TO_MEM, COND_WRITE, EVENT_WRITE_SHD (the fence D3D waits on), the WAIT_REG_MEM read-pointer write-back,
and the swap. Every one of those already waits for the GPU lane ("GpuLane::Get().Drain(); // [crackdown]").
cd_readback_batched (default false; toml: cd_readback_batched = true): in "full" mode a resolve's copy to its readback
buffer is recorded as before, but the drain and the copy into guest memory wait for the next of those points, where
ONE drain serves every resolve since the last (the newest per address range; the GPU keeps using its own copy in
shared memory either way). The CPU sees the same data at the same point of the command stream as with "full", so
nothing is a frame late. "cd readback batched:" every 10 s: readbacks, drains, and which point flushed them.

    fw_readback_fixes.py <framework>     (after fw_tiling_fixes.py)
"""
import pathlib
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        sys.exit(f"fw_readback_fixes: expected {count} of {old[:80]!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    sdk = pathlib.Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"
    cp_h = sdk / "include" / "rex" / "graphics" / "command_processor.h"
    cp = sdk / "src" / "graphics" / "command_processor.cpp"
    vcp_h = sdk / "include" / "rex" / "graphics" / "vulkan" / "command_processor.h"
    vcp = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"

    edit(cp_h, "  virtual void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,\n"
               "                         uint32_t frontbuffer_height) = 0;\n",
         "  virtual void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,\n"
         "                         uint32_t frontbuffer_height) = 0;\n"
         "  // [cd] (fw_readback_fixes.py) The GPU is about to write where the CPU sees it: resolves read back since the\n"
         "  // last such point reach guest memory first (cd_readback_batched). reason: kCdFlush*.\n"
         "  enum CdFlushReason { kCdFlushReadPointer, kCdFlushScratch, kCdFlushInterrupt, kCdFlushWaitWriteback,\n"
         "                       kCdFlushRegToMem, kCdFlushMemWrite, kCdFlushCondWrite, kCdFlushEventWriteShd,\n"
         "                       kCdFlushSwap, kCdFlushReasons };\n"
         "  virtual void CdFlushDeferredReadbacks(CdFlushReason reason) {}\n")

    sites = [
        ("      GpuLane::Get().Drain();  // [crackdown] (what the game sees: the lane's work done first)\n"
         "      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),\n"
         "                                       read_ptr_index_);\n", "kCdFlushReadPointer", 1),
        ("      GpuLane::Get().Drain();  // [crackdown]\n"
         "      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);\n", "kCdFlushScratch", 1),
        ("  GpuLane::Get().Drain();  // [crackdown]\n  for (int n = 0; n < 6; n++) {\n", "kCdFlushInterrupt", 1),
        ("        GpuLane::Get().Drain();\n"
         "        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),\n",
         "kCdFlushWaitWriteback", 1),
        ("  GpuLane::Get().Drain();  // [crackdown]\n  memory::store(memory_->TranslatePhysical(mem_addr), reg_val);\n",
         "kCdFlushRegToMem", 1),
        ("  GpuLane::Get().Drain();  // [crackdown]\n  for (uint32_t i = 0; i < count - 1; i++) {\n", "kCdFlushMemWrite", 1),
        ("      GpuLane::Get().Drain();  // [crackdown]\n"
         "      memory::store(memory_->TranslatePhysical(write_reg_addr), write_data);\n", "kCdFlushCondWrite", 1),
        ("  GpuLane::Get().Drain();  // [crackdown]\n  memory::store(memory_->TranslatePhysical(address), data_value);\n",
         "kCdFlushEventWriteShd", 1),
    ]
    for old, reason, count in sites:
        i = old.index("GpuLane::Get().Drain();")
        indent = old[:i].split("\n")[-1]
        line_end = old.index("\n", i) + 1
        new = old[:line_end] + f"{indent}CdFlushDeferredReadbacks({reason});  // [cd] (fw_readback_fixes.py)\n" + old[line_end:]
        edit(cp, old, new, count)

    # Vulkan: the batch
    edit(vcp_h, "  void EvictOldReadbackBuffers(std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map);\n",
         "  void EvictOldReadbackBuffers(std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map);\n"
         "  // [cd] (fw_readback_fixes.py, cd_readback_batched) resolves read back since the last point the CPU can see\n"
         "  // the GPU's progress: their copies are recorded; the drain and the copy into guest memory wait for it\n"
         "  void CdFlushDeferredReadbacks(CdFlushReason reason) override;\n"
         "  struct CdPendingReadback {\n"
         "    uint64_t key;\n"
         "    uint32_t index;\n"
         "    uint32_t address;\n"
         "    uint32_t length;\n"
         "  };\n"
         "  std::vector<CdPendingReadback> cd_pending_readbacks_;\n"
         "  uint64_t cd_batched_readbacks_ = 0, cd_batched_drains_ = 0;\n"
         "  uint64_t cd_batched_reasons_[kCdFlushReasons] = {};\n")
    edit(vcp, 'REXCVAR_DEFINE_BOOL(vulkan_readback_resolve, false, "GPU/Vulkan",\n',
         'REXCVAR_DEFINE_BOOL(cd_readback_batched, false, "GPU",  // [cd] (fw_readback_fixes.py)\n'
         '                    "readback_resolve full: one GPU drain at the next point the CPU can see the GPU\'s "\n'
         '                    "progress (fence, interrupt, read pointer...) serves every resolve since, not one each");\n'
         'REXCVAR_DEFINE_BOOL(vulkan_readback_resolve, false, "GPU/Vulkan",\n')
    edit(vcp, "  uint32_t read_index = write_index;\n"
              "  if (use_delayed_sync) {\n"
              "    read_index = 1 - write_index;\n"
              "  } else if (!AwaitAllQueueOperationsCompletion()) {\n"
              "    return true;\n"
              "  }\n",
         "  if (!use_delayed_sync && REXCVAR_GET(cd_readback_batched)) {\n"
         "    // [cd] (fw_readback_fixes.py) the copy is recorded; the drain and the copy into guest memory wait for the\n"
         "    // next point the CPU can see the GPU's progress (CdFlushDeferredReadbacks). Only the newest copy of a range\n"
         "    // is kept: the CPU can't have seen an older one in between.\n"
         "    cd_pending_readbacks_.erase(std::remove_if(cd_pending_readbacks_.begin(), cd_pending_readbacks_.end(),\n"
         "                                               [&](const CdPendingReadback& p) { return p.key == resolve_key; }),\n"
         "                                cd_pending_readbacks_.end());\n"
         "    cd_pending_readbacks_.push_back({resolve_key, write_index, written_address, written_length});\n"
         "    ++cd_batched_readbacks_;\n"
         "    readback.current_index = 1 - readback.current_index;\n"
         "    return true;\n"
         "  }\n"
         "  uint32_t read_index = write_index;\n"
         "  if (use_delayed_sync) {\n"
         "    read_index = 1 - write_index;\n"
         "  } else if (!AwaitAllQueueOperationsCompletion()) {\n"
         "    return true;\n"
         "  }\n")
    edit(vcp, "void VulkanCommandProcessor::EvictOldReadbackBuffers(\n",
         """void VulkanCommandProcessor::CdFlushDeferredReadbacks(CdFlushReason reason) {
  // [cd] (fw_readback_fixes.py)
  if (!cd_pending_readbacks_.empty()) {
    ++cd_batched_drains_;
    ++cd_batched_reasons_[reason];
    if (AwaitAllQueueOperationsCompletion()) {
      const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
      const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
      const VkDevice device = vulkan_device->device();
      for (const CdPendingReadback& p : cd_pending_readbacks_) {
        auto it = readback_buffers_.find(p.key);
        if (it == readback_buffers_.end()) {
          continue;
        }
        ReadbackBuffer& readback = it->second;
        if (readback.buffers[p.index] == VK_NULL_HANDLE || readback.mapped_data[p.index] == nullptr ||
            p.length > readback.sizes[p.index]) {
          continue;
        }
        VkMappedMemoryRange range = {};
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = readback.memories[p.index];
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        dfn.vkInvalidateMappedMemoryRanges(device, 1, &range);
        if (uint8_t* destination = memory_->TranslatePhysical(p.address)) {
          std::memcpy(destination, readback.mapped_data[p.index], p.length);
        }
      }
    }
    cd_pending_readbacks_.clear();
  }
  static auto cd_last = std::chrono::steady_clock::now();
  const auto cd_now = std::chrono::steady_clock::now();
  if (cd_now - cd_last >= std::chrono::seconds(10) && (cd_batched_readbacks_ || cd_batched_drains_)) {
    static const char* const kNames[kCdFlushReasons] = {"read pointer", "scratch", "interrupt", "wait write-back",
                                                        "REG_TO_MEM", "MEM_WRITE", "COND_WRITE", "EVENT_WRITE_SHD",
                                                        "swap"};
    std::string reasons;
    for (uint32_t i = 0; i < kCdFlushReasons; ++i) {
      if (cd_batched_reasons_[i]) reasons += fmt::format(" {} {}", kNames[i], cd_batched_reasons_[i]);
      cd_batched_reasons_[i] = 0;
    }
    REXGPU_INFO("cd readback batched: {} resolves read back in {} GPU drains in 10 s, at:{}", cd_batched_readbacks_,
                cd_batched_drains_, reasons);
    cd_batched_readbacks_ = cd_batched_drains_ = 0;
    cd_last = cd_now;
  }
}

void VulkanCommandProcessor::EvictOldReadbackBuffers(
""")
    # Where a copy's time goes (the "copies" figure, ~1 ms a resolve in crackdown2_016): the resolve itself
    # (render target transfers / dumps into shared memory), the old per-resolve drain, and the batched drains.
    edit(vcp, "bool VulkanCommandProcessor::IssueCopy_ReadbackResolvePath() {\n",
         "namespace {\n"
         "std::atomic<uint64_t> cd_copy_resolve_us{0}, cd_copy_drain_us{0}, cd_copy_batch_us{0}, cd_copy_count{0};\n"
         "}  // namespace\n\n"
         "bool VulkanCommandProcessor::IssueCopy_ReadbackResolvePath() {\n")
    edit(vcp, "  uint32_t written_address, written_length;\n"
              "  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,\n"
              "                                     written_length)) {\n"
              "    return false;\n"
              "  }\n",
         "  uint32_t written_address, written_length;\n"
         "  const auto cd_resolve_start = std::chrono::steady_clock::now();  // [cd]\n"
         "  const bool cd_resolved = render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,\n"
         "                                                         written_address, written_length);\n"
         "  cd_copy_resolve_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(\n"
         "                                   std::chrono::steady_clock::now() - cd_resolve_start).count()),\n"
         "                               std::memory_order_relaxed);\n"
         "  cd_copy_count.fetch_add(1, std::memory_order_relaxed);\n"
         "  if (!cd_resolved) {\n"
         "    return false;\n"
         "  }\n")
    edit(vcp, "  uint32_t read_index = write_index;\n"
              "  if (use_delayed_sync) {\n"
              "    read_index = 1 - write_index;\n"
              "  } else if (!AwaitAllQueueOperationsCompletion()) {\n"
              "    return true;\n"
              "  }\n",
         "  uint32_t read_index = write_index;\n"
         "  const auto cd_drain_start = std::chrono::steady_clock::now();  // [cd]\n"
         "  if (use_delayed_sync) {\n"
         "    read_index = 1 - write_index;\n"
         "  } else if (!AwaitAllQueueOperationsCompletion()) {\n"
         "    return true;\n"
         "  }\n"
         "  cd_copy_drain_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(\n"
         "                                 std::chrono::steady_clock::now() - cd_drain_start).count()),\n"
         "                             std::memory_order_relaxed);\n")
    edit(vcp, "    ++cd_batched_drains_;\n    ++cd_batched_reasons_[reason];\n    if (AwaitAllQueueOperationsCompletion()) {\n",
         "    ++cd_batched_drains_;\n    ++cd_batched_reasons_[reason];\n"
         "    const auto cd_batch_start = std::chrono::steady_clock::now();\n"
         "    const bool cd_drained = AwaitAllQueueOperationsCompletion();\n"
         "    cd_copy_batch_us.fetch_add(uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(\n"
         "                                   std::chrono::steady_clock::now() - cd_batch_start).count()),\n"
         "                               std::memory_order_relaxed);\n"
         "    if (cd_drained) {\n")
    edit(vcp, "  if (cd_now - cd_last >= std::chrono::seconds(10) && (cd_batched_readbacks_ || cd_batched_drains_)) {\n",
         "  if (cd_now - cd_last < std::chrono::seconds(10)) {\n"
         "    return;\n"
         "  }\n"
         "  REXGPU_INFO(\"cd copy: {} resolves in 10 s; resolve {:.1f} ms/s, per-resolve readback drains {:.1f} ms/s, \"\n"
         "              \"batched drains {:.1f} ms/s\",\n"
         "              cd_copy_count.exchange(0), cd_copy_resolve_us.exchange(0) / 10000.0,\n"
         "              cd_copy_drain_us.exchange(0) / 10000.0, cd_copy_batch_us.exchange(0) / 10000.0);\n"
         "  cd_last = cd_now;\n"
         "  if (cd_batched_readbacks_ || cd_batched_drains_) {\n")
    edit(vcp, "void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,\n"
              "                                       uint32_t frontbuffer_height) {\n"
              "  SCOPE_profile_cpu_f(\"gpu\");\n",
         "void VulkanCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,\n"
         "                                       uint32_t frontbuffer_height) {\n"
         "  SCOPE_profile_cpu_f(\"gpu\");\n"
         "  CdFlushDeferredReadbacks(kCdFlushSwap);  // [cd] (fw_readback_fixes.py)\n")


if __name__ == "__main__":
    main()
