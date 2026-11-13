#!/usr/bin/env python3
"""A scratch-register write-back the GPU itself waits on is stored when the GPU passes it (2026-11-13).

Crackdown 2's D3D runs its per-frame callbacks (the swap scheduler sub_82919998, sub_8292F658) through a block in memory
(*(device+10900), 1FCA3000 on the console): the command stream writes SCRATCH_REG4/5 (callback, argument), SCRATCH_REG0
(the CPU mask) and SCRATCH_REG1 (the swap flag), the CP's scratch write-back (SCRATCH_UMSK/SCRATCH_ADDR) puts them into
the block, and the very next packets WAIT_REG_MEM until the block holds them, then INTERRUPT (Canary trace
4D5308BC view_stk_1: T0 0x57C/0x578, WAIT 1FCA6002 == 4, 1FCA6016 == arg, 1FCA6012 == fptr, INTERRUPT 4, WAIT +0 == 0;
the swap: T0 0x579 = 1, WAIT 1FCA6006 == 1, ..., WAIT 1FCA6006 == 0). On the Xbox 360 the CP writes a scratch register
back as it processes it, so those waits pass at once.

Here cd_fence_deferred (fw_fence_fixes.py) keeps a scratch write-back, like a fence, until the host GPU has finished
the work before it whenever readbacks are pending, and cd_lane_fence_write (fw_lane_fence.py) queues it behind the GPU
lane's copies. So each of those WAIT_REG_MEMs waited for the host GPU (or the lane) to catch up: crackdown2_027's
"last 10000 waits" line puts 1FCA3006 at 28.8 s and 1FCA3002 at 10.1 s, about 15 ms of a 30 ms frame, and the
crackdown2_028 profile has the GPU command thread in WAIT_REG_MEM for ~58% of its samples.

With cd_scratch_now: WAIT_REG_MEM remembers the memory words it polls (up to 16). A scratch write-back to one of those
words is stored at once (the hardware's order for its own wait); any write to the same word still queued on the lane is
landed first and any kept (deferred) write to that word is dropped, so an older value can never land after it. Every
other scratch write-back and every fence keep their paths. Pictures don't depend on it (proof: PC trace replays,
md5-identical off/on: Extras/tmp/replay/compare_scratch.sh).

cvar cd_scratch_now (framework default false; the Crackdown 2 app turns it on). The 10 s "cd scratch write-backs" line
counts them.

    fw_scratch_now.py <framework>     (after fw_fence_fixes.py and fw_lane_fence.py)
"""
import pathlib
import sys

sdk = pathlib.Path(sys.argv[1]) / "glue" / "rexglue-sdk-main"
base = "src/graphics/command_processor.cpp"
base_h = "include/rex/graphics/command_processor.h"
vk = "src/graphics/vulkan/command_processor.cpp"
vk_h = "include/rex/graphics/vulkan/command_processor.h"


def edit(rel, old, new, count=1):
    p = sdk / rel
    s = p.read_text(encoding="utf-8")
    if new in s:
        print(f"already {rel}")
        return
    n = s.count(old)
    if n != count:
        sys.exit(f"fw_scratch_now: {rel}: expected {count} of {old[:90]!r}, found {n}")
    p.write_text(s.replace(old, new), encoding="utf-8")
    print(f"patched {rel}")


edit(base,
     'REXCVAR_DEFINE_BOOL(cd_wait_writeback, true, "GPU",\n',
     'REXCVAR_DEFINE_BOOL(cd_scratch_now, false, "GPU",  // [cd] (fw_scratch_now.py)\n'
     '                    "A scratch-register write-back to memory a WAIT_REG_MEM has polled is stored when the GPU "\n'
     '                    "passes it, not kept until the host GPU or the GPU lane catches up (Crackdown 2: D3D\'s "\n'
     '                    "per-frame callback block)");\n'
     'REXCVAR_DEFINE_BOOL(cd_wait_writeback, true, "GPU",\n')

