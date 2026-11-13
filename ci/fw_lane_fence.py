#!/usr/bin/env python3
"""A fence the game can see, written by the GPU lane after its pending copies, not after the GPU thread waited (2026-11-11).

crackdown2_024 (console): the GPU command thread waited on the GPU lane 1.0-2.5 s in every 10 s. By call site (the
e2d86ef ELF symbolizes crackdown2_020's sites): EVENT_WRITE_SHD (157-424 waits / 10 s, 1.3-1.5 s, 4-10 ms each) and
WriteRegister's scratch-register write-back (390-584 / 10 s, 0.35-0.9 s). Both drain the lane only so that the copies it
still has (guest memory read into upload buffers) are done before the game can see the fence value: past the fence the
game may change that memory.

The lane runs its jobs in order on its own thread. So the fence store itself is pushed to the lane as a job: it lands
after every copy pushed before it, exactly the order the drain gave, and the game sees the value no earlier than before
(the old drain + store made it visible at the same point, the lane catching up). Only the GPU command thread no longer
waits; it goes on with the next packets.

Kept exact where the GPU thread itself reads or writes guest memory that such a fence may be in: before WAIT_REG_MEM's
and COND_WRITE's memory reads, before every store the old code made after a drain (those drain anyway), before kept
fence groups (fw_fence_fixes.py) are written, the lane is waited for if a lane fence is still pending. Used only when
there is nothing kept (no deferred readbacks or fence groups: those keep their path), when the lane is running and busy,
and when no trace is being written.

cvar cd_lane_fence_write (framework default false; the Crackdown 2 app turns it on).

    fw_lane_fence.py <framework>     (after fw_fence_fixes.py / fw_submit_thread.py)
"""
import pathlib
import sys

sdk = pathlib.Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"


