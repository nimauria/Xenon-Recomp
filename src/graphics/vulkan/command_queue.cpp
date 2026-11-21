#include "xenon/gpu/vulkan/command_queue.hpp"

#include <stdexcept>
#include <vector>

#include "xenon/gpu/perf_stats.hpp"
#include "xenon/logging/activity_trace.hpp"

namespace xenon::gpu::vulkan {

CommandQueue::~CommandQueue() {
  (void)wait_idle();
  reset();
}

bool CommandQueue::initialize(VkDevice device, VkQueue queue,
                              std::uint32_t queue_family) {
  reset();
  error_.clear();
  if (!device || !queue) {
    error_ = "Vulkan command queue requires a device and queue";
    return false;
  }
  device_ = device;
  queue_ = queue;
  queue_family_ = queue_family;
  // Each frame starts with one segment (its first primary); more are created
  // as batches need them and kept for later batches.
  for (auto& frame : frames_) {
    if (!acquire_segment(frame)) {
      reset();
      return false;
    }
    frame.segments_used = 0;
  }
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  semaphore.pNext = &type;
  if (vkCreateSemaphore(device_, &semaphore, nullptr, &timeline_) != VK_SUCCESS) {
    error_ = "vkCreateSemaphore failed";
    reset();
    return false;
  }
  workers_ = std::make_unique<RecordingWorkers>(RecordingWorkers::default_thread_count());
  return true;
}

bool CommandQueue::enable_timestamps(VkPhysicalDevice physical_device) {
  if (!device_ || !physical_device) return false;
  std::uint32_t families = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &families, nullptr);
  std::vector<VkQueueFamilyProperties> properties(families);
  vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &families, properties.data());
  if (queue_family_ >= families || !properties[queue_family_].timestampValidBits) return false;
  const auto bits = properties[queue_family_].timestampValidBits;
  timestamp_mask_ = bits >= 64u ? ~0ull : ((1ull << bits) - 1u);
  VkPhysicalDeviceProperties device_properties{};
  vkGetPhysicalDeviceProperties(physical_device, &device_properties);
  timestamp_period_ns_ = device_properties.limits.timestampPeriod;
  if (timestamp_period_ns_ <= 0.0) return false;
  for (auto& frame : frames_) {
    VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    info.queryType = VK_QUERY_TYPE_TIMESTAMP;
    info.queryCount = 2;
    if (vkCreateQueryPool(device_, &info, nullptr, &frame.timestamps) != VK_SUCCESS) {
      for (auto& clear : frames_) {
        if (clear.timestamps) vkDestroyQueryPool(device_, clear.timestamps, nullptr);
        clear.timestamps = VK_NULL_HANDLE;
      }
      return false;
    }
  }
  return true;
}

void CommandQueue::read_timestamps(FrameContext& frame) {
  if (!frame.timestamps_pending) return;
  frame.timestamps_pending = false;
  std::uint64_t ticks[2]{};
  if (vkGetQueryPoolResults(device_, frame.timestamps, 0, 2, sizeof(ticks), ticks,
                            sizeof(std::uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
    return;
  ticks[0] &= timestamp_mask_;
  ticks[1] &= timestamp_mask_;
  if (ticks[1] < ticks[0]) return;
  const auto to_ns = [this](std::uint64_t value) {
    return static_cast<std::uint64_t>(static_cast<double>(value) * timestamp_period_ns_);
  };
  const auto busy = to_ns(ticks[1] - ticks[0]);
  perf::add(perf::Counter::GpuBusyNs, busy);
  perf::add(perf::Counter::GpuBatches, 1u);
  // Batches complete and are read in submission order: the gap since the
  // previous one's end is time the GPU had nothing to do.
  if (last_batch_end_ && ticks[0] > last_batch_end_)
    perf::add(perf::Counter::GpuIdleNs, to_ns(ticks[0] - last_batch_end_));
  last_batch_end_ = ticks[1];
  ++timed_batches_;
  timed_gpu_ns_ += busy;
}

void CommandQueue::collect_timestamps() {
  const auto completed = completed_value();
  for (std::uint32_t i = 0; i < kFrameCount; ++i) {
    auto& frame = frames_[(next_frame_ + i) % kFrameCount];
    if (frame.timestamps_pending && completed >= frame.timeline_value) read_timestamps(frame);
  }
}

bool CommandQueue::set_recording_threads(unsigned threads) {
  if (!flush()) return false;
  workers_ = std::make_unique<RecordingWorkers>(threads);
  return true;
}

VkCommandBuffer CommandQueue::acquire_segment(FrameContext& frame) {
  if (frame.segments_used == frame.segments_pool.size()) {
    // A pool of its own per segment: a worker records into it while the
    // owner records elsewhere (command pools are externally synchronized).
    Segment segment{};
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool.queueFamilyIndex = queue_family_;
    if (!device_ || vkCreateCommandPool(device_, &pool, nullptr, &segment.pool) != VK_SUCCESS) {
      error_ = "vkCreateCommandPool failed";
      return VK_NULL_HANDLE;
    }
    VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate.commandPool = segment.pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate, &segment.command) != VK_SUCCESS) {
      vkDestroyCommandPool(device_, segment.pool, nullptr);
      error_ = "vkAllocateCommandBuffers failed";
      return VK_NULL_HANDLE;
    }
    frame.segments_pool.push_back(segment);
  }
  const auto command = frame.segments_pool[frame.segments_used].command;
  if (frame.recording) {
    // The pool was reset when the batch began (its last use completed).
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin) != VK_SUCCESS) {
      error_ = "vkBeginCommandBuffer failed";
      return VK_NULL_HANDLE;
    }
  }
  ++frame.segments_used;
  return command;
}

