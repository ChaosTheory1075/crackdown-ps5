#!/usr/bin/env python3
"""The game's GPU fences written when the GPU really gets there, not by stopping the GPU thread until the GPU
is idle (cvar cd_fence_deferred, default false; on in crackdown2.toml).

What it fixes (crackdown2_017, cd_readback_batched on): 400-570 EVENT_WRITE_SHD drains in 10 s, about two a
frame, 0.8-0.9 ms each, 9-13 ms of "GPU fences" a frame. EVENT_WRITE_SHD is the game's fence: the GPU writes
a value to memory when everything before it has finished, and the game reads its resolved data back once it
sees that value. Xenia writes the value as soon as the command processor reaches the packet, so the batched
readback had to make the data land first: a full wait for the GPU (AwaitAllQueueOperationsCompletion), the
GPU thread doing nothing meanwhile.

Here, when readbacks are pending at an EVENT_WRITE_SHD, the open submission is handed to the GPU (no wait),
and the fence value is kept with those readbacks as a group tied to that submission. The GPU thread goes on
recording. A group completes (copies into guest memory, then its fence values, in order) as soon as its
submission's fence has signalled: polled when each primary buffer starts, each time WAIT_REG_MEM polls, and
in the idle loop. The game never sees a fence value before the data it guards: the same rule the Xbox 360
GPU keeps, so not the rejected previous-frame ("fast") mode.

Ordering kept with the CPU's other views of GPU progress: scratch / interrupt / REG_TO_MEM / MEM_WRITE /
COND_WRITE / EVENT_WRITE_SHD without readbacks complete every older group first (blocking if needed); the
read pointer and the swap don't (on the 360 the read pointer runs ahead of fences). A resolve that would
reuse the readback buffer half a group still holds completes that group first.
"""
import sys
from pathlib import Path

sdk = Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"


