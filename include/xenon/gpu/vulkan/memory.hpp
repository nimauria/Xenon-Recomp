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
  // `memory_properties` are required; `preferred` ones are taken when a
  // memory type has them too. Buffers the CPU reads back prefer
  // kCpuReadback: host-visible memory that is not cached reads at a small
  // fraction of normal speed (a 4 MiB resolve readback took ~40 ms).
  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device,
                                VkDevice device, VkDeviceSize size,
                                VkBufferUsageFlags usage,
                                VkMemoryPropertyFlags memory_properties,
                                VkMemoryPropertyFlags preferred = 0);
  static constexpr VkMemoryPropertyFlags kCpuReadback = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
  // The property flags of the memory type the buffer was allocated from.
  [[nodiscard]] VkMemoryPropertyFlags memory_properties() const noexcept {
    return memory_properties_;
  }
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
  VkMemoryPropertyFlags memory_properties_{};
  std::byte* mapped_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
