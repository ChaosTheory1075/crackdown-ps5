#!/usr/bin/env python3
"""The ring's read pointer written back before a WAIT_REG_MEM waits (from Saints Row: ReInstated's ci/patch_framework.py,
"The ring's read pointer was written back for the game only once a whole primary batch was done").

Crackdown 2 (crackdown2_014, the "cd perf" line): the GPU thread spent 21.8 ms of a 34.5 ms frame in WAIT_REG_MEM
(32 waits a frame): the game's D3D waits for ring space (the read pointer) before writing what the GPU's WAIT_REG_MEM
in the same batch waits for, and the read pointer only reached the game at the end of the batch. Saints Row 2 ran at
8 fps from the same thing. cd_wait_writeback (default true) turns it off.
And Prototype's WAIT_REG_MEM spin (proto-ps5 4e11118): it polls with yields for up to cd_wait_spin_us (2000 us; 0 = the
old 16 polls) before sleeping 1 ms at a time, and names the five places waited on longest every ~10000 waits.

    fw_ring_fixes.py <framework>     (after patch_framework.py and fw_perf_fixes.py)
"""
import pathlib
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        sys.exit(f"fw_ring_fixes: expected {count} of {old!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    fw = pathlib.Path(sys.argv[1])
    sdk = fw / "glue" / "rexglue-sdk-main"
    edit(sdk / "src" / "graphics" / "command_processor.cpp",
         'REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n',
         'REXCVAR_DEFINE_BOOL(vsync, true, "GPU", "Enable vertical sync");\n'
         'REXCVAR_DEFINE_BOOL(cd_wait_writeback, true, "GPU",\n'
         '                    "A WAIT_REG_MEM that has to wait first writes back how far the ring has been read "\n'
         '                    "(Saints Row 2 / Crackdown 2: the game waits for that before writing what the GPU waits on)");\n')
    # The ring's read pointer was written back for the game only once a whole primary batch was
    # done. The Xbox 360's CP writes it back as it fetches. Saints Row 2's D3D waits for that
    # progress (ring space) before writing what the GPU's WAIT_REG_MEM in the same batch waits
    # for: each side waited on the other until a timeout, and gameplay ran at 8 fps with the GPU
    # thread asleep in WAIT_REG_MEM. Now a WAIT_REG_MEM that has to wait writes back where the
    # primary ring has been read to first.
    cph = sdk / "include" / "rex" / "graphics" / "command_processor.h"
    edit(cph, "  uint32_t read_ptr_writeback_ptr_ = 0;\n",
         "  uint32_t read_ptr_writeback_ptr_ = 0;\n"
         "  memory::RingBuffer* cd_primary_reader_ = nullptr;  // [cd] (while a primary batch runs)\n")
    cpc = sdk / "src" / "graphics" / "command_processor.cpp"
    edit(cpc,
         "  reader.set_read_offset(read_index * sizeof(uint32_t));\n"
         "  reader.set_write_offset(write_index * sizeof(uint32_t));\n"
         "  do {\n"
         "    if (!ExecutePacket(&reader)) {\n"
         "      // This probably should be fatal - but we're going to continue anyways.\n",
         "  reader.set_read_offset(read_index * sizeof(uint32_t));\n"
         "  reader.set_write_offset(write_index * sizeof(uint32_t));\n"
         "  cd_primary_reader_ = &reader;  // [cd]\n"
         "  do {\n"
         "    if (!ExecutePacket(&reader)) {\n"
         "      // This probably should be fatal - but we're going to continue anyways.\n")
    edit(cpc,
         "  } while (reader.read_count());\n\n  OnPrimaryBufferEnd();\n",
         "  } while (reader.read_count());\n  cd_primary_reader_ = nullptr;  // [cd]\n\n  OnPrimaryBufferEnd();\n")
    edit(cpc,
         "  constexpr uint32_t kYieldPollCount = 16;\n"
         "  uint32_t unmatched_poll_count = 0;\n"
         "  bool matched = false;\n",
         "  constexpr uint32_t kYieldPollCount = 16;\n"
         "  uint32_t unmatched_poll_count = 0;\n"
         "  bool matched = false;\n"
         "  bool cd_wrote_back = false;  // [cd]\n")
    edit(cpc,
         "    if (!matched) {\n"
         "      PollBackgroundWork();\n",
         "    if (!matched) {\n"
         "      // [cd] the game sees how far the ring has been read before it's waited on\n"
         "      if (REXCVAR_GET(cd_wait_writeback) && !cd_wrote_back && cd_primary_reader_ && read_ptr_writeback_ptr_) {\n"
         "        cd_wrote_back = true;\n"
         "        static std::atomic<uint64_t> cd_writebacks{0};\n"
         "        if (cd_writebacks.fetch_add(1) == 0)\n"
         "          REXGPU_INFO(\"cd: WAIT_REG_MEM writes the read pointer back before waiting\");\n"
         "        GpuLane::Get().Drain();\n"
         "        memory::store_and_swap<uint32_t>(memory_->TranslatePhysical(read_ptr_writeback_ptr_),\n"
         "                                         uint32_t(cd_primary_reader_->read_offset() / sizeof(uint32_t)));\n"
         "      }\n"
         "      PollBackgroundWork();\n")
    cpc = sdk / "src" / "graphics" / "command_processor.cpp"

    # [cd] WAIT_REG_MEM noticed its value late: past the first 16 polls it slept 1 ms at a time,
    # woken early only by a vblank (NotifyGuestProgress), so a value the game wrote just after a
    # poll waited out the rest of that millisecond. Prototype 2 (proto2_015, proto-ps5 4e11118): 27 waits a frame,
    # 10.5 ms of the GPU thread's 16.7. Now it keeps polling (with yields: the GPU thread has its
    # own CPU) for up to cd_wait_spin_us before sleeping, so the wait ends when the value is
    # written. Same result, sooner. And where the time goes, in the log every ~10000 waits: the
    # five places waited on longest (register or memory address, how often, how long).
    edit(cpc, '                    "(Saints Row 2 / Crackdown 2: the game waits for that before writing what the GPU waits on)");\n',
         '                    "(Saints Row 2 / Crackdown 2: the game waits for that before writing what the GPU waits on)");\n'
         'REXCVAR_DEFINE_INT32(cd_wait_spin_us, 2000, "GPU",  // [cd]\n'
         '                     "WAIT_REG_MEM keeps polling (yielding) this long before it sleeps 1 ms at a time "\n'
         '                     "(0: the 16 polls of before)");\n'
         '#include <chrono>  // [cd]\n'
         'namespace {\n'
         'struct CdWaitSite {  // (the GPU thread\'s only)\n'
         '  uint32_t addr = 0;\n'
         '  bool memory = false;\n'
         '  uint64_t count = 0, ns = 0;\n'
         '};\n'
         'CdWaitSite cd_wait_sites[16];\n'
         'uint64_t cd_wait_total = 0;\n'
         'void CdNoteWait(uint32_t addr, bool memory, uint64_t ns) {\n'
         '  CdWaitSite* s = nullptr;\n'
         '  for (auto& w : cd_wait_sites) {\n'
         '    if (w.count && w.addr == addr && w.memory == memory) { s = &w; break; }\n'
         '    if (!w.count) { s = &w; s->addr = addr, s->memory = memory; break; }\n'
         '  }\n'
         '  if (s) ++s->count, s->ns += ns;\n'
         '  if (++cd_wait_total % 10000) return;\n'
         '  CdWaitSite top[16];\n'
         '  std::copy(std::begin(cd_wait_sites), std::end(cd_wait_sites), top);\n'
         '  std::sort(std::begin(top), std::end(top), [](const CdWaitSite& a, const CdWaitSite& b) { return a.ns > b.ns; });\n'
         '  std::string out;\n'
         '  for (int i = 0; i < 5 && top[i].count; ++i)\n'
         '    out += fmt::format("{}{} {:08X}: {} waits, {:.1f} ms", i ? "; " : "", top[i].memory ? "memory" : "register",\n'
         '                       top[i].addr, top[i].count, top[i].ns / 1e6);\n'
         '  REXGPU_INFO("cd: WAIT_REG_MEM, the last 10000 waits that waited: {}", out);\n'
         '  for (auto& w : cd_wait_sites) w = CdWaitSite{};\n'
         '}\n'
         '}  // namespace\n')
    edit(cpc, "  bool cd_wrote_back = false;  // [cd]\n",
         "  bool cd_wrote_back = false;  // [cd]\n"
         "  const auto cd_wait_start = std::chrono::steady_clock::now();\n"
         "  const auto cd_spin = std::chrono::microseconds(REXCVAR_GET(cd_wait_spin_us));\n"
         "  bool cd_waited = false;\n")
    edit(cpc,
         "        if (unmatched_poll_count < kYieldPollCount || !REXCVAR_GET(vsync)) {\n",
         "        cd_waited = true;\n"
         "        if (unmatched_poll_count < kYieldPollCount || !REXCVAR_GET(vsync) ||\n"
         "            std::chrono::steady_clock::now() - cd_wait_start < cd_spin) {\n")
    edit(cpc,
         "  } while (!matched);\n\n  return true;\n}\n\nbool CommandProcessor::ExecutePacketType3_REG_RMW(",
         "  } while (!matched);\n"
         "  if (cd_waited)\n"
         "    CdNoteWait(poll_reg_addr, is_memory,\n"
         "                  uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(\n"
         "                               std::chrono::steady_clock::now() - cd_wait_start).count()));\n"
         "\n  return true;\n}\n\nbool CommandProcessor::ExecutePacketType3_REG_RMW(")

    # [cd] Which resolves are read back, every 10 s (address, size, count, and the mode): with readback_resolve
    # "full" (Crackdown 2, for the green orbs) each one is a full GPU drain (AwaitAllQueueOperationsCompletion),
    # crackdown2_014's 10 ms a frame of fence waits. The list says which ones the CPU needs in the same frame.
    vcp = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"
    edit(vcp,
         "  uint32_t read_index = write_index;\n  if (use_delayed_sync) {\n",
         "  {  // [cd] the readback log\n"
         "    static std::unordered_map<uint64_t, uint32_t> cd_read;\n"
         "    static uint64_t cd_read_total = 0;\n"
         "    static auto cd_read_last = std::chrono::steady_clock::now();\n"
         "    ++cd_read[(uint64_t(written_address) << 32) | written_length];\n"
         "    ++cd_read_total;\n"
         "    const auto cd_now = std::chrono::steady_clock::now();\n"
         "    if (cd_now - cd_read_last >= std::chrono::seconds(10)) {\n"
         "      std::vector<std::pair<uint32_t, uint64_t>> top;\n"
         "      for (const auto& e : cd_read) top.emplace_back(e.second, e.first);\n"
         "      std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });\n"
         "      std::string list;\n"
         "      for (size_t i = 0; i < top.size() && i < 16; ++i)\n"
         "        list += fmt::format(\" {:08X}+{}KBx{}\", uint32_t(top[i].second >> 32),\n"
         "                            (uint32_t(top[i].second) + 1023) >> 10, top[i].first);\n"
         "      REXGPU_INFO(\"cd readback: {} resolves read back in 10 s ({}), {} distinct:{}\", cd_read_total,\n"
         "                  use_delayed_sync ? \"delayed\" : \"same-frame, a GPU drain each\", cd_read.size(), list);\n"
         "      cd_read.clear();\n"
         "      cd_read_total = 0;\n"
         "      cd_read_last = cd_now;\n"
         "    }\n"
         "  }\n"
         "  uint32_t read_index = write_index;\n  if (use_delayed_sync) {\n")

    # [cd] cd_readback_writable_only (default false = as before): Xenia Canary's check, ported. Canary
    # (vulkan_command_processor.cc IssueCopy) skips the CPU readback of a resolve whose destination isn't committed,
    # CPU-writable memory ("Skip all the GPU readback work if the destination memory isn't writable"): such a
    # destination is a GPU-only surface the game never reads on the CPU, and with readback_resolve "full" every one of
    # them cost a GPU drain (crackdown2_015: ~4.5 a frame, 10.8 ms of fences). Nothing the CPU can read is skipped.
    # Every 10 s: how many it skipped and their addresses.
    edit(vcp,
         "  const int32_t readback_max_bytes = REXCVAR_GET(readback_resolve_max_bytes);\n"
         "  if (readback_mode == ReadbackResolveMode::kDisabled ||\n"
         "      (readback_max_bytes > 0 && written_length > uint32_t(readback_max_bytes))) {\n"
         "    return true;\n"
         "  }\n",
         "  const int32_t readback_max_bytes = REXCVAR_GET(readback_resolve_max_bytes);\n"
         "  if (readback_mode == ReadbackResolveMode::kDisabled ||\n"
         "      (readback_max_bytes > 0 && written_length > uint32_t(readback_max_bytes))) {\n"
         "    return true;\n"
         "  }\n"
         "  if (REXCVAR_GET(cd_readback_writable_only)) {  // [cd] Canary's: not CPU-writable, no readback\n"
         "    bool cd_writable = false;\n"
         "    if (auto* cd_heap = memory_->GetPhysicalHeap()) {\n"
         "      memory::HeapAllocationInfo cd_info;\n"
         "      if (cd_heap->QueryRegionInfo(written_address, &cd_info) &&\n"
         "          (cd_info.state & memory::kMemoryAllocationCommit) && (cd_info.protect & memory::kMemoryProtectWrite) &&\n"
         "          uint64_t(written_address) + written_length <= uint64_t(cd_info.base_address) + cd_info.region_size)\n"
         "        cd_writable = true;\n"
         "    }\n"
         "    static uint32_t cd_kept = 0, cd_skipped = 0;\n"
         "    static std::unordered_map<uint64_t, uint32_t> cd_skipped_at;\n"
         "    static auto cd_last = std::chrono::steady_clock::now();\n"
         "    if (cd_writable) {\n"
         "      ++cd_kept;\n"
         "    } else {\n"
         "      ++cd_skipped;\n"
         "      ++cd_skipped_at[(uint64_t(written_address) << 32) | written_length];\n"
         "    }\n"
         "    if (std::chrono::steady_clock::now() - cd_last >= std::chrono::seconds(10)) {\n"
         "      std::string list;\n"
         "      size_t shown = 0;\n"
         "      for (const auto& e : cd_skipped_at)\n"
         "        if (shown++ < 8) list += fmt::format(\" {:08X}+{}KBx{}\", uint32_t(e.first >> 32),\n"
         "                                             (uint32_t(e.first) + 1023) >> 10, e.second);\n"
         "      REXGPU_INFO(\"cd readback writable-only: {} read back, {} skipped (not CPU-writable) in 10 s:{}\", cd_kept,\n"
         "                  cd_skipped, list);\n"
         "      cd_kept = cd_skipped = 0;\n"
         "      cd_skipped_at.clear();\n"
         "      cd_last = std::chrono::steady_clock::now();\n"
         "    }\n"
         "    if (!cd_writable) return true;\n"
         "  }\n")
    edit(vcp, "namespace rex::graphics::vulkan {\n",
         "REXCVAR_DEFINE_BOOL(cd_readback_writable_only, false, \"GPU\",  // [cd]\n"
         "                    \"No CPU readback of a resolve into memory the CPU can't write (Xenia Canary's check)\");\n"
         "\nnamespace rex::graphics::vulkan {\n")
    print("Crackdown: cd_readback_writable_only (Canary's readback check, default off)")


if __name__ == "__main__":
    main()
