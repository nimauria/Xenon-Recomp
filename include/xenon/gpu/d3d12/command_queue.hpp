#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include <d3d12.h>
#include <wrl/client.h>

namespace xenon::gpu::d3d12 {

class CommandQueue {
 public:
  static constexpr std::uint32_t kFrameCount = 3;
  static constexpr std::uint32_t kBatchCommandCount = 8;

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

  // Appends work to the current native command-list batch. The callback gets
  // both the rotating frame index and a draw slot whose descriptor/constants
  // snapshot remains immutable until that frame is recycled.
  [[nodiscard]] bool execute_async(const std::function<void(
      ID3D12GraphicsCommandList*, std::uint32_t, std::uint32_t)>& record);

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
  [[nodiscard]] std::uint64_t last_submitted_value() const noexcept {
    return last_submitted_value_;
  }
  // Fence/timeline value that protects commands already recorded in the open
  // batch, or the last submitted value when no batch is open.
  [[nodiscard]] std::uint64_t retirement_value() const noexcept {
    return current_frame_ != UINT32_MAX ? next_fence_value_ : last_submitted_value_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  struct FrameContext {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator{};
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list{};
    std::uint64_t fence_value{};
    std::uint32_t command_count{};
    bool recording{};
  };

  [[nodiscard]] bool wait_for_value(std::uint64_t value);
  [[nodiscard]] bool begin_batch();

  Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_{};
  std::array<FrameContext, kFrameCount> frames_{};
  Microsoft::WRL::ComPtr<ID3D12Fence> fence_{};
  void* event_{};
  std::uint32_t next_frame_{};
  std::uint32_t current_frame_{UINT32_MAX};
  std::uint64_t next_fence_value_{1};
  std::uint64_t last_submitted_value_{};
  std::string error_{};
};

}  // namespace xenon::gpu::d3d12
