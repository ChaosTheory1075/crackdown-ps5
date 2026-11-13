#!/usr/bin/env python3
"""The submit thread (from Driver San Francisco's dsf-ps5 815c67e, the same framework lineage): a submission's recorded
Vulkan commands are replayed and submitted on CPU 5 (empty in Crackdown's core layout) while the GPU thread records the
next one. Crackdown 2 (crackdown2_014, "cd perf"): submit 11-12 ms of a ~50 ms frame in play. The queue order is
unchanged: other queue users wait for the thread first, and fences are only awaited once the thread has submitted
them. cd_submit_thread is OFF by default (supervisor 2026-11-06: Driver SF's picture broke with it on; on again only once
its ordering against resolves and EDRAM copies is proven in code); true in the config turns it on for a test. Nothing drawn changes.

    fw_submit_thread.py <framework>     (after fw_speed_fixes.py: it uses cd_perf)
"""
import pathlib
import sys


def edit(path, old, new, count=1):
    text = path.read_text()
    if new in text:
        print(f"already  {path}")
        return
    if text.count(old) != count:
        sys.exit(f"fw_submit_thread: expected {count} of {old!r} in {path}, found {text.count(old)}")
    path.write_text(text.replace(old, new))
    print(f"patched  {path}")


def main():
    fw = pathlib.Path(sys.argv[1])
    sdk = fw / "glue" / "rexglue-sdk-main"
    CD_SUBMIT_THREAD_CLASS = r"""// [cd] The submit thread (see ci/patch_framework.py): a submission's recorded commands are
// replayed into its Vulkan command buffer and submitted here, in order, while the GPU thread
// records the next submission.
class CdSubmitThread {
 public:
  struct Job {
    std::vector<uintmax_t> stream;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkSemaphore> wait_semaphores;
    std::vector<VkPipelineStageFlags> wait_stage_masks;
    VkFence fence = VK_NULL_HANDLE;
    uint64_t submission = 0;
  };

  static CdSubmitThread& Get() {
    static CdSubmitThread thread;
    return thread;
  }

  // (from the GPU thread: the thread starts at the first submission)
  bool Active(VulkanCommandProcessor* command_processor) {
    if (state_ == kUnknown) {
      Start(command_processor);
    }
    return state_ == kRunning;
  }
  bool Running() const { return state_ == kRunning; }
  uint64_t Submitted() const { return submitted_.load(std::memory_order_acquire); }

  // An empty stream with room in it (one the thread is done with), or nothing.
  void TakeSpare(std::vector<uintmax_t>& stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!spares_.empty()) {
      stream = std::move(spares_.back());
      spares_.pop_back();
    }
  }

  void Push(Job&& job) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      jobs_.push_back(std::move(job));
    }
    work_cv_.notify_one();
  }

  // Until every submission up to `submission` is on the queue.
  void WaitSubmitted(uint64_t submission) {
    if (state_ != kRunning || Submitted() >= submission) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const uint64_t start = rex::cd_perf::NowUs();
    done_cv_.wait(lock, [&] {
      return submitted_.load(std::memory_order_acquire) >= submission || (jobs_.empty() && !busy_);
    });
    waited_us_ += rex::cd_perf::NowUs() - start;
    ++waits_;
  }

  // Until the thread has submitted everything handed to it (not on the thread itself).
  void Drain() {
    if (state_ != kRunning || on_thread_) {
      return;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    if (jobs_.empty() && !busy_) {
      return;
    }
    const uint64_t start = rex::cd_perf::NowUs();
    done_cv_.wait(lock, [&] { return jobs_.empty() && !busy_; });
    waited_us_ += rex::cd_perf::NowUs() - start;
    ++waits_;
  }

 private:
  enum State { kUnknown, kOff, kRunning };

  void Start(VulkanCommandProcessor* command_processor) {
    command_processor_ = command_processor;
    state_ = REXCVAR_GET(cd_submit_thread) ? kRunning : kOff;
    REXGPU_INFO("cd submit thread: {}", state_ == kRunning ? "on (CPU 5)" : "off");
    if (state_ == kRunning) {
      ui::vulkan::g_before_queue_acquire = [] { CdSubmitThread::Get().Drain(); };
      std::thread([this] { Main(); }).detach();
    }
  }

  void Main() {
    on_thread_ = true;
#if defined(__PROSPERO__)
    // CPU 5: the lane's twin, which nothing else uses
    rex::frame_stats::scePthreadSetaffinity(pthread_self(), uint64_t(1) << 5);
#endif
    rex::ps5diag::RegisterThread("gpu submit");
    for (;;) {
      Job job;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        work_cv_.wait(lock, [&] { return !jobs_.empty(); });
        job = std::move(jobs_.front());
        jobs_.pop_front();
        busy_ = true;
      }
      const uint64_t start = rex::cd_perf::NowUs();
      Run(job);
      const uint64_t end = rex::cd_perf::NowUs();
      job.stream.clear();
      {
        std::lock_guard<std::mutex> lock(mutex_);
        busy_ = false;
        submitted_.store(job.submission, std::memory_order_release);
        busy_us_ += end - start;
        ++done_;
        if (spares_.size() < 8) {
          spares_.push_back(std::move(job.stream));
        }
      }
      done_cv_.notify_all();
      if (!report_us_) {
        report_us_ = end;
      } else if (end - report_us_ >= 10000000) {
        uint64_t busy_us, waited_us, waits, done;
        {
          std::lock_guard<std::mutex> lock(mutex_);
          busy_us = busy_us_;
          waited_us = waited_us_;
          waits = waits_;
          done = done_;
          busy_us_ = waited_us_ = waits_ = done_ = 0;
        }
        const double seconds = double(end - report_us_) / 1000000.0;
        report_us_ = end;
        const uint64_t lane_waited_us = lane_waited_us_;
        lane_waited_us_ = 0;
        REXGPU_INFO(
            "cd submit thread: {} submissions in {:.1f} s, {:.1f} ms a second replaying and "
            "submitting ({:.1f} of it waiting for the GPU lane); waited for {} times, {:.1f} ms a second",
            done, seconds, double(busy_us) / 1000.0 / seconds, double(lane_waited_us) / 1000.0 / seconds, waits,
            double(waited_us) / 1000.0 / seconds);
      }
    }
  }

  void Run(Job& job) {
    const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_->GetVulkanDevice();
    const ui::vulkan::VulkanDevice::Functions& dfn = vulkan_device->functions();
    const VkDevice device = vulkan_device->device();
    bool recorded = dfn.vkResetCommandPool(device, job.pool, 0) == VK_SUCCESS;
    if (recorded) {
      VkCommandBufferBeginInfo begin_info = {};
      begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      recorded = dfn.vkBeginCommandBuffer(job.command_buffer, &begin_info) == VK_SUCCESS;
    }
    if (recorded) {
      command_processor_->deferred_command_buffer().ExecuteStream(job.command_buffer, job.stream);
      recorded = dfn.vkEndCommandBuffer(job.command_buffer) == VK_SUCCESS;
    }
    if (!recorded) {
      REXGPU_ERROR("cd submit thread: failed to record a Vulkan command buffer");
    }
    VkSubmitInfo submit_info = {};
    submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    if (!job.wait_semaphores.empty()) {
      submit_info.waitSemaphoreCount = uint32_t(job.wait_semaphores.size());
      submit_info.pWaitSemaphores = job.wait_semaphores.data();
      submit_info.pWaitDstStageMask = job.wait_stage_masks.data();
    }
    submit_info.commandBufferCount = recorded ? 1 : 0;
    submit_info.pCommandBuffers = &job.command_buffer;
    VkResult result;
    {
      ui::vulkan::VulkanDevice::Queue::Acquisition queue_acquisition =
          vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
      result = dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, job.fence);
      if (result != VK_SUCCESS && result != VK_ERROR_DEVICE_LOST) {
        // (the fence still signals: the GPU thread waits on it)
        VkSubmitInfo empty_submit = {};
        empty_submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &empty_submit, job.fence);
      }
    }
    if (result != VK_SUCCESS) {
      // (a lost device shows up at the GPU thread's next fence wait)
      REXGPU_ERROR("cd submit thread: failed to submit a Vulkan command buffer ({})", int(result));
    }
  }

  static inline thread_local bool on_thread_ = false;
  State state_ = kUnknown;
  VulkanCommandProcessor* command_processor_ = nullptr;
  std::mutex mutex_;
  std::condition_variable work_cv_, done_cv_;
  std::deque<Job> jobs_;
  std::vector<std::vector<uintmax_t>> spares_;
  bool busy_ = false;
  std::atomic<uint64_t> submitted_{0};
  uint64_t busy_us_ = 0, waited_us_ = 0, waits_ = 0, done_ = 0, report_us_ = 0;
};
"""

    # [cd] The submit thread (2026-11-05, the perf board): the GPU thread spent 3-5.5 ms of each
    # 17-22 ms frame in EndSubmission replaying its recorded commands into the Vulkan command
    # buffer (radv) and submitting them. That replay now runs on a thread of its own (CPU 5, the
    # lane's twin, empty until now) while the GPU thread goes on with the next submission's draws.
    # The queue sees the same order as before: anyone else acquiring it (the presenter, sparse
    # binds) first waits for the thread to be empty, and the GPU thread never waits on or polls
    # a fence the thread hasn't submitted yet. Nothing drawn changes. cd_submit_thread = false
    # turns it off.
    vk = sdk / "include" / "rex" / "ui" / "vulkan" / "device.h"
    edit(vk, "class VulkanDevice {\n",
         "// [cd] (set by the submit thread, command_processor.cpp: called before a queue is acquired\n"
         "// by a thread that doesn't hold one already)\n"
         "inline void (*g_before_queue_acquire)() = nullptr;\n"
         "inline thread_local int g_queue_acquisitions = 0;\n\n"
         "class VulkanDevice {\n")
    edit(vk, "      explicit Acquisition(Queue& queue) : lock_(queue.mutex), queue_(queue.queue) {}\n",
         "      explicit Acquisition(Queue& queue) : lock_(queue.mutex), queue_(queue.queue) {\n"
         "        ++g_queue_acquisitions;  // [cd]\n"
         "      }\n"
         "      Acquisition(Acquisition&& other) = default;\n"
         "      ~Acquisition() {\n"
         "        if (lock_.owns_lock()) {\n"
         "          --g_queue_acquisitions;\n"
         "        }\n"
         "      }\n")
    edit(vk, "                                  const uint32_t queue_index) const {\n"
             "    return queue_families()[queue_family_index].queues[queue_index]->Acquire();\n",
         "                                  const uint32_t queue_index) const {\n"
         "    if (g_before_queue_acquire && !g_queue_acquisitions) {\n"
         "      g_before_queue_acquire();  // [cd] the submit thread's submissions go first\n"
         "    }\n"
         "    return queue_families()[queue_family_index].queues[queue_index]->Acquire();\n")

    dcb_h = sdk / "include" / "rex" / "graphics" / "vulkan" / "deferred_command_buffer.h"
    edit(dcb_h, "  void Execute(VkCommandBuffer command_buffer);\n",
         "  void Execute(VkCommandBuffer command_buffer);\n"
         "  // [cd] (the submit thread's: the recorded commands handed over, an empty stream in their\n"
         "  // place; and replayed from there)\n"
         "  void TakeStream(std::vector<uintmax_t>& stream) {\n"
         "    stream.swap(command_stream_);\n"
         "    command_stream_.clear();\n"
         "  }\n"
         "  void ExecuteStream(VkCommandBuffer command_buffer, const std::vector<uintmax_t>& commands);\n")
    dcb = sdk / "src" / "graphics" / "vulkan" / "deferred_command_buffer.cpp"
    edit(dcb, "void DeferredCommandBuffer::Execute(VkCommandBuffer command_buffer) {\n",
         "void DeferredCommandBuffer::Execute(VkCommandBuffer command_buffer) {\n"
         "  ExecuteStream(command_buffer, command_stream_);\n"
         "}\n\n"
         "void DeferredCommandBuffer::ExecuteStream(VkCommandBuffer command_buffer,\n"
         "                                          const std::vector<uintmax_t>& commands) {\n")
    edit(dcb, "  const uintmax_t* stream = command_stream_.data();\n"
              "  size_t stream_remaining = command_stream_.size();\n",
         "  const uintmax_t* stream = commands.data();  // [cd]\n"
         "  size_t stream_remaining = commands.size();\n")

    edit(sdk / "src" / "core" / "ps5_diag.cpp",
         '!std::strcmp(e.what, "gpu lane"))\n',
         '!std::strcmp(e.what, "gpu lane") ||\n'
         '        !std::strcmp(e.what, "gpu submit"))  // [cd] (the submit thread: CPU 5)\n')

    cp = sdk / "src" / "graphics" / "vulkan" / "command_processor.cpp"
    edit(cp, "#include <mutex>\n#include <string>\n",
         "#include <mutex>\n#include <condition_variable>  // [cd] (the submit thread)\n#include <deque>\n"
         "#include <thread>\n#include <string>\n")
    edit(cp, 'REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, "GPU/Vulkan",\n',
         'REXCVAR_DEFINE_BOOL(cd_submit_thread, false, "GPU/Vulkan",\n'
         '                    "[cd] Replay and submit each submission\'s Vulkan commands on a thread of "\n'
         '                    "its own (CPU 5) while the GPU thread goes on with the next draws");\n\n'
         'REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, "GPU/Vulkan",\n')
    edit(cp, "VulkanCommandProcessor::VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,\n",
         CD_SUBMIT_THREAD_CLASS + "\nVulkanCommandProcessor::VulkanCommandProcessor(VulkanGraphicsSystem* graphics_system,\n")
    edit(cp, "  GpuLane::Get().Drain();  // [crackdown]\n  AwaitAllQueueOperationsCompletion();\n",
         "  GpuLane::Get().Drain();  // [crackdown]\n"
         "  CdSubmitThread::Get().Drain();  // [cd]\n"
         "  AwaitAllQueueOperationsCompletion();\n")
    edit(cp, "  size_t fences_total = submissions_in_flight_fences_.size();\n"
             "  size_t fences_awaited = 0;\n",
         "  size_t fences_total = submissions_in_flight_fences_.size();\n"
         "  if (CdSubmitThread::Get().Running()) {\n"
         "    // [cd] only fences the submit thread has submitted are awaited or polled\n"
         "    if (await_submission > submission_completed_) {\n"
         "      rex::cd_perf::Scope cd_perf_submit_wait(rex::cd_perf::kSubmit);\n"
         "      CdSubmitThread::Get().WaitSubmitted(await_submission);\n"
         "    }\n"
         "    const uint64_t submitted = CdSubmitThread::Get().Submitted();\n"
         "    fences_total = std::min(fences_total, submitted > submission_completed_\n"
         "                                              ? size_t(submitted - submission_completed_)\n"
         "                                              : size_t(0));\n"
         "  }\n"
         "  size_t fences_awaited = 0;\n")
    edit(cp, "    SubmitBarriers(true);\n\n"
             "    assert_false(command_buffers_writable_.empty());\n"
             "    CommandBuffer command_buffer = command_buffers_writable_.back();\n"
             "    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {\n",
         "    SubmitBarriers(true);\n\n"
         "    if (CdSubmitThread::Get().Active(this)) {\n"
         "      // [cd] replayed and submitted on the submit thread; the bookkeeping as below\n"
         "      assert_false(command_buffers_writable_.empty());\n"
         "      CommandBuffer command_buffer = command_buffers_writable_.back();\n"
         "      assert_false(fences_free_.empty());\n"
         "      VkFence fence = fences_free_.back();\n"
         "      if (dfn.vkResetFences(device, 1, &fence) != VK_SUCCESS) {\n"
         "        REXGPU_ERROR(\"Failed to reset a Vulkan submission fence\");\n"
         "        return false;\n"
         "      }\n"
         "      uint64_t submission_current = GetCurrentSubmission();\n"
         "      CdSubmitThread::Job job;\n"
         "      CdSubmitThread::Get().TakeSpare(job.stream);\n"
         "      deferred_command_buffer_.TakeStream(job.stream);\n"
         "      job.command_buffer = command_buffer.buffer;\n"
         "      job.pool = command_buffer.pool;\n"
         "      job.wait_semaphores = current_submission_wait_semaphores_;\n"
         "      job.wait_stage_masks = current_submission_wait_stage_masks_;\n"
         "      job.fence = fence;\n"
         "      job.submission = submission_current;\n"
         "      CdSubmitThread::Get().Push(std::move(job));\n"
         "      current_submission_wait_stage_masks_.clear();\n"
         "      for (VkSemaphore semaphore : current_submission_wait_semaphores_) {\n"
         "        submissions_in_flight_semaphores_.emplace_back(submission_current, semaphore);\n"
         "      }\n"
         "      current_submission_wait_semaphores_.clear();\n"
         "      command_buffers_submitted_.emplace_back(submission_current, command_buffer);\n"
         "      command_buffers_writable_.pop_back();\n"
         "      submissions_in_flight_fences_.push_back(fence);\n"
         "      fences_free_.pop_back();\n"
         "      submission_open_ = false;\n"
         "    } else {\n"
         "    assert_false(command_buffers_writable_.empty());\n"
         "    CommandBuffer command_buffer = command_buffers_writable_.back();\n"
         "    if (dfn.vkResetCommandPool(device, command_buffer.pool, 0) != VK_SUCCESS) {\n")
    edit(cp, "    submission_open_ = false;\n  }\n\n  if (is_closing_frame) {\n",
         "    submission_open_ = false;\n    }  // [cd] (the submit thread's else)\n  }\n\n  if (is_closing_frame) {\n")

    # [cd] The lane waited for on the submit thread (cd_lane_wait_on_submit, 2026-11-07). EndSubmission drained the
    # GPU lane (its upload copies, page hashes, watches and descriptor writes) on the GPU thread before handing the
    # submission over: in Crackdown 2's heavy scenes (crackdown2_019) the GPU thread waited for the lane 700-1300
    # times in 10 s, 3.6-5.6 ms a frame. With the submit thread, only the replay and vkQueueSubmit need that work
    # done (the descriptor sets before their binds are recorded, the copies before the GPU reads them), so the
    # submit thread waits for every lane job pushed up to the hand-off, and the GPU thread goes on with the next
    # draws. Every point where the game sees the GPU's progress still drains the lane on the GPU thread first
    # (read pointer, scratch, interrupts, MEM_WRITE, REG_TO_MEM, COND_WRITE, EVENT_WRITE_SHD), and so does the
    # stale page check. The upload buffers are flushed by the GPU thread at EndSubmission, so this is only done
    # when every host-visible memory type is coherent (the flush is then a no-op); otherwise the drain stays.
    # OFF (no app turns it on): with the lane slowed (CD_LANE_SLOW, replay only) Driver SF's trace replay hung once
    # in a write-fault loop (the GPU thread writing a page still protected by a lane watch), 2026-11-07.
    lane = sdk / "include" / "rex" / "graphics" / "gpu_lane.h"
    edit(lane, "  // Where uploads come from (any thread):",
         "  // [cd] (the submit thread's, ci/fw_submit_thread.py) jobs pushed so far, from the GPU thread; and a wait\n"
         "  // from another thread until every job up to `target` is done.\n"
         "  uint64_t Pushed() const { return pushed_; }\n"
         "  bool DoneUpTo(uint64_t target) const { return done_.load(std::memory_order_acquire) >= target; }\n"
         "  void WaitDone(uint64_t target) {\n"
         "    if (done_.load(std::memory_order_acquire) >= target) {\n"
         "      return;\n"
         "    }\n"
         "    std::unique_lock<std::mutex> lock(mutex_);\n"
         "    ++other_waiters_;\n"
         "    done_cv_.wait(lock, [&] { return done_.load(std::memory_order_acquire) >= target; });\n"
         "    --other_waiters_;\n"
         "  }\n\n"
         "  // Where uploads come from (any thread):")
    # [cd] Where the GPU thread waits for the lane (2026-11-07): crackdown2_019's 4-5 waits a frame (~1 ms each) don't
    # say at which packet. Drain() is out of line and tallies its waits by return address (the call site), the top
    # ones in the 10 s "gpu:" report with an anchor address to symbolize them against the ELF. Counters only: Drain
    # already read the clock for its wait.
    edit(lane, "  void Drain() {\n    if (Idle()) {\n      return;\n    }\n",
         "  __attribute__((noinline)) void Drain() {\n    if (Idle()) {\n      return;\n    }\n"
         "    const uintptr_t cd_site = reinterpret_cast<uintptr_t>(__builtin_return_address(0));  // [cd]\n")
    edit(lane, "    ++drains_;\n    drain_wait_us_ += uint64_t(\n"
               "        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)\n"
               "            .count());\n",
         "    ++drains_;\n    const uint64_t cd_us = uint64_t(\n"
         "        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start)\n"
         "            .count());\n"
         "    drain_wait_us_ += cd_us;\n"
         "    for (uint32_t i = 0; i < kCdSites; ++i) {  // [cd] (the call site's tally; the last slot: the rest)\n"
         "      if (cd_sites_[i].site == cd_site || !cd_sites_[i].site || i == kCdSites - 1) {\n"
         "        cd_sites_[i].site = i == kCdSites - 1 && cd_sites_[i].site != cd_site ? 1 : cd_site;\n"
         "        ++cd_sites_[i].count;\n"
         "        cd_sites_[i].us += cd_us;\n"
         "        break;\n"
         "      }\n"
         "    }\n")
    edit(lane, "    const uint64_t hashed = pages_hashed.exchange(0), unchanged = pages_unchanged.exchange(0);\n",
         "    {  // [cd] where the command thread waited (call sites; anchor = GpuLaneSiteAnchor's address)\n"
         "      std::sort(std::begin(cd_sites_), std::end(cd_sites_),\n"
         "                [](const CdSite& a, const CdSite& b) { return a.us > b.us; });\n"
         "      std::string list;\n"
         "      for (const CdSite& s : cd_sites_) {\n"
         "        if (s.count) list += fmt::format(\" {:X}:{}x/{}ms\", s.site, s.count, s.us / 1000);\n"
         "      }\n"
         "      REXGPU_INFO(\"gpu lane waits by call site (anchor {:X}):{}\",\n"
         "                  reinterpret_cast<uintptr_t>(&GpuLaneSiteAnchor), list);\n"
         "      for (CdSite& s : cd_sites_) s = {};\n"
         "    }\n"
         "    const uint64_t hashed = pages_hashed.exchange(0), unchanged = pages_unchanged.exchange(0);\n")
    edit(lane, "  uint32_t frames_ = 0;\n  std::chrono::steady_clock::time_point report_start_;\n};\n",
         "  uint32_t frames_ = 0;\n  std::chrono::steady_clock::time_point report_start_;\n"
         "  struct CdSite {\n    uintptr_t site;\n    uint64_t count, us;\n  };\n"
         "  static constexpr uint32_t kCdSites = 16;\n"
         "  CdSite cd_sites_[kCdSites] = {};\n};\n")
    edit(lane, "#include <vector>\n", "#include <vector>\n#include <iterator>\n#include <string>  // [cd]\n")
    edit(lane, "class GpuLane {\n",
         "// [cd] (an address in the image, printed beside the lane waits' call sites to symbolize them)\n"
         "__attribute__((noinline, used)) inline void GpuLaneSiteAnchor() { asm volatile(\"\"); }\n\n"
         "class GpuLane {\n")
    edit(lane, "        notify = drain_waiting_;\n",
         "        notify = drain_waiting_ || other_waiters_;  // [cd] (other_waiters_: the submit thread)\n")
    edit(lane, "  bool sleeping_ = false, drain_waiting_ = false;  // (under mutex_)\n",
         "  bool sleeping_ = false, drain_waiting_ = false;  // (under mutex_)\n"
         "  uint32_t other_waiters_ = 0;                      // [cd] (under mutex_)\n")

    edit(cp, 'REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, "GPU/Vulkan",\n',
         'REXCVAR_DEFINE_BOOL(cd_lane_wait_on_submit, false, "GPU/Vulkan",\n'
         '                    "[cd] With cd_submit_thread: the submit thread, not the GPU thread, waits for the GPU "\n'
         '                    "lane\'s copies and descriptor writes before a submission is replayed and submitted");\n\n'
         'REXCVAR_DEFINE_BOOL(vulkan_dynamic_rendering, true, "GPU/Vulkan",\n')
    edit(cp, "    VkFence fence = VK_NULL_HANDLE;\n    uint64_t submission = 0;\n  };\n",
         "    VkFence fence = VK_NULL_HANDLE;\n    uint64_t submission = 0;\n"
         "    uint64_t lane_target = 0;  // (cd_lane_wait_on_submit: lane jobs done before the replay)\n  };\n")
    edit(cp, "  void Run(Job& job) {\n"
             "    const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_->GetVulkanDevice();\n",
         "  void Run(Job& job) {\n"
         "    if (job.lane_target) {\n"
         "      const uint64_t lane_start = rex::cd_perf::NowUs();\n"
         "      const bool lane_busy = !GpuLane::Get().DoneUpTo(job.lane_target);\n"
         "      GpuLane::Get().WaitDone(job.lane_target);\n"
         "      lane_waited_us_ += rex::cd_perf::NowUs() - lane_start;\n"
         "      if (lane_busy && !lane_wait_logged_) {\n"
         "        lane_wait_logged_ = true;\n"
         "        REXGPU_INFO(\"cd lane wait on submit: submission {} waited {} us for the GPU lane on the submit \"\n"
         "                    \"thread (once)\", job.submission, rex::cd_perf::NowUs() - lane_start);\n"
         "      }\n"
         "    }\n"
         "    const ui::vulkan::VulkanDevice* const vulkan_device = command_processor_->GetVulkanDevice();\n")
    edit(cp, "  uint64_t busy_us_ = 0, waited_us_ = 0, waits_ = 0, done_ = 0, report_us_ = 0;\n",
         "  uint64_t busy_us_ = 0, waited_us_ = 0, waits_ = 0, done_ = 0, report_us_ = 0;\n"
         "  uint64_t lane_waited_us_ = 0;  // (the submit thread's own)\n"
         "  bool lane_wait_logged_ = false;\n")
    edit(cp, "  // [crackdown] the GPU's second lane's copies and descriptor writes done first\n"
             "  GpuLane::Get().Drain();\n",
         "  // [crackdown] the GPU's second lane's copies and descriptor writes done first\n"
         "  // [cd] (cd_lane_wait_on_submit: by the submit thread, before it replays this submission)\n"
         "  const auto& cd_memory_types = GetVulkanDevice()->memory_types();\n"
         "  const bool cd_lane_on_submit = REXCVAR_GET(cd_lane_wait_on_submit) && CdSubmitThread::Get().Running() &&\n"
         "                                 !(cd_memory_types.host_visible & ~cd_memory_types.host_coherent);\n"
         "  if (static bool cd_logged = false; !cd_logged && CdSubmitThread::Get().Running()) {\n"
         "    cd_logged = true;\n"
         "    REXGPU_INFO(\"cd lane wait on submit: {} (cd_lane_wait_on_submit {}, host-visible memory {})\",\n"
         "                cd_lane_on_submit ? \"on (the submit thread waits for the GPU lane)\" : \"off\",\n"
         "                REXCVAR_GET(cd_lane_wait_on_submit),\n"
         "                (cd_memory_types.host_visible & ~cd_memory_types.host_coherent) ? \"not all coherent\"\n"
         "                                                                                : \"all coherent\");\n"
         "  }\n"
         "  if (!cd_lane_on_submit) {\n"
         "    GpuLane::Get().Drain();\n"
         "  }\n")
    edit(cp, "      job.submission = submission_current;\n      CdSubmitThread::Get().Push(std::move(job));\n",
         "      job.submission = submission_current;\n"
         "      if (cd_lane_on_submit) {\n"
         "        job.lane_target = GpuLane::Get().Pushed();\n"
         "      }\n"
         "      CdSubmitThread::Get().Push(std::move(job));\n")


if __name__ == "__main__":
    main()
