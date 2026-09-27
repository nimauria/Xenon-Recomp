#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include <vulkan/vulkan.h>

namespace xenon::gpu::vulkan {

class CommandQueue {
 public:
  static constexpr std::uint32_t kFrameCount = 3;
  static constexpr std::uint32_t kBatchCommandCount = 8;

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
  [[nodiscard]] bool flush();
  [[nodiscard]] bool wait_idle();
  [[nodiscard]] bool wait_for_completion(std::uint64_t value) {
    return wait_for_value(value);
  }
  [[nodiscard]] VkQueue native_queue() const noexcept { return queue_; }
  [[nodiscard]] std::uint64_t completed_value() const noexcept;
  [[nodiscard]] std::uint64_t last_submitted_value() const noexcept {
    return last_submitted_value_;
  }
  // Fence/timeline value that protects commands already recorded in the open
  // batch, or the last submitted value when no batch is open.
  [[nodiscard]] std::uint64_t retirement_value() const noexcept {
    return current_frame_ != UINT32_MAX ? next_value_ : last_submitted_value_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  struct FrameContext {
    VkCommandPool pool{VK_NULL_HANDLE};
    VkCommandBuffer command{VK_NULL_HANDLE};
    std::uint64_t timeline_value{};
    std::uint32_t command_count{};
    bool recording{};
  };

  [[nodiscard]] bool wait_for_value(std::uint64_t value);
  [[nodiscard]] bool begin_batch();
  [[nodiscard]] bool submit_current(VkSemaphore wait_semaphore,
                                    VkPipelineStageFlags2 wait_stage,
                                    VkSemaphore signal_semaphore);

  VkDevice device_{VK_NULL_HANDLE};
  VkQueue queue_{VK_NULL_HANDLE};
  std::array<FrameContext, kFrameCount> frames_{};
  VkSemaphore timeline_{VK_NULL_HANDLE};
  std::uint32_t next_frame_{};
  std::uint32_t current_frame_{UINT32_MAX};
  std::uint64_t next_value_{1};
  std::uint64_t last_submitted_value_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
