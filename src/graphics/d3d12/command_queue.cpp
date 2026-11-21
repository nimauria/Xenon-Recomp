#include "xenon/gpu/d3d12/command_queue.hpp"

#include <cstring>
#include <stdexcept>

#include "xenon/gpu/perf_stats.hpp"
#include "xenon/logging/activity_trace.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace xenon::gpu::d3d12 {

CommandQueue::~CommandQueue() {
  (void)wait_idle();
  reset();
}

bool CommandQueue::initialize(ID3D12Device* device) {
  reset();
  error_.clear();
  if (!device) {
    error_ = "D3D12 command queue requires a device";
    return false;
  }
  device_ = device;
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_))) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence_)))) {
    error_ = "failed to create D3D12 submission objects";
    reset();
    return false;
  }
  // Each frame starts with one list (its first primary segment); more are
  // created as batches need them and kept for later batches.
  for (auto& frame : frames_) {
    if (!acquire_list(frame)) {
      reset();
      return false;
    }
    frame.lists_used = 0;
  }
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  concurrent_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event_ || !concurrent_event_) {
    error_ = "failed to create D3D12 fence event";
    reset();
    return false;
  }
  workers_ = std::make_unique<RecordingWorkers>(RecordingWorkers::default_thread_count());
  // GPU timestamps: two per batch. A queue without a timestamp frequency
  // (none in practice on a direct queue) is simply not timed.
  if (SUCCEEDED(queue_->GetTimestampFrequency(&timestamp_frequency_)) && timestamp_frequency_) {
    for (auto& frame : frames_) {
      D3D12_QUERY_HEAP_DESC heap{};
      heap.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
      heap.Count = 2;
      const D3D12_HEAP_PROPERTIES readback_heap{D3D12_HEAP_TYPE_READBACK};
      D3D12_RESOURCE_DESC buffer{};
      buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
      buffer.Width = 2 * sizeof(std::uint64_t);
      buffer.Height = 1;
      buffer.DepthOrArraySize = 1;
      buffer.MipLevels = 1;
      buffer.SampleDesc.Count = 1;
      buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
      if (FAILED(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&frame.timestamps))) ||
          FAILED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&frame.timestamp_readback)))) {
        for (auto& clear : frames_) {
          clear.timestamps.Reset();
          clear.timestamp_readback.Reset();
        }
        timestamp_frequency_ = 0;
        break;
      }
    }
  }
  return true;
}

void CommandQueue::read_timestamps(FrameContext& frame) {
  if (!frame.timestamps_pending) return;
  frame.timestamps_pending = false;
  void* mapped = nullptr;
  const D3D12_RANGE range{0, 2 * sizeof(std::uint64_t)};
  if (FAILED(frame.timestamp_readback->Map(0, &range, &mapped))) return;
  std::uint64_t ticks[2]{};
  std::memcpy(ticks, mapped, sizeof(ticks));
  const D3D12_RANGE none{0, 0};
  frame.timestamp_readback->Unmap(0, &none);
  if (ticks[1] < ticks[0]) return;
  const auto to_ns = [this](std::uint64_t value) {
    return static_cast<std::uint64_t>(static_cast<double>(value) * 1e9 /
                                      static_cast<double>(timestamp_frequency_));
  };
  const auto busy = to_ns(ticks[1] - ticks[0]);
  perf::add(perf::Counter::GpuBusyNs, busy);
  perf::add(perf::Counter::GpuBatches, 1u);
  // Batches complete in submission order and are read in that order, so the
  // gap since the previous one's end is time the GPU had nothing to do.
  if (last_batch_end_ && ticks[0] > last_batch_end_)
    perf::add(perf::Counter::GpuIdleNs, to_ns(ticks[0] - last_batch_end_));
  last_batch_end_ = ticks[1];
  ++timed_batches_;
  timed_gpu_ns_ += busy;
}

void CommandQueue::collect_timestamps() {
  // Frames in submission order, oldest first.
  for (std::uint32_t i = 0; i < kFrameCount; ++i) {
    auto& frame = frames_[(next_frame_ + i) % kFrameCount];
    if (frame.timestamps_pending && fence_ && fence_->GetCompletedValue() >= frame.fence_value)
      read_timestamps(frame);
  }
}

