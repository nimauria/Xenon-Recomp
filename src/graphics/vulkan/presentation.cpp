#include "xenon/gpu/vulkan/presentation.hpp"

#include <algorithm>
#include <cstring>
#include <limits>

namespace xenon::gpu::vulkan {

PresentationSwapchain::~PresentationSwapchain() { reset(); }

bool PresentationSwapchain::initialize(Context& context, CommandQueue& queue,
                                       VkSurfaceKHR surface,
                                       const PresentationConfig& config,
                                       bool take_surface_ownership) {
  reset();
  error_.clear();
  if (!context.device() || !context.physical_device() || !queue.native_queue() ||
      !surface) {
    error_ = "Vulkan presentation requires a device, queue and VkSurfaceKHR";
    return false;
  }
  if (!context.swapchain_supported()) {
    error_ = "Vulkan device does not support VK_KHR_swapchain";
    return false;
  }
  VkBool32 present_support = VK_FALSE;
  if (vkGetPhysicalDeviceSurfaceSupportKHR(
          context.physical_device(), context.graphics_queue_family(), surface,
          &present_support) != VK_SUCCESS ||
      !present_support) {
    error_ = "selected Vulkan graphics queue cannot present to this surface";
    return false;
  }
  context_ = &context;
  queue_ = &queue;
  surface_ = surface;
  owns_surface_ = take_surface_ownership;
  config_ = config;
  if (!create_sync_objects() || !create_swapchain()) {
    reset();
    return false;
  }
  return true;
}

void PresentationSwapchain::destroy_sync_objects() noexcept {
  if (!context_ || !context_->device()) return;
  for (auto& state : sync_) {
    if (state.acquired) vkDestroySemaphore(context_->device(), state.acquired, nullptr);
    state = {};
  }
}

bool PresentationSwapchain::create_sync_objects() {
  VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (auto& state : sync_) {
    if (vkCreateSemaphore(context_->device(), &info, nullptr, &state.acquired) != VK_SUCCESS) {
      error_ = "vkCreateSemaphore failed for presentation";
      return false;
    }
  }
  return true;
}

void PresentationSwapchain::destroy_swapchain_resources() noexcept {
  if (context_ && context_->device()) {
    for (auto& image : images_) {
      if (image.rendered)
        vkDestroySemaphore(context_->device(), image.rendered, nullptr);
      image.rendered = VK_NULL_HANDLE;
    }
  }
  images_.clear();
  if (context_ && context_->device() && swapchain_)
    vkDestroySwapchainKHR(context_->device(), swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
  extent_ = {};
  surface_format_ = {};
}

void PresentationSwapchain::reset() noexcept {
  if (queue_) {
    (void)queue_->wait_idle();
    if (queue_->native_queue()) (void)vkQueueWaitIdle(queue_->native_queue());
  }
  destroy_swapchain_resources();
  destroy_sync_objects();
  if (owns_surface_ && context_ && context_->instance() && surface_)
    vkDestroySurfaceKHR(context_->instance(), surface_, nullptr);
  context_ = nullptr;
  queue_ = nullptr;
  surface_ = VK_NULL_HANDLE;
  owns_surface_ = false;
  next_sync_ = 0;
  config_ = {};
}

bool PresentationSwapchain::create_swapchain(VkSwapchainKHR old_swapchain) {
  VkSurfaceCapabilitiesKHR capabilities{};
  if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
          context_->physical_device(), surface_, &capabilities) != VK_SUCCESS) {
    error_ = "vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed";
    return false;
  }
  if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
    error_ = "Vulkan presentation surface does not support transfer-destination images";
    return false;
  }
  std::uint32_t format_count{};
  if (vkGetPhysicalDeviceSurfaceFormatsKHR(context_->physical_device(), surface_,
                                            &format_count, nullptr) != VK_SUCCESS ||
      !format_count) {
    error_ = "Vulkan surface exposes no presentation formats";
    return false;
  }
  std::vector<VkSurfaceFormatKHR> formats(format_count);
  vkGetPhysicalDeviceSurfaceFormatsKHR(context_->physical_device(), surface_,
                                        &format_count, formats.data());
  VkSurfaceFormatKHR selected_format = formats.front();
  for (const auto& format : formats) {
    if (format.format == VK_FORMAT_R8G8B8A8_UNORM &&
        format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
      selected_format = format;
      break;
    }
  }
  if (selected_format.format != VK_FORMAT_R8G8B8A8_UNORM &&
      selected_format.format != VK_FORMAT_B8G8R8A8_UNORM &&
      selected_format.format != VK_FORMAT_R8G8B8A8_SRGB &&
      selected_format.format != VK_FORMAT_B8G8R8A8_SRGB) {
    error_ = "Vulkan presentation requires an RGBA8/BGRA8 surface format";
    return false;
  }

  VkExtent2D selected_extent{};
  if (capabilities.currentExtent.width != std::numeric_limits<std::uint32_t>::max()) {
    selected_extent = capabilities.currentExtent;
  } else {
    selected_extent.width = std::clamp(config_.width, capabilities.minImageExtent.width,
                                       capabilities.maxImageExtent.width);
    selected_extent.height = std::clamp(config_.height, capabilities.minImageExtent.height,
                                        capabilities.maxImageExtent.height);
  }
  if (!selected_extent.width || !selected_extent.height) {
    swapchain_ = old_swapchain;
    extent_ = {};
    surface_format_ = selected_format;
    return true;
  }

  std::uint32_t present_mode_count{};
  vkGetPhysicalDeviceSurfacePresentModesKHR(context_->physical_device(), surface_,
                                             &present_mode_count, nullptr);
  std::vector<VkPresentModeKHR> present_modes(present_mode_count);
  if (present_mode_count)
    vkGetPhysicalDeviceSurfacePresentModesKHR(context_->physical_device(), surface_,
                                               &present_mode_count,
                                               present_modes.data());
  VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
  if (!config_.vsync) {
    if (std::find(present_modes.begin(), present_modes.end(),
                  VK_PRESENT_MODE_MAILBOX_KHR) != present_modes.end())
      present_mode = VK_PRESENT_MODE_MAILBOX_KHR;
    else if (std::find(present_modes.begin(), present_modes.end(),
                       VK_PRESENT_MODE_IMMEDIATE_KHR) != present_modes.end())
      present_mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
  }

  std::uint32_t image_count = std::max(capabilities.minImageCount,
                                        std::clamp(config_.image_count, 2u, 4u));
  if (capabilities.maxImageCount)
    image_count = std::min(image_count, capabilities.maxImageCount);
  VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = surface_;
  info.minImageCount = image_count;
  info.imageFormat = selected_format.format;
  info.imageColorSpace = selected_format.colorSpace;
  info.imageExtent = selected_extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = capabilities.currentTransform;
  constexpr VkCompositeAlphaFlagBitsKHR kCompositeAlphaPreference[] = {
      VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
      VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
      VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR,
  };
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  for (const auto alpha : kCompositeAlphaPreference) {
    if (capabilities.supportedCompositeAlpha & alpha) {
      info.compositeAlpha = alpha;
      break;
    }
  }
  info.presentMode = present_mode;
  info.clipped = VK_TRUE;
  info.oldSwapchain = old_swapchain;

  VkSwapchainKHR replacement = VK_NULL_HANDLE;
  if (vkCreateSwapchainKHR(context_->device(), &info, nullptr, &replacement) != VK_SUCCESS) {
    error_ = "vkCreateSwapchainKHR failed";
    return false;
  }

  std::uint32_t actual_count{};
  if (vkGetSwapchainImagesKHR(context_->device(), replacement, &actual_count,
                              nullptr) != VK_SUCCESS || !actual_count) {
    error_ = "vkGetSwapchainImagesKHR failed";
    vkDestroySwapchainKHR(context_->device(), replacement, nullptr);
    return false;
  }
  std::vector<VkImage> native_images(actual_count);
  if (vkGetSwapchainImagesKHR(context_->device(), replacement, &actual_count,
                              native_images.data()) != VK_SUCCESS) {
    error_ = "vkGetSwapchainImagesKHR failed";
    vkDestroySwapchainKHR(context_->device(), replacement, nullptr);
    return false;
  }

  std::vector<ImageState> replacement_images;
  replacement_images.resize(actual_count);
  const auto upload_size = VkDeviceSize(selected_extent.width) *
                           selected_extent.height * 4u;
  bool resources_ok = true;
  for (std::uint32_t i = 0; i < actual_count; ++i) {
    auto& image = replacement_images[i];
    image.image = native_images[i];
    if (!image.upload.initialize(
            context_->physical_device(), context_->device(), upload_size,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !image.upload.map(image.upload_mapping)) {
      error_ = image.upload.error().empty()
                   ? "Vulkan presentation upload allocation failed"
                   : image.upload.error();
      resources_ok = false;
      break;
    }
    VkSemaphoreCreateInfo semaphore_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    if (vkCreateSemaphore(context_->device(), &semaphore_info, nullptr,
                          &image.rendered) != VK_SUCCESS) {
      error_ = "vkCreateSemaphore failed for a swapchain image";
      resources_ok = false;
      break;
    }
  }
  if (!resources_ok) {
    for (auto& image : replacement_images) {
      if (image.rendered)
        vkDestroySemaphore(context_->device(), image.rendered, nullptr);
      image.rendered = VK_NULL_HANDLE;
    }
    replacement_images.clear();
    vkDestroySwapchainKHR(context_->device(), replacement, nullptr);
    return false;
  }

  if (old_swapchain) vkDestroySwapchainKHR(context_->device(), old_swapchain, nullptr);
  swapchain_ = replacement;
  surface_format_ = selected_format;
  extent_ = selected_extent;
  images_ = std::move(replacement_images);
  return true;
}

