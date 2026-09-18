#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <d3d12.h>
#include <wrl/client.h>

namespace xenon::gpu::d3d12 {

class CommandQueue {
 public:
  CommandQueue() = default;
  ~CommandQueue();
  CommandQueue(const CommandQueue&) = delete;
  CommandQueue& operator=(const CommandQueue&) = delete;

  [[nodiscard]] bool initialize(ID3D12Device* device);
  void reset() noexcept;
  [[nodiscard]] bool execute(
      const std::function<void(ID3D12GraphicsCommandList*)>& record);
  [[nodiscard]] std::uint64_t completed_value() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_{};
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_{};
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list_{};
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_{};
  void* event_{};
  std::uint64_t next_fence_value_{1};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12
