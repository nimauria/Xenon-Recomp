#include "xenon/gpu/vulkan/memory.hpp"

namespace xenon::gpu::vulkan {

Buffer::~Buffer() { reset(); }

bool Buffer::initialize(VkPhysicalDevice physical_device, VkDevice device,
                        VkDeviceSize size, VkBufferUsageFlags usage,
                        VkMemoryPropertyFlags properties) {
  reset();
  error_.clear();
  if (!physical_device || !device || !size) {
    error_ = "Vulkan buffer requires devices and non-zero size";
    return false;
  }
  device_ = device;
  size_ = size;
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &info, nullptr, &buffer_) != VK_SUCCESS) {
    error_ = "vkCreateBuffer failed";
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetBufferMemoryRequirements(device_, buffer_, &requirements);
  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
  std::uint32_t type = UINT32_MAX;
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((requirements.memoryTypeBits & (1u << i)) &&
        (memory.memoryTypes[i].propertyFlags & properties) == properties) {
      type = i;
      break;
    }
  }
  if (type == UINT32_MAX) {
    error_ = "no Vulkan memory type satisfies the buffer requirements";
    reset();
    return false;
  }
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = requirements.size;
  allocate.memoryTypeIndex = type;
  if (vkAllocateMemory(device_, &allocate, nullptr, &memory_) != VK_SUCCESS ||
      vkBindBufferMemory(device_, buffer_, memory_, 0) != VK_SUCCESS) {
    error_ = "Vulkan buffer memory allocation or binding failed";
    reset();
    return false;
  }
  return true;
}

void Buffer::reset() noexcept {
  unmap();
  if (device_ && buffer_) vkDestroyBuffer(device_, buffer_, nullptr);
  if (device_ && memory_) vkFreeMemory(device_, memory_, nullptr);
  buffer_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  size_ = 0;
}

bool Buffer::map(std::span<std::byte>& bytes) {
  if (!device_ || !memory_) {
    error_ = "Vulkan buffer is not initialized";
    return false;
  }
  if (!mapped_) {
    void* mapped = nullptr;
    if (vkMapMemory(device_, memory_, 0, size_, 0, &mapped) != VK_SUCCESS) {
      error_ = "vkMapMemory failed";
      return false;
    }
    mapped_ = static_cast<std::byte*>(mapped);
  }
  bytes = {mapped_, static_cast<std::size_t>(size_)};
  return true;
}

void Buffer::unmap() noexcept {
  if (device_ && mapped_) vkUnmapMemory(device_, memory_);
  mapped_ = nullptr;
}

}  // namespace xenon::gpu::vulkan
