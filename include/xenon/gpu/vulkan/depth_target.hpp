#pragma once

#include <cstdint>
#include <string>

#include <vulkan/vulkan.h>

#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"

namespace xenon::gpu::vulkan {

[[nodiscard]] VkFormat depth_render_target_format(
    DepthRenderTargetFormat format) noexcept;

class DepthTargetImage {
 public:
  DepthTargetImage() = default;
  ~DepthTargetImage();
  DepthTargetImage(const DepthTargetImage&) = delete;
  DepthTargetImage& operator=(const DepthTargetImage&) = delete;

  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device,
                                VkDevice device,
                                const EdramSurfaceLayout& layout,
                                DepthRenderTargetFormat format);
  [[nodiscard]] bool clear(CommandQueue& queue, float depth,
                           std::uint8_t stencil);
  void transition_to_depth_attachment(VkCommandBuffer command) noexcept;
  void reset() noexcept;
  [[nodiscard]] VkImage image() const noexcept { return image_; }
  [[nodiscard]] VkImageView view() const noexcept { return view_; }
  [[nodiscard]] VkFormat format() const noexcept { return format_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
  [[nodiscard]] const EdramSurfaceLayout& surface() const noexcept {
    return surface_;
  }
  [[nodiscard]] bool requires_float24_conversion() const noexcept {
    return float24_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkImageView view_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  VkImageLayout layout_{VK_IMAGE_LAYOUT_UNDEFINED};
  std::uint32_t width_{};
  std::uint32_t height_{};
  EdramSurfaceLayout surface_{};
  bool float24_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
