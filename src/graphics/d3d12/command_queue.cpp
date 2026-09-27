#include "xenon/gpu/d3d12/command_queue.hpp"

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
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue_))) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence_)))) {
    error_ = "failed to create D3D12 submission objects";
    reset();
    return false;
  }
  for (auto& frame : frames_) {
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&frame.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         frame.allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&frame.list))) ||
        FAILED(frame.list->Close())) {
      error_ = "failed to create D3D12 frame command context";
      reset();
      return false;
    }
  }
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!event_) {
    error_ = "failed to create D3D12 fence event";
    reset();
    return false;
  }
  return true;
}

void CommandQueue::reset() noexcept {
  if (event_) CloseHandle(static_cast<HANDLE>(event_));
  event_ = nullptr;
  fence_.Reset();
  for (auto& frame : frames_) {
    frame.list.Reset();
    frame.allocator.Reset();
    frame.fence_value = 0;
    frame.command_count = 0;
    frame.recording = false;
  }
  queue_.Reset();
  next_frame_ = 0;
  current_frame_ = UINT32_MAX;
  next_fence_value_ = 1;
  last_submitted_value_ = 0;
}

bool CommandQueue::wait_for_value(std::uint64_t value) {
  if (!value) return true;
  if (!fence_ || !event_) {
    error_ = "D3D12 fence wait requested on an uninitialized queue";
    return false;
  }
  if (fence_->GetCompletedValue() >= value) return true;
  if (FAILED(fence_->SetEventOnCompletion(value, static_cast<HANDLE>(event_))) ||
      WaitForSingleObject(static_cast<HANDLE>(event_), INFINITE) !=
          WAIT_OBJECT_0) {
    error_ = "failed waiting for D3D12 fence";
    return false;
  }
  return true;
}

bool CommandQueue::begin_batch() {
  if (current_frame_ != UINT32_MAX) return true;
  if (!queue_ || !fence_) {
    error_ = "D3D12 command queue is not initialized";
    return false;
  }
  auto& frame = frames_[next_frame_];
  if (!wait_for_value(frame.fence_value)) return false;
  if (FAILED(frame.allocator->Reset()) ||
      FAILED(frame.list->Reset(frame.allocator.Get(), nullptr))) {
    error_ = "failed to reset D3D12 frame command recording";
    return false;
  }
  frame.command_count = 0;
  frame.recording = true;
  current_frame_ = next_frame_;
  return true;
}

bool CommandQueue::execute_async(const std::function<void(
    ID3D12GraphicsCommandList*, std::uint32_t, std::uint32_t)>& record) {
  if (!begin_batch()) return false;
  auto& frame = frames_[current_frame_];
  const auto draw_slot = frame.command_count;
  record(frame.list.Get(), current_frame_, draw_slot);
  ++frame.command_count;
  if (frame.command_count >= kBatchCommandCount) return flush();
  return true;
}

bool CommandQueue::flush() {
  if (current_frame_ == UINT32_MAX) return true;
  auto& frame = frames_[current_frame_];
  if (!frame.recording || !frame.command_count) {
    frame.recording = false;
    frame.command_count = 0;
    current_frame_ = UINT32_MAX;
    return true;
  }
  if (FAILED(frame.list->Close())) {
    error_ = "failed to close D3D12 command list";
    return false;
  }
  ID3D12CommandList* lists[]{frame.list.Get()};
  queue_->ExecuteCommandLists(1, lists);
  const auto value = next_fence_value_++;
  if (FAILED(queue_->Signal(fence_.Get(), value))) {
    error_ = "failed to signal D3D12 fence";
    return false;
  }
  frame.fence_value = value;
  frame.recording = false;
  frame.command_count = 0;
  last_submitted_value_ = value;
  next_frame_ = (current_frame_ + 1u) % kFrameCount;
  current_frame_ = UINT32_MAX;
  return true;
}

bool CommandQueue::execute(
    const std::function<void(ID3D12GraphicsCommandList*)>& record) {
  if (!flush() || !begin_batch()) return false;
  auto& frame = frames_[current_frame_];
  record(frame.list.Get());
  frame.command_count = 1;
  if (!flush()) return false;
  return wait_for_value(last_submitted_value_);
}

bool CommandQueue::wait_idle() {
  if (!flush()) return false;
  if (!queue_ || !fence_) return true;
  return wait_for_value(last_submitted_value_);
}

std::uint64_t CommandQueue::completed_value() const noexcept {
  return fence_ ? fence_->GetCompletedValue() : 0;
}

}  // namespace xenon::gpu::d3d12