void CommandQueue::reset() noexcept {
  workers_.reset();
  if (event_) CloseHandle(static_cast<HANDLE>(event_));
  event_ = nullptr;
  if (concurrent_event_) CloseHandle(static_cast<HANDLE>(concurrent_event_));
  concurrent_event_ = nullptr;
  fence_.Reset();
  for (auto& frame : frames_) {
    frame.lists.clear();
    frame.lists_used = 0;
    frame.segments.clear();
    frame.primary = nullptr;
    frame.fence_value = 0;
    frame.command_count = 0;
    frame.recording = false;
  }
  queue_.Reset();
  device_ = nullptr;
  next_frame_ = 0;
  current_frame_ = UINT32_MAX;
  next_fence_value_ = 1;
  last_submitted_value_ = 0;
}

bool CommandQueue::set_recording_threads(unsigned threads) {
  if (!flush()) return false;
  workers_ = std::make_unique<RecordingWorkers>(threads);
  return true;
}

bool CommandQueue::wait_for_value(std::uint64_t value) {
  if (!value) return true;
  if (!fence_ || !event_) {
    error_ = "D3D12 fence wait requested on an uninitialized queue";
    return false;
  }
  if (fence_->GetCompletedValue() >= value) return true;
  // Activity trace: how long the CPU waits on the GPU, and how far behind it
  // is (value - completed).
  static const auto activity_key = xenon::logging::intern_activity(
      {"gpu", "fence wait", {"value", "completed", "submitted", ""}, false});
  xenon::logging::ActivityScope activity(
      activity_key, {value, fence_->GetCompletedValue(), last_submitted_value_, 0u});
  const xenon::gpu::perf::ScopedTime wait_time(xenon::gpu::perf::Counter::FenceWaitNs, xenon::gpu::perf::Counter::FenceWaits);
  if (FAILED(fence_->SetEventOnCompletion(value, static_cast<HANDLE>(event_))) ||
      WaitForSingleObject(static_cast<HANDLE>(event_), INFINITE) !=
          WAIT_OBJECT_0) {
    error_ = "failed waiting for D3D12 fence";
    return false;
  }
  return true;
}

bool CommandQueue::wait_completed_concurrent(std::uint64_t value,
                                             std::uint32_t timeout_ms) const noexcept {
  if (!value) return true;
  if (!fence_ || !concurrent_event_) return false;
  // ID3D12Fence is free-threaded; the event is this waiter's own.
  if (fence_->GetCompletedValue() >= value) return true;
  if (FAILED(fence_->SetEventOnCompletion(value, static_cast<HANDLE>(concurrent_event_))))
    return false;
  // A timed-out registration may still signal later; the auto-reset event
  // then only wakes a later wait early, which rechecks the value.
  (void)WaitForSingleObject(static_cast<HANDLE>(concurrent_event_), timeout_ms);
  return fence_->GetCompletedValue() >= value;
}

ID3D12GraphicsCommandList* CommandQueue::acquire_list(FrameContext& frame) {
  if (frame.lists_used == frame.lists.size()) {
    ListSlot slot{};
    if (!device_ ||
        FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&slot.allocator))) ||
        FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                          slot.allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&slot.list))) ||
        FAILED(slot.list->Close())) {
      error_ = "failed to create a D3D12 command list for a submission segment";
      return nullptr;
    }
    frame.lists.push_back(std::move(slot));
  }
  auto& slot = frame.lists[frame.lists_used];
  // The allocator was reset when the batch began (its last use completed).
  if (frame.recording && FAILED(slot.list->Reset(slot.allocator.Get(), nullptr))) {
    error_ = "failed to reset a D3D12 segment command list";
    return nullptr;
  }
  ++frame.lists_used;
  return slot.list.Get();
}

