#!/usr/bin/env python3
"""Constant register writes that change no value invalidate nothing (MGR's per-draw skip, mgr-ps5 45528e6).

Xbox 360 games rewrite whole blocks of shader constants (float, bool / loop) and fetch constants before most draws,
mostly with the values already there. Each such write threw away the uploaded constant buffer (the next draw
allocated, copied and re-bound it), and for fetch constants re-derived every texture binding's key and the vertex
buffers' residency. A write is compared with the register file as it is copied in: no value differs, nothing is
invalidated; some do, only the changed span is. Every cache it skips would be rebuilt from the same register values:
pixel-identical. cd_skip_unchanged_writes 0 (default: no experiment in a user build, supervisor 2026-11-06) off,
1 on, 2 on and off in alternate 10 s windows (a measuring build's config only), so one run
measures it on the same scene ("cd unchanged constant writes" line).

    fw_draw_skip.py <framework>     (after the other fw_*.py)
"""
import pathlib
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        sys.exit(f"fw_draw_skip: expected {count} of {old!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    fw = pathlib.Path(sys.argv[1])
    sdk = fw / "glue" / "rexglue-sdk-main"
    vcp = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"
    P = "cd_skip::"
    edit(vcp,
         "REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, \"GPU/Vulkan\",\n",
         "namespace cd_skip {  // [cd] the counts (the GPU thread's only) and the 10 s report\n"
         "enum Counter { kConstWrite, kConstSkip, kCount };\n"
         "inline uint64_t g_n[kCount];\n"
         "inline bool g_skip_window_on = true;\n"
         "inline uint32_t g_calls = 0;\n"
         "inline std::chrono::steady_clock::time_point g_last;\n"
         "inline void CountOnGpuThread(Counter c) { ++g_n[c]; }\n"
         "void Report();\n"
         "}  // namespace cd_skip\n"
         "REXCVAR_DEFINE_INT32(cd_skip_unchanged_writes, 0, \"GPU\",\n"
         "                     \"Constant register writes that change no value invalidate nothing (0 off, 1 on, \"\n"
         "                     \"2 on and off in alternate 10 s windows, to measure it)\")\n"
         "    .range(0, 2);\n"
         "void cd_skip::Report() {  // [cd] every 10 s: how many constant writes changed nothing\n"
         "  if ((++g_calls & 4095) != 0) return;\n"
         "  const auto now = std::chrono::steady_clock::now();\n"
         "  if (g_last == std::chrono::steady_clock::time_point{}) { g_last = now; return; }\n"
         "  if (now - g_last < std::chrono::seconds(10)) return;\n"
         "  const int32_t mode = REXCVAR_GET(cd_skip_unchanged_writes);\n"
         "  REXGPU_INFO(\"cd unchanged constant writes: {} of {} in 10 s ({:.0f}%), skipping {}\", g_n[kConstSkip],\n"
         "              g_n[kConstWrite], g_n[kConstWrite] ? 100.0 * double(g_n[kConstSkip]) / double(g_n[kConstWrite]) : 0.0,\n"
         "              mode == 1 || (mode == 2 && g_skip_window_on) ? \"ON\" : \"OFF\");\n"
         "  if (mode == 2) g_skip_window_on = !g_skip_window_on;\n"
         "  g_n[kConstWrite] = g_n[kConstSkip] = 0;\n"
         "  g_last = now;\n"
         "}\n"
         "REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, \"GPU/Vulkan\",\n")
    edit(vcp,
         "void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {\n"
         "  CommandProcessor::WriteRegister(index, value);\n",
         "// [cd] cd_skip_unchanged_writes: whether it is on now\n"
         "static bool CdSkipOn() {\n"
         "  const int32_t mode = REXCVAR_GET(cd_skip_unchanged_writes);\n"
         f"  return mode == 1 || (mode == 2 && {P}g_skip_window_on);\n"
         "}\n"
         "// [cd] Copies the guest's (big-endian) values into the registers; the span of those that changed\n"
         "// in lo..hi, false when none did.\n"
         "static bool CdCopyChanged(uint32_t* dst, const uint32_t* src, uint32_t count, uint32_t& lo,\n"
         "                           uint32_t& hi) {\n"
         "  lo = UINT32_MAX, hi = 0;\n"
         "  for (uint32_t i = 0; i < count; ++i) {\n"
         "    const uint32_t value = memory::load_and_swap<uint32_t>(src + i);\n"
         "    if (dst[i] != value) {\n"
         "      dst[i] = value;\n"
         "      if (lo == UINT32_MAX) lo = i;\n"
         "      hi = i;\n"
         "    }\n"
         "  }\n"
         "  return lo != UINT32_MAX;\n"
         "}\n"
         "\n"
         "void VulkanCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {\n"
         "  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31 &&\n"
         "      (index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5 ||\n"
         "       index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031)) {\n"
         f"    {P}CountOnGpuThread({P}kConstWrite);  // [cd] a constant written with its own value: nothing to do\n"
         "    if (CdSkipOn() && register_file_->values[index] == value) {\n"
         f"      {P}CountOnGpuThread({P}kConstSkip);\n"
         "      return;\n"
         "    }\n"
         "  }\n"
         "  CommandProcessor::WriteRegister(index, value);\n")
    # float constants: only the changed span is checked against the shaders' constant maps
    edit(vcp,
         "    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n"
         "    if (frame_open_) {\n"
         "      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;\n"
         "      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;\n",
         "    uint32_t cd_first = start_index, cd_last = end_index;  // [cd] the span that changed\n"
         "    cd_skip::Report();\n"
         f"    {P}CountOnGpuThread({P}kConstWrite);\n"
         "    if (CdSkipOn()) {\n"
         "      uint32_t lo, hi;\n"
         "      if (!CdCopyChanged(register_file_->values + start_index, base, num_registers, lo, hi)) {\n"
         f"        {P}CountOnGpuThread({P}kConstSkip);\n"
         "        return;\n"
         "      }\n"
         "      cd_first = start_index + lo, cd_last = start_index + hi;\n"
         "    } else {\n"
         "      memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n"
         "    }\n"
         "    if (frame_open_) {\n"
         "      uint32_t first_float_constant = (cd_first - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;\n"
         "      uint32_t last_float_constant = (cd_last - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;\n")
    edit(vcp,
         "      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {\n"
         "    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n"
         "    current_constant_buffers_up_to_date_ &=\n",
         "      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {\n"
         f"    {P}CountOnGpuThread({P}kConstWrite);  // [cd]\n"
         "    if (CdSkipOn()) {\n"
         "      uint32_t lo, hi;\n"
         "      if (!CdCopyChanged(register_file_->values + start_index, base, num_registers, lo, hi)) {\n"
         f"        {P}CountOnGpuThread({P}kConstSkip);\n"
         "        return;\n"
         "      }\n"
         "    } else {\n"
         "      memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n"
         "    }\n"
         "    current_constant_buffers_up_to_date_ &=\n")
    # fetch constants: the texture bindings and vertex buffers of the changed span only
    edit(vcp,
         "      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {\n"
         "    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n"
         "    current_constant_buffers_up_to_date_ &=\n"
         "        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);\n"
         "    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;\n"
         "    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;\n",
         "      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {\n"
         "    uint32_t cd_first = start_index, cd_last = end_index;  // [cd] the span that changed\n"
         "    cd_skip::Report();\n"
         f"    {P}CountOnGpuThread({P}kConstWrite);\n"
         "    if (CdSkipOn()) {\n"
         "      uint32_t lo, hi;\n"
         "      if (!CdCopyChanged(register_file_->values + start_index, base, num_registers, lo, hi)) {\n"
         f"        {P}CountOnGpuThread({P}kConstSkip);\n"
         "        return;\n"
         "      }\n"
         "      cd_first = start_index + lo, cd_last = start_index + hi;\n"
         "    } else {\n"
         "      memory::copy_and_swap(register_file_->values + start_index, base, num_registers);\n"
         "    }\n"
         "    current_constant_buffers_up_to_date_ &=\n"
         "        ~(UINT32_C(1) << SpirvShaderTranslator::kConstantBufferFetch);\n"
         "    uint32_t first_fetch_dword = cd_first - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;\n"
         "    uint32_t last_fetch_dword = cd_last - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;\n")

    # [cd] "cd submit:" every 10 s: what the replay of the deferred command buffers (the "submit" ms of the perf line,
    # 11.9 ms a frame in Crackdown 2's play) spends its time on: commands of each kind a frame, and the replay ms a
    # frame split by kind (1 command in 32 timed). Counters only: nothing recorded or replayed changes.
    dcb = sdk / "src" / "graphics" / "vulkan" / "deferred_command_buffer.cpp"
    edit(dcb, "#include <cstring>\n", "#include <cstring>\n#include <algorithm>  // [cd]\n#include <chrono>  // [cd]\n#include <rex/logging.h>  // [cd]\n")
    edit(dcb,
         "  const uintmax_t* stream = commands.data();  // [cd]\n",
         "  // [cd] the replay's breakdown (counters only)\n"
         "  static uint64_t cd_count[32] = {}, cd_sampled_ns[32] = {}, cd_sampled[32] = {}, cd_replay_ns = 0, cd_streams = 0;\n"
         "  static uint32_t cd_tick = 0;\n"
         "  static auto cd_last = std::chrono::steady_clock::now();\n"
         "  const auto cd_begin = std::chrono::steady_clock::now();\n"
         "  ++cd_streams;\n"
         "  const uintmax_t* stream = commands.data();  // [cd]\n")
    edit(dcb,
         "    switch (header.command) {\n",
         "    const uint32_t cd_kind = uint32_t(header.command) & 31;  // [cd]\n"
         "    ++cd_count[cd_kind];\n"
         "    const bool cd_time = (++cd_tick & 31) == 0;\n"
         "    const auto cd_t0 = cd_time ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};\n"
         "    switch (header.command) {\n")
    edit(dcb,
         "    stream += header.arguments_size_elements;\n"
         "    stream_remaining -= header.arguments_size_elements;\n",
         "    if (cd_time) {  // [cd]\n"
         "      cd_sampled_ns[cd_kind] += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(\n"
         "          std::chrono::steady_clock::now() - cd_t0).count());\n"
         "      ++cd_sampled[cd_kind];\n"
         "    }\n"
         "    stream += header.arguments_size_elements;\n"
         "    stream_remaining -= header.arguments_size_elements;\n")
    # (at the end of the stream: the replay time, and the line every 10 s)
    text = dcb.read_text()
    tail = "}\n\n}  // namespace"
    i = text.index("void DeferredCommandBuffer::ExecuteStream")
    j = text.index("\n}\n", text.index("stream_remaining -= header.arguments_size_elements;", i))
    text = text[:j] + (
        "\n  {  // [cd]\n"
        "    const auto cd_end = std::chrono::steady_clock::now();\n"
        "    cd_replay_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(cd_end - cd_begin).count());\n"
        "    if (cd_end - cd_last >= std::chrono::seconds(10)) {\n"
        "      static const char* const kNames[] = {\"BeginRenderPass\", \"BeginQuery\", \"BindDescriptorSets\", \"BindIndexBuffer\",\n"
        "          \"BindPipeline\", \"BindVertexBuffers\", \"ClearAttachments\", \"ClearColorImage\", \"CopyBuffer\",\n"
        "          \"CopyBufferToImage\", \"CopyQueryPoolResults\", \"Dispatch\", \"Draw\", \"DrawIndexed\", \"EndQuery\",\n"
        "          \"EndRenderPass\", \"BeginRendering\", \"EndRendering\", \"PipelineBarrier\", \"PushConstants\",\n"
        "          \"ResetQueryPool\", \"SetBlendConstants\", \"SetDepthBias\", \"SetScissor\", \"SetStencilCompareMask\",\n"
        "          \"SetStencilReference\", \"SetStencilWriteMask\", \"SetViewport\"};\n"
        "      const double secs = std::chrono::duration<double>(cd_end - cd_last).count();\n"
        "      std::string list;\n"
        "      for (uint32_t k = 0; k < 28; ++k) {\n"
        "        if (!cd_count[k]) continue;\n"
        "        const double est_ms = cd_sampled[k] ? double(cd_sampled_ns[k]) / cd_sampled[k] * cd_count[k] / 1e6 : 0.0;\n"
        "        list += fmt::format(\" {} {:.0f}/s ~{:.1f} ms/s;\", kNames[k], cd_count[k] / secs, est_ms / secs);\n"
        "      }\n"
        "      REXGPU_INFO(\"cd submit: replay {:.1f} ms/s in {:.0f} streams/s;{}\", cd_replay_ns / 1e6 / secs,\n"
        "                  cd_streams / secs, list);\n"
        "      std::fill(std::begin(cd_count), std::end(cd_count), 0);\n"
        "      std::fill(std::begin(cd_sampled_ns), std::end(cd_sampled_ns), 0);\n"
        "      std::fill(std::begin(cd_sampled), std::end(cd_sampled), 0);\n"
        "      cd_replay_ns = cd_streams = 0;\n"
        "      cd_last = cd_end;\n"
        "    }\n"
        "  }\n") + text[j:]
    dcb.write_text(text)
    print("Crackdown: cd submit replay breakdown")


if __name__ == "__main__":
    main()
