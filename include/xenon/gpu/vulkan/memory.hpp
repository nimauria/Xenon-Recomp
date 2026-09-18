#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <vulkan/vulkan.h>

namespace xenon::gpu::vulkan {

class Buffer {
 public:
  Buffer() = default;
  ~Buffer();
  Buffer(const Buffer&) = delete;
  Buffer& operator=(const Buffer&) = delete;
  Buffer(Buffer&& other) noexcept;
  Buffer& operator=(Buffer&& other) noexcept;
  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device,
                                VkDevice device, VkDeviceSize size,
                                VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags memory_properties);
  void reset() noexcept;
  [[nodiscard]] bool map(std::span<std::byte>& bytes);
  void unmap() noexcept;
  [[nodiscard]] VkBuffer buffer() const noexcept { return buffer_; }
  [[nodiscard]] VkDeviceSize size() const noexcept { return size_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkBuffer buffer_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkDeviceSize size_{};
  std::byte* mapped_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
