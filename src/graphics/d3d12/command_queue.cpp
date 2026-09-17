#include "xenon/gpu/d3d12/command_queue.hpp"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace xenon::gpu::d3d12 {

CommandQueue::~CommandQueue() { reset(); }

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
      FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&allocator_))) ||
      FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                       allocator_.Get(), nullptr,
                                       IID_PPV_ARGS(&list_))) ||
      FAILED(list_->Close()) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&fence_)))) {
    error_ = "failed to create D3D12 submission objects";
    reset();
    return false;
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
  list_.Reset();
  allocator_.Reset();
  queue_.Reset();
  next_fence_value_ = 1;
}

bool CommandQueue::execute(
    const std::function<void(ID3D12GraphicsCommandList*)>& record) {
  if (!queue_ || FAILED(allocator_->Reset()) ||
      FAILED(list_->Reset(allocator_.Get(), nullptr))) {
    error_ = "failed to reset D3D12 command recording";
    return false;
  }
  record(list_.Get());
  if (FAILED(list_->Close())) {
    error_ = "failed to close D3D12 command list";
    return false;
  }
  ID3D12CommandList* lists[]{list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  const auto value = next_fence_value_++;
  if (FAILED(queue_->Signal(fence_.Get(), value))) {
    error_ = "failed to signal D3D12 fence";
    return false;
  }
  if (fence_->GetCompletedValue() < value) {
    if (FAILED(fence_->SetEventOnCompletion(value,
                                            static_cast<HANDLE>(event_))) ||
        WaitForSingleObject(static_cast<HANDLE>(event_), INFINITE) !=
            WAIT_OBJECT_0) {
      error_ = "failed waiting for D3D12 fence";
      return false;
    }
  }
  return true;
}

std::uint64_t CommandQueue::completed_value() const noexcept {
  return fence_ ? fence_->GetCompletedValue() : 0;
}

}  // namespace xenon::gpu::d3d12