VkCommandBuffer CommandQueue::open_primary() {
  auto& frame = frames_[current_frame_];
  if (frame.primary) return frame.primary;
  const auto command = acquire_segment(frame);
  if (!command) return VK_NULL_HANDLE;
  frame.primary = command;
  frame.segments.push_back(command);
  return command;
}

void CommandQueue::before_ordered_work() {
  if ((!before_ordered_work_ && !pending_work_flush_) || in_before_ordered_work_) return;
  in_before_ordered_work_ = true;
  struct Clear {
    bool& flag;
    ~Clear() { flag = false; }
  } clear{in_before_ordered_work_};
  if (before_ordered_work_) before_ordered_work_();
  if (pending_work_flush_) pending_work_flush_();
}

bool CommandQueue::reserve_slot(std::uint32_t& frame_index, std::uint32_t& slot) {
  if (!begin_batch()) return false;
  if (frames_[current_frame_].command_count >= kBatchCommandCount &&
      (!flush() || !begin_batch()))
    return false;
  auto& frame = frames_[current_frame_];
  frame_index = current_frame_;
  slot = frame.command_count++;
  return true;
}

bool CommandQueue::record_run(std::function<void(VkCommandBuffer)> record, bool parallel) {
  if (current_frame_ == UINT32_MAX) {
    error_ = "Vulkan draw run recorded with no open batch";
    return false;
  }
  auto& frame = frames_[current_frame_];
  if (!parallel || !workers_ || !workers_->thread_count()) {
    const auto command = open_primary();
    if (!command) return false;
    record(command);
    return true;
  }
  // The run becomes a segment of its own, after the primary work before it;
  // ordered work after it goes to a new primary segment.
  if (frame.primary) {
    if (vkEndCommandBuffer(frame.primary) != VK_SUCCESS) {
      error_ = "vkEndCommandBuffer failed for a primary segment";
      return false;
    }
    frame.primary = VK_NULL_HANDLE;
  }
  const auto command = acquire_segment(frame);
  if (!command) return false;
  frame.segments.push_back(command);
  ++parallel_runs_;
  workers_->submit([record = std::move(record), command] {
    record(command);
    if (vkEndCommandBuffer(command) != VK_SUCCESS)
      throw std::runtime_error("vkEndCommandBuffer failed for a draw-run segment");
  });
  return true;
}

void CommandQueue::reset() noexcept {
  workers_.reset();
  if (device_ && timeline_) vkDestroySemaphore(device_, timeline_, nullptr);
  timeline_ = VK_NULL_HANDLE;
  for (auto& frame : frames_) {
    if (device_) {
      for (const auto& segment : frame.segments_pool)
        if (segment.pool) vkDestroyCommandPool(device_, segment.pool, nullptr);
      if (frame.timestamps) vkDestroyQueryPool(device_, frame.timestamps, nullptr);
    }
    frame = {};
  }
  queue_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  next_frame_ = 0;
  current_frame_ = UINT32_MAX;
  next_value_ = 1;
  last_submitted_value_ = 0;
}

