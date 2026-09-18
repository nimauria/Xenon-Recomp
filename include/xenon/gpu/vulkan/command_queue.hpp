#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <vulkan/vulkan.h>

namespace xenon::gpu::vulkan {

class CommandQueue {
 public:
  CommandQueue() = default;
  ~CommandQueue();
  [[nodiscard]] bool initialize(VkDevice device, VkQueue queue,
                                std::uint32_t queue_family);
  void reset() noexcept;
  [[nodiscard]] bool execute(const std::function<void(VkCommandBuffer)>& record);
  [[nodiscard]] std::uint64_t completed_value() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkQueue queue_{VK_NULL_HANDLE};
  VkCommandPool pool_{VK_NULL_HANDLE};
  VkSemaphore timeline_{VK_NULL_HANDLE};
  std::uint64_t next_value_{1};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