edit(base_h,
     "  virtual bool CdDeferFenceWrite(uint32_t address, uint32_t value) { return false; }\n",
     "  virtual bool CdDeferFenceWrite(uint32_t address, uint32_t value) { return false; }\n"
     "  // [cd] (fw_scratch_now.py) memory words WAIT_REG_MEM polls, and dropping kept writes to one\n"
     "  virtual void CdDropKeptWrites(uint32_t address) {}\n"
     "  bool CdSelfWaited(uint32_t address) const {\n"
     "    for (uint32_t i = 0; i < cd_self_waited_count_; ++i)\n"
     "      if (cd_self_waited_[i] == address) return true;\n"
     "    return false;\n"
     "  }\n"
     "  uint32_t cd_self_waited_[16] = {};\n"
     "  uint32_t cd_self_waited_count_ = 0;\n"
     "  uint64_t cd_scratch_now_writes_ = 0;\n")

# WAIT_REG_MEM: remember the memory word.
edit(base,
     "  uint32_t wait = reader->ReadAndSwap<uint32_t>();\n\n  bool is_memory = (wait_info & 0x10) != 0;\n",
     "  uint32_t wait = reader->ReadAndSwap<uint32_t>();\n\n  bool is_memory = (wait_info & 0x10) != 0;\n"
     "  if (is_memory && REXCVAR_GET(cd_scratch_now) && cd_self_waited_count_ < 16 &&  // [cd] (fw_scratch_now.py)\n"
     "      !CdSelfWaited(poll_reg_addr & ~uint32_t(0x3))) {\n"
     "    cd_self_waited_[cd_self_waited_count_++] = poll_reg_addr & ~uint32_t(0x3);\n"
     "    REXGPU_INFO(\"cd: WAIT_REG_MEM polls memory {:08X}: scratch write-backs there are stored at once\",\n"
     "                poll_reg_addr & ~uint32_t(0x3));\n"
     "  }\n")

# The scratch write-back: to a polled word, stored now.
edit(base,
     "      if (CdLaneFenceWrite(mem_addr, GpuSwap(value, xenos::Endian::k8in32))) {  // [cd] (fw_lane_fence.py)\n",
     "      if (REXCVAR_GET(cd_scratch_now) && CdSelfWaited(mem_addr)) {  // [cd] (fw_scratch_now.py)\n"
     "        CdLaneFenceSync();           // a write to it still on the lane lands first\n"
     "        CdDropKeptWrites(mem_addr);  // and a kept one never lands after this\n"
     "        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(mem_addr), value);\n"
     "        ++cd_scratch_now_writes_;\n"
     "        return;\n"
     "      }\n"
     "      if (CdLaneFenceWrite(mem_addr, GpuSwap(value, xenos::Endian::k8in32))) {  // [cd] (fw_lane_fence.py)\n")

edit(vk_h,
     "  bool CdDeferFenceWrite(uint32_t address, uint32_t value) override;\n",
     "  bool CdDeferFenceWrite(uint32_t address, uint32_t value) override;\n"
     "  void CdDropKeptWrites(uint32_t address) override;  // [cd] (fw_scratch_now.py)\n")

edit(vk,
     "bool VulkanCommandProcessor::CdDeferFenceWrite(uint32_t address, uint32_t value) {\n",
     "void VulkanCommandProcessor::CdDropKeptWrites(uint32_t address) {\n"
     "  // [cd] (fw_scratch_now.py) a newer value is stored now: kept older ones to the same word go\n"
     "  for (CdFenceGroup& group : cd_fence_groups_) {\n"
     "    auto& w = group.writes;\n"
     "    w.erase(std::remove_if(w.begin(), w.end(), [address](const auto& e) { return e.first == address; }), w.end());\n"
     "  }\n"
     "}\n\n"
     "bool VulkanCommandProcessor::CdDeferFenceWrite(uint32_t address, uint32_t value) {\n")

# The count, with the readback report every 10 s.
edit(vk,
     '    REXGPU_INFO("cd readback batched: {} resolves read back in {} GPU drains in 10 s, at:{}", cd_batched_readbacks_,\n',
     '    if (cd_scratch_now_writes_) {  // [cd] (fw_scratch_now.py)\n'
     '      REXGPU_INFO("cd scratch write-backs stored at once in 10 s: {} ({} polled words)", cd_scratch_now_writes_,\n'
     '                  cd_self_waited_count_);\n'
     '      cd_scratch_now_writes_ = 0;\n'
     '    }\n'
     '    REXGPU_INFO("cd readback batched: {} resolves read back in {} GPU drains in 10 s, at:{}", cd_batched_readbacks_,\n')
