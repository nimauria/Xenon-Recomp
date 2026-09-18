#pragma once

#include <string>

#include <vulkan/vulkan.h>

#include "xenon/gpu/texture.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"

namespace xenon::gpu::vulkan {

[[nodiscard]] VkFormat host_texture_format(TextureHostFormat format) noexcept;

class TextureImage {
 public:
  ~TextureImage();
  TextureImage() = default;
  TextureImage(const TextureImage&) = delete;
  TextureImage& operator=(const TextureImage&) = delete;

  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device, VkDevice device,
                                CommandQueue& queue,
                                const DecodedTexture& texture);
  void reset() noexcept;
  [[nodiscard]] VkImage image() const noexcept { return image_; }
  [[nodiscard]] VkImageView view() const noexcept { return view_; }
  [[nodiscard]] VkSampler sampler() const noexcept { return sampler_; }
  [[nodiscard]] VkFormat format() const noexcept { return format_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkImageView view_{VK_NULL_HANDLE};
  VkSampler sampler_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
