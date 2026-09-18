#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

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
                                ColorRenderTargetFormat format,
                                bool native_2x_supported = true);
  [[nodiscard]] bool clear(CommandQueue& queue,
                           const VkClearColorValue& value);
  [[nodiscard]] bool upload(CommandQueue& queue,
                            std::span<const std::byte> source,
                            std::uint32_t row_pitch);
  [[nodiscard]] bool readback(CommandQueue& queue, std::uint32_t left,
                              std::uint32_t top, std::uint32_t right,
                              std::uint32_t bottom,
                              std::vector<std::byte>& destination,
                              std::uint32_t& row_pitch);
  [[nodiscard]] bool readback_sample(
      CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
      std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
      std::vector<std::byte>& destination, std::uint32_t& row_pitch);
  [[nodiscard]] bool upload_sample(
      CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
      std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
      std::span<const std::byte> source, std::uint32_t row_pitch);
  void reset() noexcept;
  [[nodiscard]] VkImage image() const noexcept { return image_; }
  [[nodiscard]] VkImageView view() const noexcept { return view_; }
  [[nodiscard]] VkFormat format() const noexcept { return format_; }
  [[nodiscard]] ColorRenderTargetFormat guest_format() const noexcept {
    return guest_format_;
  }
  [[nodiscard]] std::uint32_t width() const noexcept { return mip_width_; }
  [[nodiscard]] std::uint32_t height() const noexcept { return mip_height_; }
  [[nodiscard]] VkImageLayout layout() const noexcept { return layout_; }
  [[nodiscard]] const EdramSurfaceLayout& surface() const noexcept {
    return surface_;
  }
  void transition_to_color_attachment(VkCommandBuffer command) noexcept;
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkImageView view_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  ColorRenderTargetFormat guest_format_{ColorRenderTargetFormat::R8G8B8A8};
  std::uint32_t mip_width_{};
  std::uint32_t mip_height_{};
  std::uint32_t bytes_per_pixel_{};
  EdramSurfaceLayout surface_{};
  MsaaSamples host_msaa_{MsaaSamples::X1};
  VkImageLayout layout_{VK_IMAGE_LAYOUT_UNDEFINED};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