bool CommandQueue::wait_for_value(std::uint64_t value) {
  if (!value) return true;
  if (!device_ || !timeline_) {
    error_ = "Vulkan timeline wait requested on an uninitialized queue";
    return false;
  }
  // Activity trace: how long the CPU waits on the GPU.
  static const auto activity_key = xenon::logging::intern_activity(
      {"gpu", "fence wait", {"value", "submitted", "", ""}, false});
  xenon::logging::ActivityScope activity(activity_key, {value, last_submitted_value_, 0u, 0u});
  // Only a wait that blocks is counted.
  std::uint64_t completed = 0;
  if (vkGetSemaphoreCounterValue(device_, timeline_, &completed) == VK_SUCCESS &&
      completed >= value)
    return true;
  const xenon::gpu::perf::ScopedTime wait_time(xenon::gpu::perf::Counter::FenceWaitNs, xenon::gpu::perf::Counter::FenceWaits);
  VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wait.semaphoreCount = 1;
  wait.pSemaphores = &timeline_;
  wait.pValues = &value;
  if (vkWaitSemaphores(device_, &wait, UINT64_MAX) != VK_SUCCESS) {
    error_ = "vkWaitSemaphores failed";
    return false;
  }
  return true;
}

bool CommandQueue::wait_completed_concurrent(std::uint64_t value,
                                             std::uint32_t timeout_ms) const noexcept {
  if (!value) return true;
  if (!device_ || !timeline_) return false;
  // Waiting on a timeline semaphore needs no external synchronization.
  VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
  wait.semaphoreCount = 1;
  wait.pSemaphores = &timeline_;
  wait.pValues = &value;
  return vkWaitSemaphores(device_, &wait,
                          std::uint64_t{timeout_ms} * 1000000ull) == VK_SUCCESS;
}

bool CommandQueue::begin_batch() {
  if (current_frame_ != UINT32_MAX) return true;
  if (!device_ || !queue_ || !timeline_) {
    error_ = "Vulkan command queue is not initialized";
    return false;
  }
  auto& frame = frames_[next_frame_];
  if (!wait_for_value(frame.timeline_value)) return false;
  for (std::uint32_t i = 0; i < frame.segments_used; ++i) {
    if (vkResetCommandPool(device_, frame.segments_pool[i].pool, 0) != VK_SUCCESS) {
      error_ = "vkResetCommandPool failed";
      return false;
    }
  }
  frame.segments_used = 0;
  frame.segments.clear();
  frame.primary = VK_NULL_HANDLE;
  frame.command_count = 0;
  frame.recording = true;
  current_frame_ = next_frame_;
  // The frame's previous batch has completed (waited for above): read its
  // timestamps, then start timing this one.
  read_timestamps(frame);
  if (frame.timestamps) {
    if (const auto command = open_primary()) {
      vkCmdResetQueryPool(command, frame.timestamps, 0, 2);
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, frame.timestamps, 0);
    }
  }
  return true;
}

bool CommandQueue::execute_async(const std::function<void(
    VkCommandBuffer, std::uint32_t, std::uint32_t)>& record) {
  if (!begin_batch()) return false;
  before_ordered_work();
  if (frames_[current_frame_].command_count >= kBatchCommandCount &&
      (!flush() || !begin_batch()))
    return false;
  const auto command = open_primary();
  if (!command) return false;
  auto& frame = frames_[current_frame_];
  const auto draw_slot = frame.command_count;
  record(command, current_frame_, draw_slot);
  ++frame.command_count;
  if (frame.command_count >= kBatchCommandCount) return flush();
  return true;
}