bool PresentationSwapchain::resize(std::uint32_t width, std::uint32_t height) {
  if (!context_ || !queue_ || !surface_) {
    error_ = "Vulkan presentation is not configured";
    return false;
  }
  config_.width = width;
  config_.height = height;
  if (!width || !height) {
    if (!queue_->wait_idle()) {
      error_ = queue_->error();
      return false;
    }
    if (vkQueueWaitIdle(queue_->native_queue()) != VK_SUCCESS) {
      error_ = "vkQueueWaitIdle failed while minimizing presentation";
      return false;
    }
    destroy_swapchain_resources();
    return true;
  }
  if (!queue_->wait_idle()) {
    error_ = queue_->error();
    return false;
  }
  if (vkQueueWaitIdle(queue_->native_queue()) != VK_SUCCESS) {
    error_ = "vkQueueWaitIdle failed before swapchain recreation";
    return false;
  }
  const auto old = swapchain_;
  // Upload buffers and image-scoped render-finished semaphores belong to the
  // old images. The VkSwapchainKHR itself is retained for WSI handoff.
  for (auto& image : images_) {
    if (image.rendered)
      vkDestroySemaphore(context_->device(), image.rendered, nullptr);
    image.rendered = VK_NULL_HANDLE;
  }
  images_.clear();
  swapchain_ = VK_NULL_HANDLE;
  if (!create_swapchain(old)) {
    if (old && !swapchain_) vkDestroySwapchainKHR(context_->device(), old, nullptr);
    return false;
  }
  return true;
}

