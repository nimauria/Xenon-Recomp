#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/context.hpp"
#include "xenon/gpu/vulkan/memory.hpp"

namespace xenon::gpu::vulkan {

class PresentationSwapchain {
 public:
  static constexpr std::uint32_t kSyncCount = 3;

  ~PresentationSwapchain();
  [[nodiscard]] bool initialize(Context& context, CommandQueue& queue,
                                VkSurfaceKHR surface,
                                const PresentationConfig& config,
                                bool take_surface_ownership = false);
  void reset() noexcept;
  [[nodiscard]] bool resize(std::uint32_t width, std::uint32_t height);
  // Records `draw` into the acquired image (in COLOR_ATTACHMENT_OPTIMAL
  // layout, of format()) and presents it.
  using DrawBackBuffer = std::function<void(VkCommandBuffer, VkImageView)>;
  [[nodiscard]] PresentStatus present(const DrawBackBuffer& draw);
  [[nodiscard]] VkFormat format() const noexcept { return surface_format_.format; }
  // The surface re-encodes written colors to sRGB.
  [[nodiscard]] bool srgb() const noexcept {
    return surface_format_.format == VK_FORMAT_R8G8B8A8_SRGB ||
           surface_format_.format == VK_FORMAT_B8G8R8A8_SRGB;
  }

  [[nodiscard]] bool ready() const noexcept { return swapchain_ != VK_NULL_HANDLE; }
  [[nodiscard]] std::uint32_t width() const noexcept { return extent_.width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return extent_.height; }
  [[nodiscard]] const PresentationConfig& config() const noexcept { return config_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  struct ImageState {
    VkImage image{VK_NULL_HANDLE};
    VkImageView view{VK_NULL_HANDLE};
    VkSemaphore rendered{VK_NULL_HANDLE};
    std::uint64_t timeline_value{};
  };
  struct SyncState {
    VkSemaphore acquired{VK_NULL_HANDLE};
    std::uint64_t timeline_value{};
  };

  [[nodiscard]] bool create_swapchain(VkSwapchainKHR old_swapchain = VK_NULL_HANDLE);
  void destroy_swapchain_resources() noexcept;
  [[nodiscard]] bool create_sync_objects();
  void destroy_sync_objects() noexcept;

  Context* context_{};
  CommandQueue* queue_{};
  VkSurfaceKHR surface_{VK_NULL_HANDLE};
  bool owns_surface_{};
  VkSwapchainKHR swapchain_{VK_NULL_HANDLE};
  VkSurfaceFormatKHR surface_format_{};
  VkExtent2D extent_{};
  std::vector<ImageState> images_{};
  std::array<SyncState, kSyncCount> sync_{};
  std::uint32_t next_sync_{};
  PresentationConfig config_{};
  std::string error_{};
};

}  // namespace xenon::gpu::vulkan