bool CommandQueue::submit_current(VkSemaphore wait_semaphore,
                                  VkPipelineStageFlags2 wait_stage,
                                  VkSemaphore signal_semaphore) {
  // Staged work (see set_pending_work_flush()) may open the batch.
  before_ordered_work();
  if (current_frame_ == UINT32_MAX) return true;
  // Every run recorded on a worker is finished before the batch is submitted.
  if (workers_) {
    workers_->wait_all();
    if (auto failure = workers_->take_error(); !failure.empty()) {
      error_ = std::move(failure);
      return false;
    }
  }
  auto& frame = frames_[current_frame_];
  if (!frame.recording || !frame.command_count) {
    // Nothing recorded but the start timestamp: nothing to submit.
    if (frame.primary) static_cast<void>(vkEndCommandBuffer(frame.primary));
    frame.primary = VK_NULL_HANDLE;
    frame.segments.clear();
    frame.recording = false;
    frame.command_count = 0;
    current_frame_ = UINT32_MAX;
    return true;
  }
  if (frame.timestamps) {
    if (const auto command = open_primary()) {
      vkCmdWriteTimestamp2(command, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, frame.timestamps, 1);
      frame.timestamps_pending = true;
    }
  }
  if (frame.primary) {
    if (vkEndCommandBuffer(frame.primary) != VK_SUCCESS) {
      error_ = "vkEndCommandBuffer failed";
      return false;
    }
    frame.primary = VK_NULL_HANDLE;
  }
  const xenon::gpu::perf::ScopedTime submit_time(xenon::gpu::perf::Counter::SubmitNs,
                                                 xenon::gpu::perf::Counter::Submits);
  const auto value = next_value_++;
  std::vector<VkCommandBufferSubmitInfo> command_infos(frame.segments.size());
  for (std::size_t i = 0; i < frame.segments.size(); ++i) {
    command_infos[i].sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    command_infos[i].commandBuffer = frame.segments[i];
  }

  VkSemaphoreSubmitInfo waits[1]{};
  std::uint32_t wait_count = 0;
  if (wait_semaphore) {
    waits[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waits[0].semaphore = wait_semaphore;
    waits[0].value = 0;
    waits[0].stageMask = wait_stage ? wait_stage
                                    : VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    wait_count = 1;
  }
  VkSemaphoreSubmitInfo signals[2]{};
  signals[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
  signals[0].semaphore = timeline_;
  signals[0].value = value;
  signals[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  std::uint32_t signal_count = 1;
  if (signal_semaphore) {
    signals[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signals[1].semaphore = signal_semaphore;
    signals[1].value = 0;
    signals[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signal_count = 2;
  }
  VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
  submit.waitSemaphoreInfoCount = wait_count;
  submit.pWaitSemaphoreInfos = wait_count ? waits : nullptr;
  submit.commandBufferInfoCount = static_cast<std::uint32_t>(command_infos.size());
  submit.pCommandBufferInfos = command_infos.data();
  submit.signalSemaphoreInfoCount = signal_count;
  submit.pSignalSemaphoreInfos = signals;
  if (vkQueueSubmit2(queue_, 1, &submit, VK_NULL_HANDLE) != VK_SUCCESS) {
    error_ = "vkQueueSubmit2 failed";
    return false;
  }
  frame.timeline_value = value;
  frame.recording = false;
  frame.command_count = 0;
  frame.segments.clear();
  last_submitted_value_ = value;
  next_frame_ = (current_frame_ + 1u) % kFrameCount;
  current_frame_ = UINT32_MAX;
  return true;
}

bool CommandQueue::flush() {
  return submit_current(VK_NULL_HANDLE, VK_PIPELINE_STAGE_2_NONE,
                        VK_NULL_HANDLE);
}

bool CommandQueue::execute_present(
    const std::function<void(VkCommandBuffer)>& record,
    VkSemaphore wait_semaphore, VkPipelineStageFlags2 wait_stage,
    VkSemaphore signal_semaphore) {
  if (!flush() || !begin_batch()) return false;
  const auto command = open_primary();
  if (!command) return false;
  record(command);
  frames_[current_frame_].command_count = 1;
  return submit_current(wait_semaphore, wait_stage, signal_semaphore);
}

bool CommandQueue::execute(const std::function<void(VkCommandBuffer)>& record) {
  if (!flush() || !begin_batch()) return false;
  const auto command = open_primary();
  if (!command) return false;
  record(command);
  frames_[current_frame_].command_count = 1;
  if (!flush()) return false;
  return wait_for_value(last_submitted_value_);
}

bool CommandQueue::wait_idle() {
  if (!flush()) return false;
  if (!device_ || !timeline_) return true;
  return wait_for_value(last_submitted_value_);
}

std::uint64_t CommandQueue::completed_value() const noexcept {
  std::uint64_t value = 0;
  if (device_ && timeline_)
    vkGetSemaphoreCounterValue(device_, timeline_, &value);
  return value;
}

}  // namespace xenon::gpu::vulkan
