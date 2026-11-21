#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <d3d12.h>
#include <wrl/client.h>

#include "xenon/gpu/recording_workers.hpp"

namespace xenon::gpu::d3d12 {

// The GPU submission coordinator for one D3D12 direct queue.
//
// Work is recorded into batches. A batch is a sequence of command-list
// segments submitted together, in order, with one ExecuteCommandLists():
// primary segments record ordered work as it arrives (transfers, resolves,
// uploads, barriers - execute_async()), and run segments hold runs of
// prepared draws, recorded on worker threads (record_run()) while the
// backend prepares what follows. Every segment has its own allocator and
// list, from the frame's pool, reset only once the GPU has finished the
// frame. Ordered work first closes the backend's open draw run
// (set_before_ordered_work()), so everything reaches the GPU in the order the
// guest issued it.
class CommandQueue {
 public:
  static constexpr std::uint32_t kFrameCount = 3;
  // Draws and ordered work a batch holds before it is submitted. A draw's
  // descriptor tables are allocated only when they change
  // (ResourceLayout::prepare_draw), so this is no longer tied to the 2048-entry
  // shader-visible sampler heap: 64 draws a batch meant 16 submissions for a
  // 1000-draw frame, about a quarter of a draw's host time in draw-bench.
  static constexpr std::uint32_t kBatchCommandCount = 1024;

  CommandQueue() = default;
  ~CommandQueue();
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

  [[nodiscard]] bool initialize(ID3D12Device* device);
  void reset() noexcept;

  // Synchronous helper retained for transfers/readbacks whose CPU-visible
  // result is consumed immediately after this call returns. Pending draw work
  // is submitted before the synchronous command is recorded.
  [[nodiscard]] bool execute(
      const std::function<void(ID3D12GraphicsCommandList*)>& record);

  // Appends ordered work to the open batch's primary segment. The callback
  // gets the rotating frame index and a draw slot whose descriptor/constants
  // snapshot remains immutable until that frame is recycled.
  [[nodiscard]] bool execute_async(const std::function<void(
      ID3D12GraphicsCommandList*, std::uint32_t, std::uint32_t)>& record);

  // Reserves the next draw slot of the open batch (flushing a full batch
  // first) for a draw prepared now and recorded later through record_run().
  // Identifies the open batch: every begin_batch() that opens one makes a new
  // value. Descriptor tables written in one batch are reused only within it.
  [[nodiscard]] std::uint64_t batch_serial() const noexcept { return batch_serial_; }
  [[nodiscard]] bool reserve_slot(std::uint32_t& frame_index, std::uint32_t& slot);
  // Records a run of prepared draws in order with everything before it: on
  // the calling thread into the primary segment, or (`parallel`, with
  // workers) on a worker into a segment of its own. Called only from the
  // before-ordered-work hook, which owns the open run.
  [[nodiscard]] bool record_run(std::function<void(ID3D12GraphicsCommandList*)> record,
                                bool parallel);
  // The backend's "close the open draw run" step, run before any ordered work
  // and before a batch is submitted.
  void set_before_ordered_work(std::function<void()> hook) { before_ordered_work_ = std::move(hook); }
  // Work a component staged for the queue and has not recorded yet (the
  // guest-memory mirror's batched uploads): run after the hook above, before
  // every ordered command and submission, so staged work keeps its place in
  // the order. Runs inside the same re-entrancy guard.
  void set_pending_work_flush(std::function<void()> flush) { pending_work_flush_ = std::move(flush); }
  // Recording worker threads (RecordingWorkers::default_thread_count() by
  // default; 0 records every run inline). Submits pending work first.
  [[nodiscard]] bool set_recording_threads(unsigned threads);
  [[nodiscard]] unsigned recording_threads() const noexcept {
    return workers_ ? workers_->thread_count() : 0u;
  }

  // Submits a partially filled asynchronous batch without waiting.
  [[nodiscard]] bool flush();
  [[nodiscard]] bool wait_idle();
  [[nodiscard]] bool wait_for_completion(std::uint64_t value) {
    return wait_for_value(value);
  }
  [[nodiscard]] ID3D12CommandQueue* native_queue() const noexcept {
    return queue_.Get();
  }
  [[nodiscard]] std::uint64_t completed_value() const noexcept;
  // For one waiter on another thread (the readback publisher) while this
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
    return current_frame_ != UINT32_MAX ? next_fence_value_ : last_submitted_value_;
  }
  // Frame slot of the open asynchronous batch, or UINT32_MAX when none is open.
  [[nodiscard]] std::uint32_t open_frame() const noexcept { return current_frame_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  // Run segments recorded on workers since initialization (tests, diagnostics).
  [[nodiscard]] std::uint64_t parallel_runs() const noexcept { return parallel_runs_; }
  // GPU timestamps around every submitted batch (perf_frame.log "gpu"):
  // batches measured, and the GPU time they took, since initialization.
  [[nodiscard]] std::uint64_t timed_batches() const noexcept { return timed_batches_; }
  [[nodiscard]] std::uint64_t timed_gpu_ns() const noexcept { return timed_gpu_ns_; }
  // Reads the timestamps of every completed batch not read yet.
  void collect_timestamps();

 private:
  struct ListSlot {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list{};
  };
  struct FrameContext {
    std::vector<ListSlot> lists{};
    std::uint32_t lists_used{};
    // The batch's segments, in submission order.
    std::vector<ID3D12CommandList*> segments{};
    // The primary segment open for ordered work, if any.
    ID3D12GraphicsCommandList* primary{};
    // Start and end timestamps of the frame's last batch, resolved into
    // timestamp_readback; read once the batch has completed.
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> timestamps{};
    Microsoft::WRL::ComPtr<ID3D12Resource> timestamp_readback{};
    bool timestamps_pending{};
    std::uint64_t fence_value{};
    std::uint32_t command_count{};
    bool recording{};
  };

  [[nodiscard]] bool wait_for_value(std::uint64_t value);
  [[nodiscard]] bool begin_batch();
  [[nodiscard]] ID3D12GraphicsCommandList* acquire_list(FrameContext& frame);
  [[nodiscard]] ID3D12GraphicsCommandList* open_primary();
  void before_ordered_work();

  ID3D12Device* device_{};
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_{};
  std::array<FrameContext, kFrameCount> frames_{};
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_{};
  void* event_{};
  // Owned by the one concurrent waiter (wait_completed_concurrent()).
  void* concurrent_event_{};
  std::uint32_t next_frame_{};
  std::uint64_t batch_serial_{};
  std::uint32_t current_frame_{UINT32_MAX};
  std::uint64_t next_fence_value_{1};
  std::uint64_t last_submitted_value_{};
  std::function<void()> before_ordered_work_{};
  std::function<void()> pending_work_flush_{};
  bool in_before_ordered_work_{};
  std::unique_ptr<RecordingWorkers> workers_{};
  std::uint64_t parallel_runs_{};
  void read_timestamps(FrameContext& frame);
  std::uint64_t timestamp_frequency_{};
  std::uint64_t last_batch_end_{};
  std::uint64_t timed_batches_{};
  std::uint64_t timed_gpu_ns_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12
