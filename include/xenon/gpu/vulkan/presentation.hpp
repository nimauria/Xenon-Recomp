#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
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
  [[nodiscard]] PresentStatus present(const PreparedPresentationFrame& frame);

  [[nodiscard]] bool ready() const noexcept { return swapchain_ != VK_NULL_HANDLE; }
  [[nodiscard]] std::uint32_t width() const noexcept { return extent_.width; }
  [[nodiscard]] std::uint32_t height() const noexcept { return extent_.height; }
  [[nodiscard]] const PresentationConfig& config() const noexcept { return config_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }

 private:
  struct ImageState {
    VkImage image{VK_NULL_HANDLE};
    Buffer upload{};
    std::span<std::byte> upload_mapping{};
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