def edit(rel, old, new, count=1):
    p = sdk / rel
    s = p.read_text(encoding="utf-8")
    n = s.count(old)
    if n != count:
        sys.exit(f"fw_fence_fixes: {rel}: expected {count} of {old[:90]!r}, found {n}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print(f"patched {rel}")


base_h = "include/rex/graphics/command_processor.h"
if "CdDeferFenceWrite" in (sdk / base_h).read_text(encoding="utf-8"):
    print("fw_fence_fixes: already applied")
    sys.exit(0)

edit(base_h, "  virtual void CdFlushDeferredReadbacks(CdFlushReason reason) {}\n",
     "  virtual void CdFlushDeferredReadbacks(CdFlushReason reason) {}\n"
     "  // [cd] (fw_fence_fixes.py, cd_fence_deferred) an EVENT_WRITE_SHD fence value kept until the GPU has done\n"
     "  // the readbacks before it: true when it's kept (written later by CdPollDeferredFences)\n"
     "  virtual bool CdDeferFenceWrite(uint32_t address, uint32_t value) { return false; }\n"
     "  // [cd] the kept fences whose GPU work is done written (wait: all of them, waiting for the GPU)\n"
     "  virtual void CdPollDeferredFences(bool wait) {}\n")

base = "src/graphics/command_processor.cpp"
# EVENT_WRITE_SHD: kept with its readbacks instead of a full GPU wait.
edit(base,
     "  GpuLane::Get().Drain();  // [crackdown]\n"
     "  CdFlushDeferredReadbacks(kCdFlushEventWriteShd);  // [cd] (fw_readback_fixes.py)\n"
     "  memory::store(memory_->TranslatePhysical(address), data_value);\n",
     "  GpuLane::Get().Drain();  // [crackdown]\n"
     "  if (!CdDeferFenceWrite(address, data_value)) {  // [cd] (fw_fence_fixes.py)\n"
     "    CdFlushDeferredReadbacks(kCdFlushEventWriteShd);  // [cd] (fw_readback_fixes.py)\n"
     "    memory::store(memory_->TranslatePhysical(address), data_value);\n"
     "  }\n")
# Polled when a primary buffer starts executing.
edit(base,
     "    rex::ps5diag::Add(rex::ps5diag::kPrimaryBuffers);\n"
     "    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);\n",
     "    rex::ps5diag::Add(rex::ps5diag::kPrimaryBuffers);\n"
     "    CdPollDeferredFences(false);  // [cd] (fw_fence_fixes.py)\n"
     "    read_ptr_index_ = ExecutePrimaryBuffer(read_ptr_index_, write_ptr_index);\n")
# Polled in the idle loop (the game may be spinning on the fence while the ring is empty), 1 ms waits then.
edit(base,
     "          rex::thread::Wait(write_ptr_index_event_.get(), false,\n"
     "                            std::chrono::milliseconds(poll_background_work ? 1 : 5));\n"
     "        }\n",
     "          rex::thread::Wait(write_ptr_index_event_.get(), false,\n"
     "                            std::chrono::milliseconds(poll_background_work || cd_fences_kept_ ? 1 : 5));\n"
     "        }\n"
     "        if (cd_fences_kept_) CdPollDeferredFences(false);  // [cd] (fw_fence_fixes.py)\n")
# WAIT_REG_MEM: polled on every unmatched poll (the GPU may wait on the fence itself).
edit(base,
     "    if (!matched) {\n"
     "      // [cd] the game sees how far the ring has been read before it's waited on\n",
     "    if (!matched && cd_fences_kept_) {\n"
     "      CdPollDeferredFences(false);  // [cd] (fw_fence_fixes.py)\n"
     "    }\n"
     "    if (!matched) {\n"
     "      // [cd] the game sees how far the ring has been read before it's waited on\n")
edit(base_h, "  virtual void CdPollDeferredFences(bool wait) {}\n",
     "  virtual void CdPollDeferredFences(bool wait) {}\n"
     "  bool cd_fences_kept_ = false;  // [cd] fences are being kept (set by the backend)\n")

vk_h = "include/rex/graphics/vulkan/command_processor.h"
edit(vk_h, "  std::vector<CdPendingReadback> cd_pending_readbacks_;\n",
     "  std::vector<CdPendingReadback> cd_pending_readbacks_;\n"
     "  // [cd] (fw_fence_fixes.py) readbacks and the fence values after them, waiting for one submission\n"
     "  struct CdFenceGroup {\n"
     "    uint64_t submission;\n"
     "    std::vector<CdPendingReadback> readbacks;\n"
     "    std::vector<std::pair<uint32_t, uint32_t>> writes;  // (physical address, value as stored)\n"
     "  };\n"
     "  std::vector<CdFenceGroup> cd_fence_groups_;\n"
     "  bool CdDeferFenceWrite(uint32_t address, uint32_t value) override;\n"
     "  void CdPollDeferredFences(bool wait) override;\n"
     "  void CdCopyReadbacks(const std::vector<CdPendingReadback>& readbacks);\n")

vk = "src/graphics/vulkan/command_processor.cpp"
edit(vk,
     "REXCVAR_DEFINE_BOOL(cd_readback_batched, false, \"GPU\",  // [cd] (fw_readback_fixes.py)\n",
     "REXCVAR_DEFINE_BOOL(cd_fence_deferred, false, \"GPU\",  // [cd] (fw_fence_fixes.py)\n"
     "                    \"With cd_readback_batched: an EVENT_WRITE_SHD fence after readbacks is written when the GPU \"\n"
     "                    \"has done them, not after stopping the GPU thread until the GPU is idle\");\n"
     "REXCVAR_DEFINE_BOOL(cd_readback_batched, false, \"GPU\",  // [cd] (fw_readback_fixes.py)\n")

# The batched flush: older kept groups first (in order), except at the read pointer, the read pointer write-back
# before a wait, and the swap; the copy loop moved into CdCopyReadbacks.
edit(vk,
     "void VulkanCommandProcessor::CdFlushDeferredReadbacks(CdFlushReason reason) {\n"
     "  // [cd] (fw_readback_fixes.py)\n"
     "  if (!cd_pending_readbacks_.empty()) {\n",
     "void VulkanCommandProcessor::CdFlushDeferredReadbacks(CdFlushReason reason) {\n"
     "  // [cd] (fw_readback_fixes.py)\n"
     "  if (!cd_fence_groups_.empty()) {\n"
     "    // [cd] (fw_fence_fixes.py) what the CPU sees here comes after the kept fences (not the read pointer:\n"
     "    // on the 360 it runs ahead of fences; not the swap)\n"
     "    const bool ordered = reason != kCdFlushReadPointer && reason != kCdFlushWaitWriteback && reason != kCdFlushSwap;\n"
     "    CdPollDeferredFences(ordered);\n"
     "  }\n"
     "  if (!cd_pending_readbacks_.empty()) {\n")

# Helpers, after the flush function.
edit(vk,
     "void VulkanCommandProcessor::CdFlushDeferredReadbacks(CdFlushReason reason) {\n",
     r'''void VulkanCommandProcessor::CdCopyReadbacks(const std::vector<CdPendingReadback>& readbacks) {
  // [cd] (fw_fence_fixes.py) the GPU has done these copies: into guest memory
  const ui::vulkan::VulkanDevice* const vulkan_device = GetVulkanDevice();
  const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  for (const CdPendingReadback& p : readbacks) {
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

bool VulkanCommandProcessor::CdDeferFenceWrite(uint32_t address, uint32_t value) {
  // [cd] (fw_fence_fixes.py)
  if (!REXCVAR_GET(cd_fence_deferred) || device_lost_) {
    return false;
  }
  if (cd_pending_readbacks_.empty()) {
    if (cd_fence_groups_.empty()) {
      return false;  // nothing before it to wait for: written now, as always
    }
    // after a kept fence: kept too, in order
    cd_fence_groups_.back().writes.emplace_back(address, value);
    return true;
  }
  // The work so far (with the readbacks' copies) handed to the GPU, not waited for.
  if (submission_open_) {
    EndSubmission(false);
  }
  const uint64_t submission = GetCurrentSubmission() - 1;
  if (submission <= GetCompletedSubmission()) {
    // (nothing could be submitted, or it's already done: as before)
    return false;
  }
  CdFenceGroup group;
  group.submission = submission;
  group.readbacks = std::move(cd_pending_readbacks_);
  cd_pending_readbacks_.clear();
  group.writes.emplace_back(address, value);
  cd_fence_groups_.push_back(std::move(group));
  cd_fences_kept_ = true;
  return true;
}

void VulkanCommandProcessor::CdPollDeferredFences(bool wait) {
  // [cd] (fw_fence_fixes.py) groups in order: each one's copies, then its fence values
  if (cd_fence_groups_.empty()) {
    return;
  }
  CheckSubmissionFenceAndDeviceLoss(wait ? cd_fence_groups_.back().submission : 0);
  size_t done = 0;
  for (; done < cd_fence_groups_.size(); ++done) {
    CdFenceGroup& group = cd_fence_groups_[done];
    if (!device_lost_ && GetCompletedSubmission() < group.submission) {
      break;
    }
    if (!device_lost_) {
      CdCopyReadbacks(group.readbacks);
    }
    for (const auto& w : group.writes) {
      memory::store(memory_->TranslatePhysical(w.first), w.second);
    }
  }
  if (done) {
    cd_fence_groups_.erase(cd_fence_groups_.begin(), cd_fence_groups_.begin() + done);
  }
  cd_fences_kept_ = !cd_fence_groups_.empty();
}

void VulkanCommandProcessor::CdFlushDeferredReadbacks(CdFlushReason reason) {
''')

# A resolve about to reuse the readback buffer half a kept group still holds: that group first.
edit(vk,
     "    cd_pending_readbacks_.push_back({resolve_key, write_index, written_address, written_length});\n",
     "    for (const CdFenceGroup& group : cd_fence_groups_) {  // [cd] (fw_fence_fixes.py)\n"
     "      bool held = false;\n"
     "      for (const CdPendingReadback& p : group.readbacks) held |= p.key == resolve_key && p.index == write_index;\n"
     "      if (held) {\n"
     "        CdPollDeferredFences(true);\n"
     "        break;\n"
     "      }\n"
     "    }\n"
     "    cd_pending_readbacks_.push_back({resolve_key, write_index, written_address, written_length});\n")

# ---- 2026-11-11: what still made the GPU thread wait for nearly every kept group (crackdown2_020: the GPU thread
# waited for the submit thread 734 times in 10 s, 7.7 ms a frame, with cd_fence_deferred on) ----
# A scratch register's write-back (SCRATCH_UMSK) is the GPU telling the CPU how far it got, as EVENT_WRITE_SHD
# does: on the console it lands when the GPU reaches it in the command stream. Kept with the fences (in order)
# instead of forcing every kept group through the GPU first.
edit(base,
     "      GpuLane::Get().Drain();  // [crackdown]\n"
     "      CdFlushDeferredReadbacks(kCdFlushScratch);  // [cd] (fw_readback_fixes.py)\n"
     "      memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);\n",
     "      GpuLane::Get().Drain();  // [crackdown]\n"
     "      // [cd] (fw_fence_fixes.py) a scratch write-back is a fence too: kept, in order, until the GPU is there\n"
     "      if (!CdDeferFenceWrite(mem_addr, GpuSwap(value, xenos::Endian::k8in32))) {\n"
     "        CdFlushDeferredReadbacks(kCdFlushScratch);  // [cd] (fw_readback_fixes.py)\n"
     "        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);\n"
     "      }\n")
# The 10 s report: what forced a wait for kept groups (only counted when the GPU wasn't done yet).
edit(vk_h, "  uint64_t cd_batched_reasons_[kCdFlushReasons] = {};\n",
     "  uint64_t cd_batched_reasons_[kCdFlushReasons] = {};\n"
     "  uint64_t cd_fence_wait_reasons_[kCdFlushReasons] = {};  // [cd] forced waits for kept groups, by flush reason\n"
     "  uint64_t cd_fence_wait_reuse_ = 0;  // [cd] ... and by a resolve reusing a readback buffer half\n")
edit(vk,
     "    const bool ordered = reason != kCdFlushReadPointer && reason != kCdFlushWaitWriteback && reason != kCdFlushSwap;\n"
     "    CdPollDeferredFences(ordered);\n",
     "    const bool ordered = reason != kCdFlushReadPointer && reason != kCdFlushWaitWriteback && reason != kCdFlushSwap;\n"
     "    if (ordered && GetCompletedSubmission() < cd_fence_groups_.back().submission) ++cd_fence_wait_reasons_[reason];\n"
     "    CdPollDeferredFences(ordered);\n")
edit(vk,
     "      if (held) {\n"
     "        CdPollDeferredFences(true);\n",
     "      if (held) {\n"
     "        if (GetCompletedSubmission() < cd_fence_groups_.back().submission) ++cd_fence_wait_reuse_;\n"
     "        CdPollDeferredFences(true);\n")
edit(vk,
     "  if (cd_batched_readbacks_ || cd_batched_drains_) {\n",
     "  {  // [cd] (fw_fence_fixes.py) what made the GPU thread wait for kept fence groups\n"
     "    static const char* const kWaitNames[kCdFlushReasons] = {\"read pointer\", \"scratch\", \"interrupt\",\n"
     "        \"wait write-back\", \"REG_TO_MEM\", \"MEM_WRITE\", \"COND_WRITE\", \"EVENT_WRITE_SHD\", \"swap\"};\n"
     "    std::string waits;\n"
     "    for (uint32_t i = 0; i < kCdFlushReasons; ++i) {\n"
     "      if (cd_fence_wait_reasons_[i]) waits += fmt::format(\" {} {}\", kWaitNames[i], cd_fence_wait_reasons_[i]);\n"
     "      cd_fence_wait_reasons_[i] = 0;\n"
     "    }\n"
     "    REXGPU_INFO(\"cd fence waits forced in 10 s:{}; readback buffer reuse {}\", waits.empty() ? \" none\" : waits,\n"
     "                cd_fence_wait_reuse_);\n"
     "    cd_fence_wait_reuse_ = 0;\n"
     "  }\n"
     "  if (cd_batched_readbacks_ || cd_batched_drains_) {\n")
