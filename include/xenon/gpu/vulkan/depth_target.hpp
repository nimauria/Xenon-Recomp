#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

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
                                DepthRenderTargetFormat format,
                                bool native_2x_supported = true);
  [[nodiscard]] bool clear(CommandQueue& queue, float depth,
                           std::uint8_t stencil);
  [[nodiscard]] bool readback_sample(
      CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
      std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
      std::vector<std::uint32_t>& destination,
      std::uint32_t& row_pitch);
  // Host-sample readback is intended for backend validation and diagnostics.
  // Guest-visible ownership and resolve code must use readback_sample so the
  // Xenos-to-host sample mapping remains centralized.
  [[nodiscard]] bool readback_native_sample(
      CommandQueue& queue, std::uint32_t host_sample, std::uint32_t left,
      std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
      std::vector<std::uint32_t>& destination,
      std::uint32_t& row_pitch);
  [[nodiscard]] bool upload_sample(
      CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
      std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
      std::span<const std::uint32_t> source, std::uint32_t row_pitch);
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
  [[nodiscard]] DepthRenderTargetFormat guest_format() const noexcept {
    return guest_format_;
  }
  [[nodiscard]] MsaaSamples host_msaa() const noexcept { return host_msaa_; }
  [[nodiscard]] bool requires_float24_conversion() const noexcept {
    return float24_;
  }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  VkDevice device_{VK_NULL_HANDLE};
  VkPhysicalDevice physical_device_{VK_NULL_HANDLE};
  VkImage image_{VK_NULL_HANDLE};
  VkDeviceMemory memory_{VK_NULL_HANDLE};
  VkImageView view_{VK_NULL_HANDLE};
  VkImageView depth_view_{VK_NULL_HANDLE};
  VkImageView stencil_view_{VK_NULL_HANDLE};
  VkFormat format_{VK_FORMAT_UNDEFINED};
  DepthRenderTargetFormat guest_format_{DepthRenderTargetFormat::D24S8};
  MsaaSamples host_msaa_{MsaaSamples::X1};
  VkImageLayout layout_{VK_IMAGE_LAYOUT_UNDEFINED};
  std::uint32_t width_{};
  std::uint32_t height_{};
  EdramSurfaceLayout surface_{};
  bool float24_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