bool CommandQueue::begin_batch() {
  if (current_frame_ != UINT32_MAX) return true;
  if (!queue_ || !fence_) {
    error_ = "D3D12 command queue is not initialized";
    return false;
  }
  auto& frame = frames_[next_frame_];
  if (!wait_for_value(frame.fence_value)) return false;
  for (std::uint32_t i = 0; i < frame.lists.size(); ++i) {
    if (FAILED(frame.lists[i].allocator->Reset())) {
      error_ = "failed to reset D3D12 frame command allocators";
      return false;
    }
  }
  frame.lists_used = 0;
  frame.segments.clear();
  frame.primary = nullptr;
  frame.command_count = 0;
  frame.recording = true;
  current_frame_ = next_frame_;
  ++batch_serial_;
  // The frame's previous batch has completed (waited for above): read its
  // timestamps before this batch overwrites them, then start timing this one.
  read_timestamps(frame);
  if (frame.timestamps) {
    if (auto* list = open_primary()) list->EndQuery(frame.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
  }
  return true;
}

ID3D12GraphicsCommandList* CommandQueue::open_primary() {
  auto& frame = frames_[current_frame_];
  if (frame.primary) return frame.primary;
  auto* list = acquire_list(frame);
  if (!list) return nullptr;
  frame.primary = list;
  frame.segments.push_back(list);
  return list;
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

bool CommandQueue::execute_async(const std::function<void(
    ID3D12GraphicsCommandList*, std::uint32_t, std::uint32_t)>& record) {
  if (!begin_batch()) return false;
  before_ordered_work();
  if (frames_[current_frame_].command_count >= kBatchCommandCount &&
      (!flush() || !begin_batch()))
    return false;
  auto* list = open_primary();
  if (!list) return false;
  auto& frame = frames_[current_frame_];
  const auto draw_slot = frame.command_count;
  record(list, current_frame_, draw_slot);
  ++frame.command_count;
  if (frame.command_count >= kBatchCommandCount) return flush();
  return true;
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

bool CommandQueue::record_run(std::function<void(ID3D12GraphicsCommandList*)> record,
                              bool parallel) {
  if (current_frame_ == UINT32_MAX) {
    error_ = "D3D12 draw run recorded with no open batch";
    return false;
  }
  auto& frame = frames_[current_frame_];
  if (!parallel || !workers_ || !workers_->thread_count()) {
    auto* list = open_primary();
    if (!list) return false;
    record(list);
    return true;
  }
  // The run becomes a segment of its own, after the primary work before it;
  // ordered work after it goes to a new primary segment.
  if (frame.primary) {
    if (FAILED(frame.primary->Close())) {
      error_ = "failed to close a D3D12 primary segment";
      return false;
    }
    frame.primary = nullptr;
  }
  auto* list = acquire_list(frame);
  if (!list) return false;
  frame.segments.push_back(list);
  ++parallel_runs_;
  workers_->submit([record = std::move(record), list] {
    record(list);
    if (FAILED(list->Close())) throw std::runtime_error("failed to close a D3D12 draw-run command list");
  });
  return true;
}

bool CommandQueue::flush() {
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
    if (frame.primary) static_cast<void>(frame.primary->Close());
    frame.primary = nullptr;
    frame.segments.clear();
    frame.recording = false;
    frame.command_count = 0;
    current_frame_ = UINT32_MAX;
    return true;
  }
  if (frame.timestamps) {
    if (auto* list = open_primary()) {
      list->EndQuery(frame.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
      list->ResolveQueryData(frame.timestamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2,
                             frame.timestamp_readback.Get(), 0);
      frame.timestamps_pending = true;
    }
  }
  if (frame.primary) {
    if (FAILED(frame.primary->Close())) {
      error_ = "failed to close D3D12 command list";
      return false;
    }
    frame.primary = nullptr;
  }
  const xenon::gpu::perf::ScopedTime submit_time(xenon::gpu::perf::Counter::SubmitNs,
                                                 xenon::gpu::perf::Counter::Submits);
  queue_->ExecuteCommandLists(static_cast<UINT>(frame.segments.size()), frame.segments.data());
  const auto value = next_fence_value_++;
  if (FAILED(queue_->Signal(fence_.Get(), value))) {
    error_ = "failed to signal D3D12 fence";
    return false;
  }
  frame.fence_value = value;
  frame.recording = false;
  frame.command_count = 0;
  frame.segments.clear();
  last_submitted_value_ = value;
  next_frame_ = (current_frame_ + 1u) % kFrameCount;
  current_frame_ = UINT32_MAX;
  return true;
}

bool CommandQueue::execute(
    const std::function<void(ID3D12GraphicsCommandList*)>& record) {
  if (!flush() || !begin_batch()) return false;
  auto* list = open_primary();
  if (!list) return false;
  record(list);
  frames_[current_frame_].command_count = 1;
  if (!flush()) return false;
  return wait_for_value(last_submitted_value_);
}

bool CommandQueue::wait_idle() {
  if (!flush()) return false;
  if (!queue_ || !fence_) return true;
  // Work reaches this queue without passing through flush(): DXGI's Present()
  // queues the presentation of a back buffer on it. Waiting only for Xenon's
  // last submission let resize and teardown release back buffers and the
  // queue while a present was still in flight (debug-layer CORRUPTION #921).
  // A new signal covers everything queued so far.
  const auto value = next_fence_value_++;
  if (FAILED(queue_->Signal(fence_.Get(), value))) {
    error_ = "failed to signal D3D12 fence";
    return false;
  }
  last_submitted_value_ = value;
  return wait_for_value(value);
}

std::uint64_t CommandQueue::completed_value() const noexcept {
  return fence_ ? fence_->GetCompletedValue() : 0;
}

}  // namespace xenon::gpu::d3d12
