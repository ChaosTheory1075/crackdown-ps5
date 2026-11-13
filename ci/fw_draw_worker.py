# [cd] Draws on two cores (cvar cd_draw_worker): Saints Row's ci/draw_worker.py (sr-ps5), ported.
# draw_worker(sdk, edit): the same edits for the replay copy and for ci/patch_framework.py.
#
# The GPU command thread (A) keeps everything that touches guest memory and the shared caches, in
# order: PM4, register writes, shader analysis and translations, samplers, texture requests, render
# targets, pipeline, vertex buffers, memexport, barriers and the render pass. Then it writes a splice
# into its deferred command buffer and hands the draw to the worker (B): pipeline bind and layout,
# viewport and dynamic state, system constants, constant uploads and descriptor sets, the draw
# command, recorded into the worker's own deferred command buffer, one segment a draw. At replay each
# splice plays the worker's next segment. B reads registers from a mirror fed, in order, by every
# register write A makes (one ring: register writes and draws). Texture views, samplers and the
# texture cache's per-fetch answers travel in the draw's packet.
import pathlib

GUARD = "// [cd] draw worker"


def draw_worker(sdk, edit):
    sdk = pathlib.Path(sdk)
    dh = sdk / "include" / "rex" / "graphics" / "vulkan" / "deferred_command_buffer.h"
    dc = sdk / "src" / "graphics" / "vulkan" / "deferred_command_buffer.cpp"
    bh = sdk / "include" / "rex" / "graphics" / "command_processor.h"
    bc = sdk / "src" / "graphics" / "command_processor.cpp"
    vh = sdk / "include" / "rex" / "graphics" / "vulkan" / "command_processor.h"
    vc = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"

    # ---- deferred command buffer: splices
    edit(dh, "    kVkSetViewport,\n  };\n",
         "    kVkSetViewport,\n"
         "    kSrSplice,      // [cd] draw worker: its next segment plays here\n"
         "    kSrSegmentEnd,  // [cd] draw worker: the end of one of its segments\n"
         "  };\n")
    edit(dh, "  void ExecuteStream(VkCommandBuffer command_buffer, const std::vector<uintmax_t>& commands);\n",
         "  void ExecuteStream(VkCommandBuffer command_buffer, const std::vector<uintmax_t>& commands);\n"
         "  // [cd] draw worker: `splice` is the worker's stream, whose segments play at the splices, in order\n"
         "  void ExecuteStream(VkCommandBuffer command_buffer, const std::vector<uintmax_t>& commands,\n"
         "                     const std::vector<uintmax_t>* splice);\n"
         "  void CmdSrSplice() { WriteCommand(Command::kSrSplice, 0); }\n"
         "  void CmdSrSegmentEnd() { WriteCommand(Command::kSrSegmentEnd, 0); }\n"
         "  std::vector<uintmax_t>& sr_stream() { return command_stream_; }\n")
    edit(dc, "void DeferredCommandBuffer::ExecuteStream(VkCommandBuffer command_buffer,\n"
             "                                          const std::vector<uintmax_t>& commands) {\n",
         "void DeferredCommandBuffer::ExecuteStream(VkCommandBuffer command_buffer,\n"
         "                                          const std::vector<uintmax_t>& commands) {\n"
         "  ExecuteStream(command_buffer, commands, nullptr);\n"
         "}\n\n"
         "void DeferredCommandBuffer::ExecuteStream(VkCommandBuffer command_buffer,\n"
         "                                          const std::vector<uintmax_t>& commands,\n"
         "                                          const std::vector<uintmax_t>* splice) {  " + GUARD + "\n")
    edit(dc, "  size_t stream_remaining = commands.size();\n  while (stream_remaining) {\n",
         "  size_t stream_remaining = commands.size();\n"
         "  const uintmax_t* splice_stream = splice ? splice->data() : nullptr;  " + GUARD + "\n"
         "  size_t splice_remaining = splice ? splice->size() : 0;\n"
         "  const uintmax_t* main_stream = nullptr;\n"
         "  size_t main_remaining = 0;\n"
         "  while (stream_remaining) {\n")
    edit(dc, "    stream_remaining -= kCommandHeaderSizeElements;\n\n    const uint32_t cd_kind = uint32_t(header.command) & 31;  // [cd]\n",
         "    stream_remaining -= kCommandHeaderSizeElements;\n\n"
         "    if (header.command == Command::kSrSplice) {  " + GUARD + ": the worker's next segment\n"
         "      if (!splice_remaining) {\n"
         "        continue;  // (no segment: nothing recorded for it)\n"
         "      }\n"
         "      main_stream = stream;\n"
         "      main_remaining = stream_remaining;\n"
         "      stream = splice_stream;\n"
         "      stream_remaining = splice_remaining;\n"
         "      continue;\n"
         "    }\n"
         "    if (header.command == Command::kSrSegmentEnd) {  // [cd] (back to the GPU thread's commands)\n"
         "      splice_stream = stream;\n"
         "      splice_remaining = stream_remaining;\n"
         "      stream = main_stream;\n"
         "      stream_remaining = main_remaining;\n"
         "      continue;\n"
         "    }\n\n"
         "    const uint32_t cd_kind = uint32_t(header.command) & 31;  // [cd]\n")

    # ---- base command processor: every register write reported (the worker's mirror)
    edit(bh, "  virtual void WriteRegister(uint32_t index, uint32_t value);\n",
         "  virtual void WriteRegister(uint32_t index, uint32_t value);\n"
         "  // [cd] draw worker: registers [first, first + count) written (the worker's mirror)\n"
         "  virtual void SrRegsWritten(uint32_t /*first*/, uint32_t /*count*/) {}\n")
    edit(bc, "  const_cast<volatile uint32_t&>(regs.values[index]) = value;\n",
         "  const_cast<volatile uint32_t&>(regs.values[index]) = value;\n"
         "  SrRegsWritten(index, 1);  " + GUARD + "\n")
    edit(bc, "        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);\n",
         "        const_cast<volatile uint32_t&>(regs.values[index]) |= UINT32_C(0x80000000);\n"
         "        SrRegsWritten(index, 1);  " + GUARD + "\n")
    edit(bc, "    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n    return;\n",
         "    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n"
         "    SrRegsWritten(start_index, num_registers);  " + GUARD + "\n"
         "    return;\n")
    edit(bc, "    std::memcpy(register_file_->values + first_register, register_values,\n"
             "                sizeof(uint32_t) * register_count);\n",
         "    std::memcpy(register_file_->values + first_register, register_values,\n"
         "                sizeof(uint32_t) * register_count);\n"
         "    SrRegsWritten(first_register, register_count);  " + GUARD + "\n")

    # the PS5 thread list: the worker keeps its CPU (the 10 s re-pinning leaves it alone)
    diag = sdk / "src" / "core" / "ps5_diag.cpp"
    if diag.exists():
        edit(diag, '!std::strcmp(e.what, "gpu submit"))',
             '!std::strcmp(e.what, "gpu submit") || !std::strcmp(e.what, "gpu draw worker"))')
    # ---- Vulkan command processor: header
    edit(vh, "#include <rex/graphics/xenos.h>\n#include <rex/hash.h>\n",
         "#include <rex/graphics/xenos.h>\n#include <rex/hash.h>\n#include <rex/cd_perf.h>  " + GUARD + "\n#include <atomic>\n")
    edit(vh, "  DeferredCommandBuffer deferred_command_buffer_;\n",
         "  DeferredCommandBuffer deferred_command_buffer_;\n"
         "\n"
         "  " + GUARD + " (cd_draw_worker; Extras/CD2-DRAW-WORKER.md): a draw's own state and draw\n"
         "  // command recorded on a second core, from a packet the GPU thread fills.\n"
         "  struct SrTexInfo {\n"
         "    uint32_t host_swizzle;\n"
         "    uint8_t signs;\n"
         "    bool scaled;\n"
         "  };\n"
         "  static constexpr uint32_t kSrMaxBindings = 64;\n"
         "  struct SrDrawPacket {\n"
         "    VulkanShader* vertex_shader;\n"
         "    VulkanShader* pixel_shader;\n"
         "    PrimitiveProcessor::ProcessingResult primitive_processing_result;\n"
         "    reg::RB_DEPTHCONTROL normalized_depth_control;\n"
         "    uint32_t normalized_color_mask;\n"
         "    uint32_t used_texture_mask;\n"
         "    uint32_t invalid;\n"
         "    bool primitive_polygonal;\n"
         "    bool shader_32bit_index_dma;\n"
         "    bool indexed;\n"
         "    VkIndexType index_type;\n"
         "    VkBuffer index_buffer;\n"
         "    VkDeviceSize index_offset;\n"
         "    VkPipeline pipeline;\n"
         "    const PipelineLayout* pipeline_layout;\n"
         "    uint32_t view_count[2];\n"
         "    uint32_t sampler_count[2];\n"
         "    VkImageView views[2][kSrMaxBindings];\n"
         "    std::pair<VulkanTextureCache::SamplerParameters, VkSampler> samplers[2][kSrMaxBindings];\n"
         "    SrTexInfo tex[32];\n"
         "    int32_t window_offset_tiles;\n"
         "  };\n"
         "  enum : uint32_t {\n"
         "    kSrInvPipeline = 1u << 0,\n"
         "    kSrInvViewport = 1u << 1,\n"
         "    kSrInvScissor = 1u << 2,\n"
         "    kSrInvDepthBias = 1u << 3,\n"
         "    kSrInvBlend = 1u << 4,\n"
         "    kSrInvStencil = 1u << 5,\n"
         "    kSrInvAll = (1u << 6) - 1,\n"
         "  };\n"
         "  using SrSamplerPair = std::pair<VulkanTextureCache::SamplerParameters, VkSampler>;\n"
         "  struct SrSamplerRange {\n"
         "    const SrSamplerPair* b;\n"
         "    const SrSamplerPair* e;\n"
         "    const SrSamplerPair* begin() const { return b; }\n"
         "    const SrSamplerPair* end() const { return e; }\n"
         "  };\n"
         "  void SrRegsWritten(uint32_t first, uint32_t count) override;\n"
         "  void SrWorkerStart();\n"
         "  void SrWorkerDrain();\n"
         "  void SrWorkerLoop();\n"
         "  void SrApplyRegs(uint32_t first, uint32_t count, const uint32_t* values);\n"
         "  void SrDrawTail(const SrDrawPacket& packet);\n"
         "  bool SrIssueDrawSplit(VulkanShader* vertex_shader, VulkanShader* pixel_shader,\n"
         "                        const PrimitiveProcessor::ProcessingResult& primitive_processing_result,\n"
         "                        reg::RB_DEPTHCONTROL normalized_depth_control,\n"
         "                        uint32_t normalized_color_mask, uint32_t used_texture_mask,\n"
         "                        bool primitive_polygonal, bool memexport_writes_possible,\n"
         "                        VkPipeline pipeline, const PipelineLayout* pipeline_layout,\n"
         "                        int32_t window_offset_tiles);\n"
         "  const RegisterFile& SrRegs() const;\n"
         "  DeferredCommandBuffer& SrDcb();\n"
         "  SrSamplerRange SrSamplers(uint32_t stage) const;\n"
         "  VkImageView SrBindingView(uint32_t stage, size_t index,\n"
         "                            const VulkanShader::TextureBinding& binding);\n"
         "  uint32_t SrTexHostSwizzle(uint32_t fetch) const;\n"
         "  uint8_t SrTexSigns(uint32_t fetch) const;\n"
         "  bool SrTexScaled(uint32_t fetch) const;\n"
         "  DeferredCommandBuffer sr_draw_dcb_{*this};\n"
         "  bool sr_worker_on_ = false;\n"
         "  bool sr_worker_checked_ = false;\n"
         "  uint32_t sr_pending_invalid_ = 0;\n"
         "  uint32_t sr_unwoken_draws_ = 0;\n"
         "  const SrDrawPacket* sr_packet_ = nullptr;  // (the worker's current draw)\n"
         "  std::unique_ptr<RegisterFile> sr_mirror_;\n"
         "  void* sr_ring_ = nullptr;\n"
         "  uint64_t sr_mirror_mismatches_ = 0, sr_mirror_checks_ = 0;\n"
         "  std::atomic<uint64_t> sr_busy_ticks_{0}, sr_draws_done_{0};  // (the worker's)\n"
         "  uint64_t sr_drain_wait_us_ = 0, sr_drains_ = 0, sr_report_us_ = 0;\n")

    # ---- Vulkan command processor: the worker
    edit(vc, "REXCVAR_DEFINE_BOOL(cd_submit_thread,",
         "REXCVAR_DEFINE_BOOL(cd_draw_worker, false, \"GPU/Vulkan\",\n"
         "                    \"[cd] Draws on two cores: each draw's own state, constants, descriptor sets \"\n"
         "                    \"and draw command recorded on a second core\");\n"
         "REXCVAR_DEFINE_INT32(cd_draw_worker_cpu, 5, \"GPU/Vulkan\",\n"
         "                     \"[cd] The draw worker's CPU (PS5; the submit thread then on CPU 1)\");\n"
         "static thread_local bool tl_sr_on_worker = false;  // [cd] draw worker (its thread)\n"
         "REXCVAR_DEFINE_BOOL(cd_submit_thread,")
    edit(vc, "#include <rex/graphics/gpu_lane.h>\n",
         "#include <rex/graphics/gpu_lane.h>\n#include <cstdlib>  " + GUARD + "\n#include <pthread.h>\n")
    edit(vc, "    std::vector<uintmax_t> stream;\n    VkCommandBuffer command_buffer = VK_NULL_HANDLE;\n",
         "    std::vector<uintmax_t> stream;\n"
         "    std::vector<uintmax_t> splice;  " + GUARD + " (its segments)\n"
         "    VkCommandBuffer command_buffer = VK_NULL_HANDLE;\n")
    edit(vc, "      command_processor_->deferred_command_buffer().ExecuteStream(job.command_buffer, job.stream);\n",
         "      command_processor_->deferred_command_buffer().ExecuteStream(\n"
         "          job.command_buffer, job.stream, job.splice.empty() ? nullptr : &job.splice);\n")
    ring = r'''
namespace {
// [cd] draw worker: register writes and draws, in order, from the GPU thread (the one writer) to
// the worker (the one reader). Records are 16-byte aligned; positions only grow.
class SrDrawRing {
 public:
  static constexpr uint64_t kSize = uint64_t(8) << 20;
  enum Type : uint32_t { kRegs = 1, kDraw = 2, kWrap = 3 };
  struct Header {
    uint32_t type;
    uint32_t size;  // (bytes, the header included)
    uint64_t pad;
  };
  SrDrawRing()  // (plain new[]: no new import on the PS5)
      : mem_(new uint8_t[kSize + 64]),
        buf_(mem_ + ((64 - (reinterpret_cast<uintptr_t>(mem_) & 63)) & 63)) {
    std::memset(buf_, 0, kSize);  // (every page there before the first draw: no page faults in play)
  }

  // (writer) Room for a record; the records before it stay unpublished until Publish.
  void* Reserve(uint32_t type, size_t payload) {
    const uint64_t size = (sizeof(Header) + payload + 15) & ~uint64_t(15);
    uint64_t at = write_ % kSize;
    if (at + size > kSize) {
      WaitRoom(kSize - at);
      auto* wrap = reinterpret_cast<Header*>(buf_ + at);
      wrap->type = kWrap;
      wrap->size = uint32_t(kSize - at);
      write_ += kSize - at;
      at = 0;
    }
    WaitRoom(size);
    auto* h = reinterpret_cast<Header*>(buf_ + at);
    h->type = type;
    h->size = uint32_t(size);
    write_ += size;
    return h + 1;
  }
  // (a sleeping reader is woken only with `wake`: a wake is a system call, so the writer wakes it
  // once some draws are waiting, or when it needs it done)
  void Publish(bool wake = true) {
    if (head_.load(std::memory_order_relaxed) != write_) {
      head_.store(write_);
    }
    if (wake && sleeping_.load()) {
      std::lock_guard<std::mutex> lock(mutex_);
      cv_.notify_one();
    }
  }
  // (writer) Everything handed over done.
  void Drain() {
    Publish();
    for (uint32_t spin = 0; tail_.load(std::memory_order_acquire) != write_; ++spin) {
      if (spin < 20000) {
        _mm_pause();
      } else {
        std::this_thread::yield();
      }
    }
  }
  bool Idle() const { return tail_.load(std::memory_order_acquire) == write_; }

  // (reader) The next record, waiting for one.
  Header* Next() {
    for (;;) {
      if (read_ == head_cached_) {
        for (uint32_t spin = 0;; ++spin) {
          head_cached_ = head_.load(std::memory_order_acquire);
          if (head_cached_ != read_) {
            break;
          }
          if (spin < 8000) {
            _mm_pause();
            continue;
          }
          std::unique_lock<std::mutex> lock(mutex_);
          sleeping_.store(true);
          cv_.wait(lock, [&] { return head_.load() != read_; });
          sleeping_.store(false);
        }
      }
      auto* h = reinterpret_cast<Header*>(buf_ + read_ % kSize);
      if (h->type == kWrap) {
        read_ += h->size;
        tail_.store(read_, std::memory_order_release);
        continue;
      }
      return h;
    }
  }
  void Done(const Header* h) {
    read_ += h->size;
    tail_.store(read_, std::memory_order_release);
  }

 private:
  void WaitRoom(uint64_t size) {
    if (write_ + size - tail_cached_ <= kSize) {
      return;  // (room, by the last position seen: no read of the reader's line)
    }
    for (uint32_t spin = 0; write_ + size - (tail_cached_ = tail_.load(std::memory_order_acquire)) > kSize;
         ++spin) {
      Publish();
      if (spin < 20000) {
        _mm_pause();
      } else {
        std::this_thread::yield();
      }
    }
  }
  uint8_t* mem_;
  uint8_t* buf_;
  uint64_t write_ = 0;  // (writer's own)
  uint64_t tail_cached_ = 0;  // (writer's: the reader's position, last seen)
  uint64_t read_ = 0;   // (reader's own)
  uint64_t head_cached_ = 0;  // (reader's: the writer's position, last seen)
  alignas(64) std::atomic<uint64_t> head_{0};
  alignas(64) std::atomic<uint64_t> tail_{0};
  alignas(64) std::atomic<bool> sleeping_{false};
  std::mutex mutex_;
  std::condition_variable cv_;
};
SrDrawRing& SrRing(void* ring) { return *static_cast<SrDrawRing*>(ring); }
bool SrMapRangeUsed(const uint64_t* usage_map, uint32_t first, uint32_t last) {
  for (uint32_t c = first; c <= last; ++c) {
    if (usage_map[c >> 6] & (uint64_t(1) << (c & 63))) {
      return true;
    }
  }
  return false;
}
}  // namespace

const RegisterFile& VulkanCommandProcessor::SrRegs() const {
  return tl_sr_on_worker ? *sr_mirror_ : *register_file_;
}
DeferredCommandBuffer& VulkanCommandProcessor::SrDcb() {
  return tl_sr_on_worker ? sr_draw_dcb_ : deferred_command_buffer_;
}
VulkanCommandProcessor::SrSamplerRange VulkanCommandProcessor::SrSamplers(uint32_t stage) const {
  if (tl_sr_on_worker) {
    return {sr_packet_->samplers[stage], sr_packet_->samplers[stage] + sr_packet_->sampler_count[stage]};
  }
  const auto& v = stage ? current_samplers_pixel_ : current_samplers_vertex_;
  return {v.data(), v.data() + v.size()};
}
VkImageView VulkanCommandProcessor::SrBindingView(uint32_t stage, size_t index,
                                                  const VulkanShader::TextureBinding& binding) {
  if (tl_sr_on_worker) {
    return index < sr_packet_->view_count[stage] ? sr_packet_->views[stage][index] : VK_NULL_HANDLE;
  }
  return texture_cache_->GetActiveBindingOrNullImageView(binding.fetch_constant, binding.dimension,
                                                         bool(binding.is_signed));
}
uint32_t VulkanCommandProcessor::SrTexHostSwizzle(uint32_t fetch) const {
  return tl_sr_on_worker ? sr_packet_->tex[fetch].host_swizzle
                         : texture_cache_->GetActiveTextureHostSwizzle(fetch);
}
uint8_t VulkanCommandProcessor::SrTexSigns(uint32_t fetch) const {
  return tl_sr_on_worker ? sr_packet_->tex[fetch].signs
                         : texture_cache_->GetActiveTextureSwizzledSigns(fetch);
}
bool VulkanCommandProcessor::SrTexScaled(uint32_t fetch) const {
  return tl_sr_on_worker ? sr_packet_->tex[fetch].scaled
                         : texture_cache_->IsActiveTextureResolutionScaled(fetch);
}

void VulkanCommandProcessor::SrRegsWritten(uint32_t first, uint32_t count) {
  if (!sr_worker_on_ || !count || first >= RegisterFile::kRegisterCount) {
    return;
  }
  count = std::min(count, uint32_t(RegisterFile::kRegisterCount) - first);
  auto* p = static_cast<uint32_t*>(SrRing(sr_ring_).Reserve(SrDrawRing::kRegs, 8 + 4 * size_t(count)));
  p[0] = first;
  p[1] = count;
  std::memcpy(p + 2, register_file_->values + first, 4 * size_t(count));
}

void VulkanCommandProcessor::SrApplyRegs(uint32_t first, uint32_t count, const uint32_t* values) {
  uint32_t* dst = sr_mirror_->values + first;
  uint32_t lo = 0;
  while (lo < count && dst[lo] == values[lo]) {
    ++lo;
  }
  if (lo == count) {
    return;
  }
  uint32_t hi = count - 1;
  while (dst[hi] == values[hi]) {
    --hi;
  }
  std::memcpy(dst + lo, values + lo, 4 * size_t(hi - lo + 1));
  const uint32_t a = first + lo, b = first + hi;
  // The constant buffers the changed registers are in (the GPU thread's WriteRegister and
  // WriteRegistersFromMem do this without the worker).
  if (a <= XE_GPU_REG_SHADER_CONSTANT_511_W && b >= XE_GPU_REG_SHADER_CONSTANT_000_X) {
    const uint32_t fa = (std::max(a, uint32_t(XE_GPU_REG_SHADER_CONSTANT_000_X)) -
                         XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    const uint32_t fb = (std::min(b, uint32_t(XE_GPU_REG_SHADER_CONSTANT_511_W)) -
                         XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
    if (fa < 256 && SrMapRangeUsed(current_float_constant_map_vertex_, fa, std::min(fb, 255u))) {
      current_constant_buffers_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatVertex);
    }
    if (fb >= 256 &&
        SrMapRangeUsed(current_float_constant_map_pixel_, std::max(fa, 256u) - 256, fb - 256)) {
      current_constant_buffers_up_to_date_ &=
          ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFloatPixel);
    }
  }
  if (a <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31 && b >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031) {
    current_constant_buffers_up_to_date_ &= ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);
  }
  if (a <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 && b >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) {
    current_constant_buffers_up_to_date_ &= ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);
  }
}

void VulkanCommandProcessor::SrWorkerStart() {
  sr_mirror_ = std::make_unique<RegisterFile>(*register_file_);
  sr_ring_ = new SrDrawRing();
  sr_pending_invalid_ = kSrInvAll;
  sr_worker_on_ = true;
  const int32_t cpu = REXCVAR_GET(cd_draw_worker_cpu);
  std::thread([this, cpu] {
#if defined(__PROSPERO__)
    rex::frame_stats::scePthreadSetaffinity(pthread_self(), uint64_t(1) << cpu);
    rex::ps5diag::RegisterThread("gpu draw worker");
#else
    (void)cpu;
    pthread_setname_np(pthread_self(), "cd draw worker");
#endif
    SrWorkerLoop();
  }).detach();
  REXGPU_INFO("cd draw worker: on (CPU {})", cpu);
}

void VulkanCommandProcessor::SrWorkerLoop() {
  tl_sr_on_worker = true;
  SrDrawRing& ring = SrRing(sr_ring_);
  for (;;) {
    SrDrawRing::Header* h = ring.Next();
    const uint64_t busy_start = rex::cd_perf::NowNs();
    if (h->type == SrDrawRing::kRegs) {
      const auto* p = reinterpret_cast<const uint32_t*>(h + 1);
      SrApplyRegs(p[0], p[1], p + 2);
    } else if (h->type == SrDrawRing::kDraw) {
      sr_packet_ = reinterpret_cast<const SrDrawPacket*>(h + 1);
      SrDrawTail(*sr_packet_);
      sr_draw_dcb_.CmdSrSegmentEnd();
      sr_packet_ = nullptr;
      sr_draws_done_.fetch_add(1, std::memory_order_relaxed);
    }
    sr_busy_ticks_.fetch_add(rex::cd_perf::NowNs() - busy_start, std::memory_order_relaxed);
    ring.Done(h);
  }
}

void VulkanCommandProcessor::SrWorkerDrain() {
  if (!sr_worker_on_) {
    return;
  }
  SrDrawRing& ring = SrRing(sr_ring_);
  const uint64_t now_us = rex::cd_perf::NowUs();
  if (!ring.Idle()) {
    ring.Drain();
    sr_drain_wait_us_ += rex::cd_perf::NowUs() - now_us;
    ++sr_drains_;
  }
  if (!sr_report_us_) {
    sr_report_us_ = now_us;
  } else if (now_us - sr_report_us_ >= 10000000) {
    const double seconds = double(now_us - sr_report_us_) / 1e6;
    REXGPU_INFO(
        "cd draw worker: {:.0f} draws a second; the worker busy {:.1f} ms a second; the GPU thread "
        "waited for it {:.1f} times a second, {:.2f} ms a second",
        double(sr_draws_done_.exchange(0)) / seconds,
        double(sr_busy_ticks_.exchange(0)) / 1e6 / seconds, double(sr_drains_) / seconds,
        double(sr_drain_wait_us_) / 1000.0 / seconds);
    sr_drains_ = sr_drain_wait_us_ = 0;
    sr_report_us_ = now_us;
  }
  // (writes the mirror missed, if any - registers changed without a report: taken over, and the
  // constant buffers made again)
  ++sr_mirror_checks_;
  // (COHER_STATUS_HOST: the game's CPU writes it, the worker never reads it)
  sr_mirror_->values[XE_GPU_REG_COHER_STATUS_HOST] = register_file_->values[XE_GPU_REG_COHER_STATUS_HOST];
  // (CP_RB_WPTR, 01C5: the ring's write pointer, which the game's CPU writes too; sr1_046 / sr2_013 had it as
  // the one register different at every check, and each time every constant buffer was made again for nothing)
  sr_mirror_->values[XE_GPU_REG_CP_RB_WPTR] = register_file_->values[XE_GPU_REG_CP_RB_WPTR];
  if (std::memcmp(sr_mirror_->values, register_file_->values, sizeof(register_file_->values))) {
    uint32_t first_mismatch = 0, mismatches = 0;
    for (uint32_t i = 0; i < RegisterFile::kRegisterCount; ++i) {
      // (the two registers the game's CPU writes while this runs don't count: a write between the copy
      // above and this compare still showed 01C5 in sr1_048, 13 times)
      if (i == XE_GPU_REG_CP_RB_WPTR || i == XE_GPU_REG_COHER_STATUS_HOST) continue;
      if (sr_mirror_->values[i] != register_file_->values[i]) {
        if (!mismatches++) {
          first_mismatch = i;
        }
      }
    }
    if (!mismatches) return;  // (only those two: nothing the worker reads was missed)
    if (sr_mirror_mismatches_++ < 20) {
      REXGPU_INFO("cd draw worker: mirror had {} register(s) different, the first {:04X}",
                  mismatches, first_mismatch);
    }
    std::memcpy(sr_mirror_->values, register_file_->values, sizeof(register_file_->values));
    current_constant_buffers_up_to_date_ = 0;
  }
}

bool VulkanCommandProcessor::SrIssueDrawSplit(
    VulkanShader* vertex_shader, VulkanShader* pixel_shader,
    const PrimitiveProcessor::ProcessingResult& primitive_processing_result,
    reg::RB_DEPTHCONTROL normalized_depth_control, uint32_t normalized_color_mask,
    uint32_t used_texture_mask, bool primitive_polygonal, bool memexport_writes_possible,
    VkPipeline pipeline, const PipelineLayout* pipeline_layout, int32_t window_offset_tiles) {
  const RegisterFile& regs = *register_file_;
  const ui::vulkan::VulkanDevice::Properties& device_properties = GetVulkanDevice()->properties();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  bool shader_32bit_index_dma =
      !device_properties.fullDrawIndexUint32 &&
      primitive_processing_result.index_buffer_type ==
          PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA &&
      vgt_draw_initiator.index_size == xenos::IndexFormat::kInt32 &&
      primitive_processing_result.host_vertex_shader_type == Shader::HostVertexShaderType::kVertex;
@@VERTEX_FETCH@@
@@MEMEXPORT_AND_RENDER_PASS@@
  ++frame_flow_draw_submitted_;
  SrDrawRing& ring = SrRing(sr_ring_);
  auto* packet = static_cast<SrDrawPacket*>(ring.Reserve(SrDrawRing::kDraw, sizeof(SrDrawPacket)));
  packet->vertex_shader = vertex_shader;
  packet->pixel_shader = pixel_shader;
  packet->primitive_processing_result = primitive_processing_result;
  packet->normalized_depth_control = normalized_depth_control;
  packet->normalized_color_mask = normalized_color_mask;
  packet->used_texture_mask = used_texture_mask;
  packet->invalid = sr_pending_invalid_;
  sr_pending_invalid_ = 0;
  packet->primitive_polygonal = primitive_polygonal;
  packet->shader_32bit_index_dma = shader_32bit_index_dma;
  packet->pipeline = pipeline;
  packet->pipeline_layout = pipeline_layout;
  packet->window_offset_tiles = window_offset_tiles;
  packet->indexed = false;
  if (primitive_processing_result.index_buffer_type !=
          PrimitiveProcessor::ProcessedIndexBufferType::kNone &&
      !shader_32bit_index_dma) {
    std::pair<VkBuffer, VkDeviceSize> index_buffer;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA:
        if (guest_dma_index_scratch_buffer.buffer() != VK_NULL_HANDLE) {
          index_buffer.first = guest_dma_index_scratch_buffer.buffer();
          index_buffer.second = 0;
        } else {
          index_buffer.first = shared_memory_->buffer();
          index_buffer.second = primitive_processing_result.guest_index_base;
        }
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer = primitive_processor_->GetConvertedIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer = primitive_processor_->GetBuiltinIndexBuffer(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        index_buffer = {VK_NULL_HANDLE, 0};
        break;
    }
    packet->indexed = index_buffer.first != VK_NULL_HANDLE;
    packet->index_buffer = index_buffer.first;
    packet->index_offset = index_buffer.second;
    packet->index_type = primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16
                             ? VK_INDEX_TYPE_UINT16
                             : VK_INDEX_TYPE_UINT32;
  }
  // The texture cache's answers for this draw, as UpdateBindings and the system constants would
  // have asked them now.
  for (uint32_t stage = 0; stage < 2; ++stage) {
    const VulkanShader* shader = stage ? pixel_shader : vertex_shader;
    packet->view_count[stage] = 0;
    packet->sampler_count[stage] = 0;
    if (!shader) {
      continue;
    }
    const auto& bindings = shader->GetTextureBindingsAfterTranslation();
    uint32_t n = uint32_t(std::min(bindings.size(), size_t(kSrMaxBindings)));
    for (uint32_t i = 0; i < n; ++i) {
      packet->views[stage][i] = texture_cache_->GetActiveBindingOrNullImageView(
          bindings[i].fetch_constant, bindings[i].dimension, bool(bindings[i].is_signed));
    }
    packet->view_count[stage] = n;
    const auto& samplers = stage ? current_samplers_pixel_ : current_samplers_vertex_;
    n = uint32_t(std::min(samplers.size(), size_t(kSrMaxBindings)));
    std::copy(samplers.begin(), samplers.begin() + n, packet->samplers[stage]);
    packet->sampler_count[stage] = n;
    if (bindings.size() > kSrMaxBindings || samplers.size() > kSrMaxBindings) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXGPU_ERROR("cd draw worker: a shader with {} texture and {} sampler bindings (room for {})",
                     bindings.size(), samplers.size(), kSrMaxBindings);
      }
    }
  }
  uint32_t textures_remaining = used_texture_mask, texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(UINT32_C(1) << texture_index);
    SrTexInfo& t = packet->tex[texture_index];
    t.host_swizzle = texture_cache_->GetActiveTextureHostSwizzle(texture_index);
    t.signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    t.scaled = texture_cache_->IsActiveTextureResolutionScaled(texture_index);
  }
  deferred_command_buffer_.CmdSrSplice();
  ring.Publish(++sr_unwoken_draws_ >= 16);
  if (sr_unwoken_draws_ >= 16) {
    sr_unwoken_draws_ = 0;
  }
@@MEMEXPORT_TAIL@@
  return true;
}

void VulkanCommandProcessor::SrDrawTail(const SrDrawPacket& packet) {
  const RegisterFile& regs = *sr_mirror_;
  const ui::vulkan::VulkanDevice::Properties& device_properties = GetVulkanDevice()->properties();
  const uint32_t invalid = packet.invalid;
  if (invalid & kSrInvPipeline) {
    current_guest_graphics_pipeline_ = VK_NULL_HANDLE;
    current_guest_graphics_pipeline_layout_ = nullptr;
  }
  if (invalid & kSrInvViewport) {
    dynamic_viewport_update_needed_ = true;
  }
  if (invalid & kSrInvScissor) {
    dynamic_scissor_update_needed_ = true;
  }
  if (invalid & kSrInvDepthBias) {
    dynamic_depth_bias_update_needed_ = true;
  }
  if (invalid & kSrInvBlend) {
    dynamic_blend_constants_update_needed_ = true;
  }
  if (invalid & kSrInvStencil) {
    dynamic_stencil_compare_mask_front_update_needed_ = true;
    dynamic_stencil_compare_mask_back_update_needed_ = true;
    dynamic_stencil_write_mask_front_update_needed_ = true;
    dynamic_stencil_write_mask_back_update_needed_ = true;
    dynamic_stencil_reference_front_update_needed_ = true;
    dynamic_stencil_reference_back_update_needed_ = true;
  }
  VulkanShader* const vertex_shader = packet.vertex_shader;
  VulkanShader* const pixel_shader = packet.pixel_shader;
  const reg::RB_DEPTHCONTROL normalized_depth_control = packet.normalized_depth_control;
  if (current_guest_graphics_pipeline_ != packet.pipeline) {
    sr_draw_dcb_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, packet.pipeline);
    current_guest_graphics_pipeline_ = packet.pipeline;
  }
  const VulkanPipelineCache::PipelineLayoutProvider* pipeline_layout_provider = packet.pipeline_layout;
@@LAYOUT@@
  bool host_render_targets_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();
  draw_util::ViewportInfo viewport_info;
  draw_util::GetHostViewportInfo(
      regs, draw_resolution_scale_x, draw_resolution_scale_y, false,
      device_properties.maxViewportDimensions[0], device_properties.maxViewportDimensions[1], true,
      normalized_depth_control,
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader(),
      host_render_targets_used, pixel_shader && pixel_shader->writes_depth(), viewport_info,
      packet.window_offset_tiles != 0);
  UpdateDynamicState(viewport_info, packet.primitive_polygonal, normalized_depth_control,
                     packet.window_offset_tiles != 0);
  UpdateSystemConstantValues(packet.primitive_polygonal, packet.primitive_processing_result,
                             packet.shader_32bit_index_dma, 0, viewport_info,
                             packet.used_texture_mask, normalized_depth_control,
                             packet.normalized_color_mask, packet.window_offset_tiles);
  if (!UpdateBindings(vertex_shader, pixel_shader)) {
    REXGPU_ERROR("cd draw worker: UpdateBindings failed, a draw dropped");
    return;
  }
  const uint32_t vertex_count = packet.primitive_processing_result.host_draw_vertex_count;
  if (!packet.indexed) {
    sr_draw_dcb_.CmdVkDraw(vertex_count, 1, 0, 0);
  } else {
    sr_draw_dcb_.CmdVkBindIndexBuffer(packet.index_buffer, packet.index_offset, packet.index_type);
    sr_draw_dcb_.CmdVkDrawIndexed(vertex_count, 1, 0, 0, 0);
  }
}
'''
    text = vc.read_text()
    if "void VulkanCommandProcessor::SrIssueDrawSplit(" not in text:
        # the pieces copied from IssueDraw as it is
        def between(start, end):
            i = text.index(start)
            j = text.index(end, i)
            return text[i:j]
        vfetch = between("  // Ensure vertex buffers are resident.\n",
                         "  // Synchronize the memory pages backing memory scatter export streams, and\n")
        mem = between("  // Synchronize the memory pages backing memory scatter export streams, and\n",
                      "  // Draw.\n  ++frame_flow_draw_submitted_;\n")
        tail_start = "  // Invalidate textures in memexported memory and watch for changes.\n"
        i = text.index(tail_start)
        j = text.index("  return true;\n}\n", i)
        memtail = text[i:j]
        layout = between("  // Update the graphics pipeline, and if the new graphics pipeline has a\n",
                         "  bool host_render_targets_used =\n")
        assert "return false;" in vfetch  # (its failures: the draw dropped, as IssueDraw does)
        code = (ring.replace("@@VERTEX_FETCH@@\n", vfetch)
                    .replace("@@MEMEXPORT_AND_RENDER_PASS@@\n", mem)
                    .replace("@@MEMEXPORT_TAIL@@\n", memtail)
                    .replace("@@LAYOUT@@\n", layout))
        code = code.replace("return draw_fail(\"guest_dma_index_scratch_buffer\");", "return false;")
        anchor = "bool VulkanCommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {\n"
        assert text.count(anchor) == 1
        text = text.replace(anchor, code.lstrip("\n") + "\n" + anchor)
        vc.write_text(text)
        print(f"patched  {vc} (draw worker)")

    # the submit thread makes room on CPU 5 for the worker: on CPU 1, the GPU thread's twin
    edit(vc, "    rex::frame_stats::scePthreadSetaffinity(pthread_self(), uint64_t(1) << 5);\n",
         "    rex::frame_stats::scePthreadSetaffinity(  " + GUARD + " (CPU 5 its: this one on CPU 1)\n"
         "        pthread_self(), uint64_t(1) << (REXCVAR_GET(cd_draw_worker) ? 1 : 5));\n")
    # IssueDraw: the worker starts at the first draw; the split at the pipeline's bind
    edit(vc, "  ++frame_flow_draw_calls_;\n  const RegisterFile& regs = *register_file_;\n",
         "  ++frame_flow_draw_calls_;\n"
         "  if (!sr_worker_checked_) {  " + GUARD + " (cd_draw_worker)\n"
         "    sr_worker_checked_ = true;\n"
         "    if (REXCVAR_GET(cd_draw_worker)) {\n"
         "      SrWorkerStart();\n"
         "    }\n"
         "  }\n"
         "  const RegisterFile& regs = *register_file_;\n")
    edit(vc, "  if (current_guest_graphics_pipeline_ != pipeline) {\n"
             "    deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);\n",
         "  if (sr_worker_on_) {  " + GUARD + ": the draw's own state and command on the worker\n"
         "    return SrIssueDrawSplit(vertex_shader, pixel_shader, primitive_processing_result,\n"
         "                            normalized_depth_control, normalized_color_mask, used_texture_mask,\n"
         "                            primitive_polygonal, memexport_writes_possible, pipeline,\n"
         "                            static_cast<const PipelineLayout*>(pipeline_layout_provider),\n"
         "                            window_offset_tiles);\n"
         "  }\n"
         "  if (current_guest_graphics_pipeline_ != pipeline) {\n"
         "    deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);\n")

    # submissions: the worker drained first; its stream goes with the GPU thread's
    edit(vc, "  rex::cd_perf::Scope cd_perf_submit(rex::cd_perf::kSubmit);\n",
         "  rex::cd_perf::Scope cd_perf_submit(rex::cd_perf::kSubmit);\n"
         "  SrWorkerDrain();  " + GUARD + "\n")
    edit(vc, "      deferred_command_buffer_.TakeStream(job.stream);\n",
         "      deferred_command_buffer_.TakeStream(job.stream);\n"
         "      if (sr_worker_on_) {  " + GUARD + "\n"
         "        sr_draw_dcb_.TakeStream(job.splice);\n"
         "      }\n")
    edit(vc, "    deferred_command_buffer_.Execute(command_buffer.buffer);\n",
         "    deferred_command_buffer_.ExecuteStream(  " + GUARD + "\n"
         "        command_buffer.buffer, deferred_command_buffer_.sr_stream(),\n"
         "        sr_worker_on_ ? &sr_draw_dcb_.sr_stream() : nullptr);\n")
    edit(vc, "  if (submission_open_ && !is_opening_frame) {\n    return true;\n  }\n",
         "  if (submission_open_ && !is_opening_frame) {\n    return true;\n  }\n"
         "  SrWorkerDrain();  " + GUARD + " (its state reset below)\n")
    edit(vc, "    deferred_command_buffer_.Reset();\n",
         "    deferred_command_buffer_.Reset();\n"
         "    sr_draw_dcb_.Reset();  " + GUARD + "\n")
    edit(vc, "  CdSubmitThread::Get().Drain();  // [cd]\n  AwaitAllQueueOperationsCompletion();\n",
         "  SrWorkerDrain();  " + GUARD + "\n"
         "  CdSubmitThread::Get().Drain();  // [cd]\n  AwaitAllQueueOperationsCompletion();\n")

    # register writes: the worker's constant-buffer state left to it
    edit(vc, "    if (frame_open_) {\n      uint32_t float_constant_index =",
         "    if (frame_open_ && !sr_worker_on_) {  " + GUARD + " (does this itself)\n"
         "      uint32_t float_constant_index =")
    edit(vc, "    if (frame_open_) {\n      uint32_t first_float_constant =",
         "    if (frame_open_ && !sr_worker_on_) {  " + GUARD + " (does this itself)\n"
         "      uint32_t first_float_constant =")
    edit(vc, "    current_constant_buffers_up_to_date_ &=\n"
             "        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);\n",
         "    if (!sr_worker_on_)  " + GUARD + "\n"
         "    current_constant_buffers_up_to_date_ &=\n"
         "        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);\n", count=2)
    edit(vc, "    current_constant_buffers_up_to_date_ &=\n"
             "        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);\n",
         "    if (!sr_worker_on_)  " + GUARD + "\n"
         "    current_constant_buffers_up_to_date_ &=\n"
         "        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);\n", count=2)
    edit(vc, "    return;\n  }\n\n  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&\n",
         "    SrRegsWritten(cd_first, cd_last - cd_first + 1);  " + GUARD + "\n"
         "    return;\n  }\n\n  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&\n")
    edit(vc, "        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);\n    return;\n  }\n",
         "        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferBoolLoop);\n"
         "    SrRegsWritten(start_index, num_registers);  " + GUARD + "\n"
         "    return;\n  }\n")
    edit(vc, "    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);\n    return;\n",
         "    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);\n"
         "    SrRegsWritten(cd_first, cd_last - cd_first + 1);  " + GUARD + "\n"
         "    return;\n")

    # state the GPU thread changes between the worker's draws: recorded, and the worker told
    edit(vc, "                                                          bool keep_dynamic_stencil_mask_ref) {\n",
         "                                                          bool keep_dynamic_stencil_mask_ref) {\n"
         "  if (sr_worker_on_) {  " + GUARD + ": its state set again at its next draw\n"
         "    sr_pending_invalid_ |= kSrInvPipeline | (keep_dynamic_depth_bias ? 0 : kSrInvDepthBias) |\n"
         "                           (keep_dynamic_blend_constants ? 0 : kSrInvBlend) |\n"
         "                           (keep_dynamic_stencil_mask_ref ? 0 : kSrInvStencil);\n"
         "    deferred_command_buffer_.CmdVkBindPipeline(VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);\n"
         "    return;\n"
         "  }\n")
    edit(vc, "void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {\n",
         "void VulkanCommandProcessor::SetViewport(const VkViewport& viewport) {\n"
         "  if (sr_worker_on_ && !tl_sr_on_worker) {  " + GUARD + ": set again at its next draw\n"
         "    deferred_command_buffer_.CmdVkSetViewport(0, 1, &viewport);\n"
         "    sr_pending_invalid_ |= kSrInvViewport;\n"
         "    return;\n"
         "  }\n")
    edit(vc, "    deferred_command_buffer_.CmdVkSetViewport(0, 1, &dynamic_viewport_);\n",
         "    SrDcb().CmdVkSetViewport(0, 1, &dynamic_viewport_);\n")
    edit(vc, "void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {\n",
         "void VulkanCommandProcessor::SetScissor(const VkRect2D& scissor) {\n"
         "  if (sr_worker_on_ && !tl_sr_on_worker) {  " + GUARD + ": set again at its next draw\n"
         "    deferred_command_buffer_.CmdVkSetScissor(0, 1, &scissor);\n"
         "    sr_pending_invalid_ |= kSrInvScissor;\n"
         "    return;\n"
         "  }\n")
    edit(vc, "    deferred_command_buffer_.CmdVkSetScissor(0, 1, &dynamic_scissor_);\n",
         "    SrDcb().CmdVkSetScissor(0, 1, &dynamic_scissor_);\n")
    edit(vc, "  GpuLane& lane = GpuLane::Get();\n  if (!lane.Active()) {\n",
         "  if (tl_sr_on_worker) {  " + GUARD + " (writes its sets itself)\n    return false;\n  }\n"
         "  GpuLane& lane = GpuLane::Get();\n  if (!lane.Active()) {\n")

    # the worker's functions: its registers, its command buffer, the packet's texture answers
    text = vc.read_text()
    if "const RegisterFile& regs = SrRegs();" not in text:
        for fn in ("void VulkanCommandProcessor::UpdateDynamicState(",
                   "void VulkanCommandProcessor::UpdateSystemConstantValues(",
                   "bool VulkanCommandProcessor::UpdateBindings("):
            i = text.index(fn)
            j = text.index("\n}\n", i)
            body = text[i:j]
            assert body.count("  const RegisterFile& regs = *register_file_;\n") == 1, fn
            body = body.replace("  const RegisterFile& regs = *register_file_;\n",
                                "  const RegisterFile& regs = SrRegs();  " + GUARD + "\n")
            body = body.replace("deferred_command_buffer_.", "SrDcb().")
            body = body.replace("texture_cache_->GetActiveTextureSwizzledSigns(", "SrTexSigns(")
            body = body.replace("texture_cache_->IsActiveTextureResolutionScaled(", "SrTexScaled(")
            body = body.replace("texture_cache_->GetActiveTextureHostSwizzle(", "SrTexHostSwizzle(")
            if fn.startswith("bool VulkanCommandProcessor::UpdateBindings("):
                old_v = ("    for (const VulkanShader::TextureBinding& texture_binding : textures_vertex) {\n"
                         "      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();\n"
                         "      descriptor_image_info.imageView = texture_cache_->GetActiveBindingOrNullImageView(\n"
                         "          texture_binding.fetch_constant, texture_binding.dimension,\n"
                         "          bool(texture_binding.is_signed));\n")
                old_p = old_v.replace(": textures_vertex)", ": *textures_pixel)")
                assert body.count(old_v) == 1 and body.count(old_p) == 1
                body = body.replace(old_v,
                    "    for (const VulkanShader::TextureBinding& texture_binding : textures_vertex) {\n"
                    "      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();\n"
                    "      descriptor_image_info.imageView =  " + GUARD + "\n"
                    "          SrBindingView(0, &texture_binding - textures_vertex.data(), texture_binding);\n")
                body = body.replace(old_p,
                    "    for (const VulkanShader::TextureBinding& texture_binding : *textures_pixel) {\n"
                    "      VkDescriptorImageInfo& descriptor_image_info = descriptor_write_image_info_.emplace_back();\n"
                    "      descriptor_image_info.imageView =  " + GUARD + "\n"
                    "          SrBindingView(1, &texture_binding - textures_pixel->data(), texture_binding);\n")
                assert body.count("         current_samplers_vertex_) {") == 1
                assert body.count("         current_samplers_pixel_) {") == 1
                body = body.replace("         current_samplers_vertex_) {", "         SrSamplers(0)) {")
                body = body.replace("         current_samplers_pixel_) {", "         SrSamplers(1)) {")
                assert "texture_cache_->GetActive" not in body
            text = text[:i] + body + text[j:]
        vc.write_text(text)
        print(f"patched  {vc} (draw worker: its functions)")


def _edit(path, old, new, count=1):
    import sys
    text = path.read_text(encoding="utf-8")
    if new in text:
        print(f"already  {path.name}")
        return
    if text.count(old) != count:
        sys.exit(f"fw_draw_worker: expected {count} of {old[:120]!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new), encoding="utf-8")
    print(f"patched  {path.name}")


if __name__ == "__main__":
    import sys
    draw_worker(pathlib.Path(sys.argv[1]) / "glue" / "rexglue-sdk-main", _edit)