PresentStatus PresentationSwapchain::present(
    const PreparedPresentationFrame& frame) {
  if (!context_ || !queue_ || !surface_) return PresentStatus::NotConfigured;
  if (!swapchain_ || !extent_.width || !extent_.height) return PresentStatus::Minimized;
  if (!frame.valid || frame.width != extent_.width || frame.height != extent_.height ||
      frame.row_pitch < extent_.width * 4u) {
    error_ = "Vulkan presentation frame extent does not match the swapchain";
    return PresentStatus::Error;
  }
  auto& sync = sync_[next_sync_];
  if (sync.timeline_value && queue_->completed_value() < sync.timeline_value &&
      !queue_->wait_for_completion(sync.timeline_value)) {
    error_ = queue_->error();
    return PresentStatus::Error;
  }
  std::uint32_t image_index{};
  const auto acquire = vkAcquireNextImageKHR(
      context_->device(), swapchain_, UINT64_MAX, sync.acquired, VK_NULL_HANDLE,
      &image_index);
  if (acquire == VK_ERROR_OUT_OF_DATE_KHR) return PresentStatus::OutOfDate;
  if (acquire == VK_ERROR_SURFACE_LOST_KHR) return PresentStatus::SurfaceLost;
  if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR) {
    error_ = "vkAcquireNextImageKHR failed";
    return PresentStatus::Error;
  }
  if (image_index >= images_.size()) {
    error_ = "Vulkan returned an invalid swapchain image index";
    return PresentStatus::Error;
  }
  auto& image = images_[image_index];
  if (image.timeline_value && queue_->completed_value() < image.timeline_value &&
      !queue_->wait_for_completion(image.timeline_value)) {
    error_ = queue_->error();
    return PresentStatus::Error;
  }
  const bool bgra = surface_format_.format == VK_FORMAT_B8G8R8A8_UNORM ||
                    surface_format_.format == VK_FORMAT_B8G8R8A8_SRGB;
  for (std::uint32_t y = 0; y < extent_.height; ++y) {
    const auto* src = reinterpret_cast<const std::uint8_t*>(
        frame.rgba8.data() + std::size_t(y) * frame.row_pitch);
    auto* dst = reinterpret_cast<std::uint8_t*>(
        image.upload_mapping.data() + std::size_t(y) * extent_.width * 4u);
    if (!bgra) {
      std::memcpy(dst, src, std::size_t(extent_.width) * 4u);
    } else {
      for (std::uint32_t x = 0; x < extent_.width; ++x) {
        dst[x * 4u + 0u] = src[x * 4u + 2u];
        dst[x * 4u + 1u] = src[x * 4u + 1u];
        dst[x * 4u + 2u] = src[x * 4u + 0u];
        dst[x * 4u + 3u] = src[x * 4u + 3u];
      }
    }
  }

  if (!queue_->execute_present(
          [&](VkCommandBuffer command) {
            VkImageMemoryBarrier2 to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_copy.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            to_copy.srcAccessMask = 0;
            to_copy.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_copy.image = image.image;
            to_copy.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            to_copy.subresourceRange.levelCount = 1;
            to_copy.subresourceRange.layerCount = 1;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.imageMemoryBarrierCount = 1;
            dependency.pImageMemoryBarriers = &to_copy;
            vkCmdPipelineBarrier2(command, &dependency);

            VkBufferImageCopy copy{};
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageExtent = {extent_.width, extent_.height, 1};
            vkCmdCopyBufferToImage(command, image.upload.buffer(), image.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);

            VkImageMemoryBarrier2 to_present{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_present.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            to_present.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_present.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
            to_present.dstAccessMask = 0;
            to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            to_present.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_present.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_present.image = image.image;
            to_present.subresourceRange = to_copy.subresourceRange;
            dependency.pImageMemoryBarriers = &to_present;
            vkCmdPipelineBarrier2(command, &dependency);
          },
          sync.acquired, VK_PIPELINE_STAGE_2_TRANSFER_BIT, image.rendered)) {
    error_ = queue_->error();
    return PresentStatus::Error;
  }
  sync.timeline_value = queue_->last_submitted_value();
  image.timeline_value = sync.timeline_value;

  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &image.rendered;
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &image_index;
  const auto result = vkQueuePresentKHR(queue_->native_queue(), &present);
  next_sync_ = (next_sync_ + 1u) % kSyncCount;
  if (result == VK_ERROR_OUT_OF_DATE_KHR) return PresentStatus::OutOfDate;
  if (result == VK_ERROR_SURFACE_LOST_KHR) return PresentStatus::SurfaceLost;
  if (result == VK_SUBOPTIMAL_KHR || acquire == VK_SUBOPTIMAL_KHR)
    return PresentStatus::Suboptimal;
  if (result != VK_SUCCESS) {
    error_ = "vkQueuePresentKHR failed";
    return PresentStatus::Error;
  }
  return PresentStatus::Success;
}

}  // namespace xenon::gpu::vulkan
