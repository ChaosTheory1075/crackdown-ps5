#!/usr/bin/env python3
"""The XMA decoder as Xenia Canary's (from Saints Row: ReInstated's ci/patch_framework.py "XMA decoder: where the
framework's decoder differs" and "XMA loops"). Crackdown 2 (crackdown2_013): 57 "XMA context N: error N" warnings
and the XMA Decoder thread ~94% busy.
- Work: a decoded frame is delivered before stopping on an error or on running out of input, and a pass that made no
  progress (waiting for the game's next buffer) stops without an error (it set error_status 4, which then stuck).
- GetNextPacket: a packet past the current buffer is (index - count) of the next one, not always its first.
- GetNextPacketReadOffset: a continuation-only packet follows its own skip count to this stream's next packet (the
  next packet in memory belongs to another stream in an interleaved buffer).
- No "invalid loop interval" check (Xenia has none: it rejected legal loop data and stopped looping voices), and the
  loop-start skip applied only when shorter than the frame.
cd_xma_canary (default true): false is the decoder as it was.

    fw_xma_fixes.py <framework>     (after fw_perf_fixes.py)
"""
import pathlib
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        sys.exit(f"fw_xma_fixes: expected {count} of {old!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    fw = pathlib.Path(sys.argv[1])
    sdk = fw / "glue" / "rexglue-sdk-main"
    xc = sdk / "src" / "audio" / "xma_context.cpp"
    edit(xc, "#include <rex/logging.h>\n",
         "#include <rex/logging.h>\n#include <rex/cvar.h>  // [cd]\n")
    edit(xc, "namespace rex::audio {\n",
         "REXCVAR_DEFINE_BOOL(cd_xma_canary, true, \"Audio\",  // [cd]\n"
         "                    \"The XMA decoder as Xenia Canary's: frames drained before stopping, next-buffer packet \"\n"
         "                    \"index, continuation skip counts, no invalid-loop check (Saints Row's fix)\");\n\n"
         "namespace rex::audio {\n", count=1)
    edit(xc,
         "    // Invalid packets must not leave the decoder worker spinning forever.\n",
         "    if (REXCVAR_GET(cd_xma_canary)) {\n"
         "      // [cd] As Xenia: finish delivering a decoded frame before stopping on an error or on running out\n"
         "      // of input, and stop without an error when a pass made no progress (waiting for the game's next\n"
         "      // buffer is not an error).\n"
         "      if ((!data.IsAnyInputBufferValid() || data.error_status == 4) &&\n"
         "          current_frame_remaining_subframes_ == 0) {\n"
         "        break;\n"
         "      }\n"
         "      if (data.input_buffer_read_offset == old_offset && data.current_buffer == old_buffer &&\n"
         "          data.IsAnyInputBufferValid() == old_valid &&\n"
         "          current_frame_remaining_subframes_ == old_remaining &&\n"
         "          remaining_subframe_blocks_in_output_buffer_ == old_capacity) {\n"
         "        break;\n"
         "      }\n"
         "      continue;\n"
         "    }\n"
         "    // Invalid packets must not leave the decoder worker spinning forever.\n")
    edit(xc,
         "  const uint8_t next_buffer_index = data->current_buffer ^ 1;\n"
         "  if (!data->IsInputBufferValid(next_buffer_index)) {\n"
         "    return nullptr;\n  }\n",
         "  const uint8_t next_buffer_index = data->current_buffer ^ 1;\n"
         "  if (!data->IsInputBufferValid(next_buffer_index)) {\n"
         "    return nullptr;\n  }\n"
         "  const bool cd_canary = REXCVAR_GET(cd_xma_canary);  // [cd] the next buffer's packet (index - count)\n"
         "  if (cd_canary) {\n"
         "    next_packet_index -= current_input_packet_count;\n"
         "    if (next_packet_index >= data->GetInputBufferPacketCount(next_buffer_index)) {\n"
         "      return nullptr;\n"
         "    }\n"
         "  }\n")
    edit(xc,
         "  return memory()->TranslatePhysical(next_buffer_address);\n",
         "  return memory()->TranslatePhysical(next_buffer_address) +\n"
         "         (cd_canary ? next_packet_index * kBytesPerPacket : 0);  // [cd]\n")
    edit(xc,
         "      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;\n"
         "    }\n"
         "    next_packet_index++;\n",
         "      return (next_packet_index * kBitsPerPacket) + packet_frame_offset;\n"
         "    }\n"
         "    if (REXCVAR_GET(cd_xma_canary)) {  // [cd] a continuation-only packet: this stream's next, by its skip count\n"
         "      const uint8_t next_skip = xma::GetPacketSkipCount(next_packet);\n"
         "      if (next_skip == 0xFF) {\n"
         "        break;\n"
         "      }\n"
         "      next_packet_index += next_skip + 1;\n"
         "      continue;\n"
         "    }\n"
         "    next_packet_index++;\n")
    edit(xc,
         "  if (is_loop_end_frame &&\n"
         "      (data->loop_subframe_end > 3 || data->loop_subframe_skip > 4 ||\n",
         "  if (!REXCVAR_GET(cd_xma_canary) && is_loop_end_frame &&  // [cd] as Xenia: no such check\n"
         "      (data->loop_subframe_end > 3 || data->loop_subframe_skip > 4 ||\n")
    edit(xc,
         "      current_frame_remaining_subframes_ -=\n"
         "          std::min<uint8_t>(skip, current_frame_remaining_subframes_);\n",
         "      if (REXCVAR_GET(cd_xma_canary)) {  // [cd] as Xenia: only a skip shorter than the frame\n"
         "        if (skip < current_frame_remaining_subframes_) {\n"
         "          current_frame_remaining_subframes_ -= skip;\n"
         "        }\n"
         "      } else {\n"
         "        current_frame_remaining_subframes_ -=\n"
         "            std::min<uint8_t>(skip, current_frame_remaining_subframes_);\n"
         "      }\n")

    # [cd] cd_xma_threads (default 1 = as before): the XMA decoder's kicked contexts decoded on N threads at once.
    # Crackdown 2 (crackdown2_013): the "XMA Decoder" thread ~94% busy, the game's sound engine (Wwise) waiting on its
    # kicks (each kick blocks its caller until that context is decoded: WaitForWorkDone, as Xenia Canary), and the
    # main thread then waiting 60-277 ms in Wwise's lock-free queue (sub_82959FC8 <- sub_82931D60) for the audio side
    # to hand buffers back. Each context has its own FFmpeg codec, frame and packet (xma_context.cpp), so different
    # contexts decode independently; one pass still finishes every context it took before waiting for the next kick,
    # so the game sees the same data in the same order, only sooner. Canary decodes them one by one on one thread.
    xd = sdk / "src" / "audio" / "xma_decoder.cpp"
    edit(xd, "#include <bit>\n",
         "#include <bit>\n#include <atomic>\n#include <condition_variable>\n#include <mutex>\n#include <thread>\n"
         "#include <vector>  // [cd] (cd_xma_threads)\n")
    edit(xd, "REXCVAR_DEFINE_BOOL(ffmpeg_verbose, false, \"Audio\", \"Verbose FFmpeg output (debug and above)\");\n",
         "REXCVAR_DEFINE_BOOL(ffmpeg_verbose, false, \"Audio\", \"Verbose FFmpeg output (debug and above)\");\n"
         "REXCVAR_DEFINE_INT32(cd_xma_threads, 1, \"Audio\",  // [cd]\n"
         "                     \"Threads decoding the kicked XMA contexts at once (1: one by one, as Xenia)\")\n"
         "    .range(1, 4);\n")
    edit(xd, "void XmaDecoder::WorkerThreadMain() {\n",
         "namespace {\n"
         "// [cd] cd_xma_threads: a pass's contexts shared between the decoder thread and N-1 helpers (made once, kept).\n"
         "class CdXmaPool {\n"
         " public:\n"
         "  void Run(XmaContext* contexts, const std::vector<uint32_t>& ids, int threads) {\n"
         "    {\n"
         "      std::lock_guard<std::mutex> lock(mutex_);\n"
         "      while (int(helpers_.size()) < threads - 1) helpers_.emplace_back([this] { Helper(); });\n"
         "      contexts_.store(contexts, std::memory_order_relaxed);\n"
         "      ids_ = ids;\n"
         "      count_.store(uint32_t(ids_.size()), std::memory_order_relaxed);\n"
         "      next_.store(0, std::memory_order_relaxed);\n"
         "      left_.store(uint32_t(ids_.size()), std::memory_order_release);\n"
         "      ++generation_;\n"
         "    }\n"
         "    wake_.notify_all();\n"
         "    Drain();\n"
         "    std::unique_lock<std::mutex> lock(mutex_);\n"
         "    done_.wait(lock, [this] { return left_.load(std::memory_order_acquire) == 0; });\n"
         "  }\n"
         "\n"
         " private:\n"
         "  void Drain() {\n"
         "    for (;;) {\n"
         "      const uint32_t i = next_.fetch_add(1, std::memory_order_acq_rel);\n"
         "      if (i >= count_.load(std::memory_order_acquire)) return;\n"
         "      XmaContext& context = contexts_.load(std::memory_order_acquire)[ids_[i]];\n"
         "      if (context.Work()) context.SignalWorkDone();\n"
         "      if (left_.fetch_sub(1, std::memory_order_acq_rel) == 1) {\n"
         "        std::lock_guard<std::mutex> lock(mutex_);\n"
         "        done_.notify_all();\n"
         "      }\n"
         "    }\n"
         "  }\n"
         "  void Helper() {\n"
         "    uint64_t seen = 0;\n"
         "    std::unique_lock<std::mutex> lock(mutex_);\n"
         "    for (;;) {\n"
         "      wake_.wait(lock, [&] { return generation_ != seen; });\n"
         "      seen = generation_;\n"
         "      lock.unlock();\n"
         "      Drain();\n"
         "      lock.lock();\n"
         "    }\n"
         "  }\n"
         "  std::mutex mutex_;\n"
         "  std::condition_variable wake_, done_;\n"
         "  std::vector<std::thread> helpers_;\n"
         "  std::vector<uint32_t> ids_;\n"
         "  std::atomic<XmaContext*> contexts_{nullptr};\n"
         "  std::atomic<uint32_t> count_{0}, next_{0}, left_{0};\n"
         "  uint64_t generation_ = 0;\n"
         "};\n"
         "CdXmaPool& CdXmaPoolInstance() {\n"
         "  static CdXmaPool* pool = new CdXmaPool;  // (never destroyed: its helpers live as long as the process)\n"
         "  return *pool;\n"
         "}\n"
         "}  // namespace\n"
         "\n"
         "void XmaDecoder::WorkerThreadMain() {\n")
    edit(xd,
         "  while (worker_running_) {\n"
         "    // Process only contexts made ready by a kick instead of scanning all 320\n",
         "  while (worker_running_) {\n"
         "    if (const int cd_threads = REXCVAR_GET(cd_xma_threads); cd_threads > 1) {  // [cd]\n"
         "      std::vector<uint32_t> cd_ids;\n"
         "      for (uint32_t w = 0; w < kKickRegisterCount; ++w) {\n"
         "        uint32_t bits = ready_context_words_[w].exchange(0, std::memory_order_acq_rel);\n"
         "        for (; bits; bits &= bits - 1)\n"
         "          cd_ids.push_back(w * kContextsPerKickRegister + uint32_t(std::countr_zero(bits)));\n"
         "      }\n"
         "      if (!cd_ids.empty()) CdXmaPoolInstance().Run(contexts_, cd_ids, cd_threads);\n"
         "      if (paused_) {\n"
         "        pause_fence_.Signal();\n"
         "        resume_fence_.Wait();\n"
         "      }\n"
         "      rex::thread::Wait(work_event_.get(), false);\n"
         "      continue;\n"
         "    }\n"
         "    // Process only contexts made ready by a kick instead of scanning all 320\n")
    print("Crackdown: cd_xma_threads (parallel XMA contexts, default 1)")


if __name__ == "__main__":
    main()