def edit(rel, old, new, count=1):
    p = sdk / rel
    s = p.read_text(encoding="utf-8")
    if new in s:
        print(f"already {rel}")
        return
    n = s.count(old)
    if n != count:
        sys.exit(f"fw_lane_fence: {rel}: expected {count} of {old[:90]!r}, found {n}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print(f"patched {rel}")


h = "include/rex/graphics/command_processor.h"
edit(h, "  virtual bool CdDeferFenceWrite(uint32_t address, uint32_t value) { return false; }\n",
     "  virtual bool CdDeferFenceWrite(uint32_t address, uint32_t value) { return false; }\n"
     "  // [cd] (fw_lane_fence.py) kept readbacks or fence groups (the Vulkan backend's)\n"
     "  virtual bool CdHasKeptWork() const { return false; }\n"
     "  // [cd] (fw_lane_fence.py) a game-visible store pushed to the GPU lane after its pending copies; and the wait\n"
     "  // for the last such store before the GPU thread reads or writes guest memory itself\n"
     "  bool CdLaneFenceWrite(uint32_t address, uint32_t raw_value);\n"
     "  void CdLaneFenceSync();\n"
     "  uint64_t cd_lane_fence_job_ = 0, cd_lane_fence_writes_ = 0, cd_lane_fence_drains_ = 0;\n")

c = "src/graphics/command_processor.cpp"
edit(c, "bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,\n",
     "// [cd] (fw_lane_fence.py)\n"
     "REXCVAR_DEFINE_BOOL(cd_lane_fence_write, false, \"GPU\",\n"
     "                    \"[cd] A fence or scratch write-back the game sees is written by the GPU lane after the copies \"\n"
     "                    \"it still has, instead of the GPU thread waiting for them\");\n"
     "\n"
     "bool CommandProcessor::CdLaneFenceWrite(uint32_t address, uint32_t raw_value) {\n"
     "  if (!REXCVAR_GET(cd_lane_fence_write) || trace_writer_.is_open() || CdHasKeptWork()) {\n"
     "    return false;\n"
     "  }\n"
     "  GpuLane& lane = GpuLane::Get();\n"
     "  if (!lane.Active() || lane.Idle()) {\n"
     "    return false;  // (nothing to wait for: stored now, as before)\n"
     "  }\n"
     "  uint8_t* host = memory_->TranslatePhysical(address);\n"
     "  if (!host) {\n"
     "    return false;\n"
     "  }\n"
     "  lane.Run(\n"
     "      [](void* p, uint64_t v) { __atomic_store_n(static_cast<uint32_t*>(p), uint32_t(v), __ATOMIC_RELEASE); },\n"
     "      host, raw_value);\n"
     "  cd_lane_fence_job_ = lane.Pushed();\n"
     "  ++cd_lane_fence_writes_;\n"
     "  static auto cd_last = std::chrono::steady_clock::now();\n"
     "  const auto cd_now = std::chrono::steady_clock::now();\n"
     "  if (cd_now - cd_last >= std::chrono::seconds(10)) {\n"
     "    REXGPU_INFO(\"cd lane fence writes in 10 s: {} by the lane, {} waits before the GPU thread's own reads\",\n"
     "                cd_lane_fence_writes_, cd_lane_fence_drains_);\n"
     "    cd_lane_fence_writes_ = cd_lane_fence_drains_ = 0;\n"
     "    cd_last = cd_now;\n"
     "  }\n"
     "  return true;\n"
     "}\n"
     "\n"
     "void CommandProcessor::CdLaneFenceSync() {\n"
     "  if (cd_lane_fence_job_ && !GpuLane::Get().DoneUpTo(cd_lane_fence_job_)) {\n"
     "    ++cd_lane_fence_drains_;\n"
     "    GpuLane::Get().Drain();\n"
     "  }\n"
     "}\n"
     "\n"
     "bool CommandProcessor::ExecutePacketType3_EVENT_WRITE_SHD(memory::RingBuffer* reader,\n")

# EVENT_WRITE_SHD: the store to the lane when nothing is kept
edit(c, "  data_value = GpuSwap(data_value, endianness);\n  GpuLane::Get().Drain();  // [crackdown]\n"
        "  if (!CdDeferFenceWrite(address, data_value)) {  // [cd] (fw_fence_fixes.py)\n",
     "  data_value = GpuSwap(data_value, endianness);\n"
     "  if (CdLaneFenceWrite(address, data_value)) {  // [cd] (fw_lane_fence.py)\n"
     "    trace_writer_.WriteMemoryWrite(CpuToGpu(address), 4);\n"
     "    return true;\n"
     "  }\n"
     "  GpuLane::Get().Drain();  // [crackdown]\n"
     "  if (!CdDeferFenceWrite(address, data_value)) {  // [cd] (fw_fence_fixes.py)\n")

# scratch write-back
edit(c, "      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);\n      GpuLane::Get().Drain();  // [crackdown]\n",
     "      uint32_t mem_addr = scratch_addr + (scratch_reg * 4);\n"
     "      if (CdLaneFenceWrite(mem_addr, GpuSwap(value, xenos::Endian::k8in32))) {  // [cd] (fw_lane_fence.py)\n"
     "        return;\n"
     "      }\n"
     "      GpuLane::Get().Drain();  // [crackdown]\n")

# the GPU thread's own memory reads: WAIT_REG_MEM, COND_WRITE
edit(c, "    if (is_memory) {\n      value =\n          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));\n",
     "    if (is_memory) {\n"
     "      CdLaneFenceSync();  // [cd] (fw_lane_fence.py)\n"
     "      value =\n          *reinterpret_cast<uint32_t*>(memory_->TranslatePhysical(poll_reg_addr & ~uint32_t(0x3)));\n",
     )

edit(c, "    trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr), 4);\n"
        "    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));\n",
     "    trace_writer_.WriteMemoryRead(CpuToGpu(poll_reg_addr), 4);\n"
     "    CdLaneFenceSync();  // [cd] (fw_lane_fence.py)\n"
     "    value = memory::load<uint32_t>(memory_->TranslatePhysical(poll_reg_addr));\n")

v = "src/graphics/vulkan/command_processor.cpp"
edit(v, "bool VulkanCommandProcessor::CdDeferFenceWrite(uint32_t address, uint32_t value) {\n"
        "  // [cd] (fw_fence_fixes.py)\n",
     "bool VulkanCommandProcessor::CdDeferFenceWrite(uint32_t address, uint32_t value) {\n"
     "  // [cd] (fw_fence_fixes.py)\n"
     "  CdLaneFenceSync();  // [cd] (fw_lane_fence.py) a lane fence before it lands first\n")
edit(v, "void VulkanCommandProcessor::CdPollDeferredFences(bool wait) {\n"
        "  // [cd] (fw_fence_fixes.py) groups in order: each one's copies, then its fence values\n"
        "  if (cd_fence_groups_.empty()) {\n    return;\n  }\n",
     "void VulkanCommandProcessor::CdPollDeferredFences(bool wait) {\n"
     "  // [cd] (fw_fence_fixes.py) groups in order: each one's copies, then its fence values\n"
     "  if (cd_fence_groups_.empty()) {\n    return;\n  }\n"
     "  CdLaneFenceSync();  // [cd] (fw_lane_fence.py)\n")
vh = "include/rex/graphics/vulkan/command_processor.h"
edit(vh, "  bool CdDeferFenceWrite(uint32_t address, uint32_t value) override;\n",
     "  bool CdDeferFenceWrite(uint32_t address, uint32_t value) override;\n"
     "  bool CdHasKeptWork() const override {  // [cd] (fw_lane_fence.py)\n"
     "    return !cd_fence_groups_.empty() || !cd_pending_readbacks_.empty();\n"
     "  }\n")
