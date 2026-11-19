#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "xenon/gpu/recording_workers.hpp"

namespace xenon::gpu::vulkan {

// The GPU submission coordinator for one Vulkan queue (see the D3D12
// CommandQueue, whose model this shares): a batch is a sequence of primary
// command buffers, submitted together in order with one vkQueueSubmit2 -
// primary segments for ordered work, run segments for runs of prepared draws
// recorded on worker threads. Every segment has its own command pool, reset
// once the GPU has finished the frame. Ordered work first closes the
// backend's open draw run.
class CommandQueue {
 public:
  static constexpr std::uint32_t kFrameCount = 3;
  // Draws and ordered work a batch holds before it is submitted: each slot has
  // its own descriptor set and constants (ResourceLayout), and 64 a batch
  // meant 16 submissions for a 1000-draw frame (draw-bench).
  static constexpr std::uint32_t kBatchCommandCount = 1024;

  CommandQueue() = default;
  ~CommandQueue();
  [[nodiscard]] bool initialize(VkDevice device, VkQueue queue,
                                std::uint32_t queue_family);
  void reset() noexcept;

  [[nodiscard]] bool execute(const std::function<void(VkCommandBuffer)>& record);
  [[nodiscard]] bool execute_async(const std::function<void(
      VkCommandBuffer, std::uint32_t, std::uint32_t)>& record);
  // Presentation submit: flushes ordinary draw work first, then submits one
  // command buffer waiting on / signaling binary WSI semaphores while still
  // advancing the queue timeline used for resource retirement.
  [[nodiscard]] bool execute_present(
      const std::function<void(VkCommandBuffer)>& record,
      VkSemaphore wait_semaphore, VkPipelineStageFlags2 wait_stage,
      VkSemaphore signal_semaphore);
  // Reserves the next draw slot of the open batch (flushing a full batch
  // first) for a draw prepared now and recorded later through record_run().
  [[nodiscard]] bool reserve_slot(std::uint32_t& frame_index, std::uint32_t& slot);
  // Records a run of prepared draws in order with everything before it: into
  // the primary segment, or (`parallel`, with workers) on a worker into a
  // segment of its own. Called only from the before-ordered-work hook.
  [[nodiscard]] bool record_run(std::function<void(VkCommandBuffer)> record, bool parallel);
  void set_before_ordered_work(std::function<void()> hook) { before_ordered_work_ = std::move(hook); }
  [[nodiscard]] bool set_recording_threads(unsigned threads);
  [[nodiscard]] unsigned recording_threads() const noexcept {
    return workers_ ? workers_->thread_count() : 0u;
  }
  [[nodiscard]] std::uint64_t parallel_runs() const noexcept { return parallel_runs_; }
  // GPU timestamps around every submitted batch (perf_frame.log "gpu"), when
  // the queue family supports them; false when it does not.
  bool enable_timestamps(VkPhysicalDevice physical_device);
  [[nodiscard]] std::uint64_t timed_batches() const noexcept { return timed_batches_; }
  [[nodiscard]] std::uint64_t timed_gpu_ns() const noexcept { return timed_gpu_ns_; }
  // Reads the timestamps of every completed batch not read yet.
  void collect_timestamps();
  [[nodiscard]] bool flush();
  [[nodiscard]] bool wait_idle();
  [[nodiscard]] bool wait_for_completion(std::uint64_t value) {
    return wait_for_value(value);
  }
  [[nodiscard]] VkQueue native_queue() const noexcept { return queue_; }
  [[nodiscard]] std::uint64_t completed_value() const noexcept;
  // For a waiter on another thread (the readback publisher) while this
  // queue's owner keeps recording and submitting: true once `value` has
  // completed, false after about `timeout_ms` when it has not.
  [[nodiscard]] bool wait_completed_concurrent(std::uint64_t value,
                                               std::uint32_t timeout_ms) const noexcept;
  [[nodiscard]] std::uint64_t last_submitted_value() const noexcept {
    return last_submitted_value_;
  }
  // Fence/timeline value that protects commands already recorded in the open
  // batch, or the last submitted value when no batch is open.
  [[nodiscard]] std::uint64_t retirement_value() const noexcept {
    return current_frame_ != UINT32_MAX ? next_value_ : last_submitted_value_;
  }
  // Frame slot of the open asynchronous batch, or UINT32_MAX when none is open.
  [[nodiscard]] std::uint32_t open_frame() const noexcept { return current_frame_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  struct Segment {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer command{VK_NULL_HANDLE};
  };
  struct FrameContext {
    std::vector<Segment> segments_pool{};
    std::uint32_t segments_used{};
    // The batch's command buffers, in submission order.
    std::vector<VkCommandBuffer> segments{};
    // The primary segment open for ordered work, if any.
    VkCommandBuffer primary{VK_NULL_HANDLE};
    // Start and end timestamps of the frame's last batch.
    VkQueryPool timestamps{VK_NULL_HANDLE};
    bool timestamps_pending{};
    std::uint64_t timeline_value{};
    std::uint32_t command_count{};
    bool recording{};
  };
  [[nodiscard]] VkCommandBuffer acquire_segment(FrameContext& frame);
  [[nodiscard]] VkCommandBuffer open_primary();
  void before_ordered_work();

  [[nodiscard]] bool wait_for_value(std::uint64_t value);
  [[nodiscard]] bool begin_batch();
  [[nodiscard]] bool submit_current(VkSemaphore wait_semaphore,
                                    VkPipelineStageFlags2 wait_stage,
                                    VkSemaphore signal_semaphore);

  VkDevice device_{VK_NULL_HANDLE};
  VkQueue queue_{VK_NULL_HANDLE};
  std::uint32_t queue_family_{};
  std::function<void()> before_ordered_work_{};
  bool in_before_ordered_work_{};
  std::unique_ptr<RecordingWorkers> workers_{};
  std::uint64_t parallel_runs_{};
  void read_timestamps(FrameContext& frame);
  double timestamp_period_ns_{};
  std::uint64_t timestamp_mask_{};
  std::uint64_t last_batch_end_{};
  std::uint64_t timed_batches_{};
  std::uint64_t timed_gpu_ns_{};
  std::array<FrameContext, kFrameCount> frames_{};
  VkSemaphore timeline_{VK_NULL_HANDLE};
  std::uint32_t next_frame_{};
  std::uint32_t current_frame_{UINT32_MAX};
  std::uint64_t next_value_{1};
  std::uint64_t last_submitted_value_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
