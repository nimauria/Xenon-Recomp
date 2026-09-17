#pragma once

#include <string>

#include <vulkan/vulkan.h>

#include "xenon/gpu/edram_surface.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"

namespace xenon::gpu::vulkan {

[[nodiscard]] VkFormat color_render_target_format(
    ColorRenderTargetFormat format) noexcept;

class RenderTargetImage {
 public:
  RenderTargetImage() = default;
  ~RenderTargetImage();
  RenderTargetImage(const RenderTargetImage&) = delete;
  RenderTargetImage& operator=(const RenderTargetImage&) = delete;

  [[nodiscard]] bool initialize(VkPhysicalDevice physical_device,
                                VkDevice device, CommandQueue& queue,
                                const EdramSurfaceLayout& layout,
                                ColorRenderTargetFormat format);
  [[nodiscard]] bool clear(CommandQueue& queue,
                           const VkClearColorValue& value);
  void reset() noexcept;
  [[nodiscard]] VkImage image() const noexcept { return image_; }
  [[nodiscard]] VkImageView view() const noexcept { return view_; }
  [[nodiscard]] VkFormat format() const noexcept { return format_; }
  [[nodiscard]] std::uint32_t width() const noexcept { return mip_width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return mip_height_; }
  [[nodiscard]] VkImageLayout layout() const noexcept { return layout_; }
  void transition_to_color_attachment(VkCommandBuffer command) noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkImageView view_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  std::uint32_t mip_width_{};
  std::uint32_t mip_height_{};
  VkImageLayout layout_{VK_IMAGE_LAYOUT_UNDEFINED};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
