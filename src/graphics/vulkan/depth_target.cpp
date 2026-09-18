#include "xenon/gpu/vulkan/depth_target.hpp"

namespace xenon::gpu::vulkan {
namespace {

std::uint32_t find_memory_type(VkPhysicalDevice physical_device,
                               std::uint32_t bits,
                               VkMemoryPropertyFlags required) {
  VkPhysicalDeviceMemoryProperties properties{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
  for (std::uint32_t i = 0; i < properties.memoryTypeCount; ++i)
    if ((bits & (1u << i)) &&
        (properties.memoryTypes[i].propertyFlags & required) == required)
      return i;
  return UINT32_MAX;
}

VkSampleCountFlagBits sample_count(MsaaSamples samples) {
  switch (samples) {
    case MsaaSamples::X1: return VK_SAMPLE_COUNT_1_BIT;
    case MsaaSamples::X2: return VK_SAMPLE_COUNT_2_BIT;
    case MsaaSamples::X4: return VK_SAMPLE_COUNT_4_BIT;
  }
  return VK_SAMPLE_COUNT_1_BIT;
}

}  // namespace

VkFormat depth_render_target_format(DepthRenderTargetFormat format) noexcept {
  return format == DepthRenderTargetFormat::D24S8
             ? VK_FORMAT_D24_UNORM_S8_UINT
             : VK_FORMAT_D32_SFLOAT_S8_UINT;
}

DepthTargetImage::~DepthTargetImage() { reset(); }

bool DepthTargetImage::initialize(VkPhysicalDevice physical_device,
                                  VkDevice device,
                                  const EdramSurfaceLayout& surface,
                                  DepthRenderTargetFormat depth_format) {
  reset();
  error_.clear();
  if (!physical_device || !device || !surface.valid() || !surface.depth) {
    error_ = "Vulkan depth target requires a valid depth EDRAM surface";
    return false;
  }
  device_ = device;
  format_ = depth_render_target_format(depth_format);
  width_ = surface.pitch_pixels;
  height_ = surface.height_pixels;
  surface_ = surface;
  float24_ = depth_format == DepthRenderTargetFormat::D24FS8;
  VkFormatProperties properties{};
  vkGetPhysicalDeviceFormatProperties(physical_device, format_, &properties);
  if (!(properties.optimalTilingFeatures &
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
    error_ = "Vulkan device does not support the selected depth format";
    reset();
    return false;
  }
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = format_;
  info.extent = {surface.pitch_pixels, surface.height_pixels, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = sample_count(surface.msaa);
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(device_, &info, nullptr, &image_) != VK_SUCCESS) {
    error_ = "vkCreateImage failed for Xenos depth target";
    reset();
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, image_, &requirements);
  const auto type = find_memory_type(physical_device, requirements.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = type;
  if (type == UINT32_MAX ||
      vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS ||
      vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
    error_ = "Vulkan depth-target memory allocation failed";
    reset();
    return false;
  }
  VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view.image = image_;
  view.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view.format = format_;
  view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                           0, 1, 0, 1};
  if (vkCreateImageView(device_, &view, nullptr, &view_) != VK_SUCCESS) {
    error_ = "vkCreateImageView failed for Xenos depth target";
    reset();
    return false;
  }
  return true;
}

void DepthTargetImage::transition_to_depth_attachment(
    VkCommandBuffer command) noexcept {
  if (!command || !image_ ||
      layout_ == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL)
    return;
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = layout_ == VK_IMAGE_LAYOUT_UNDEFINED
                             ? VK_PIPELINE_STAGE_2_NONE
                             : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = layout_ == VK_IMAGE_LAYOUT_UNDEFINED
                              ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                         VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout = layout_;
  barrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  barrier.image = image_;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT,
                              0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(command, &dependency);
  layout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

bool DepthTargetImage::clear(CommandQueue& queue, float depth,
                             std::uint8_t stencil) {
  if (!image_) return false;
  const auto old_layout = layout_;
  if (!queue.execute([&](VkCommandBuffer command) {
        VkImageMemoryBarrier2 before{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        before.srcStageMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                  ? VK_PIPELINE_STAGE_2_NONE
                                  : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        before.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        before.oldLayout = old_layout;
        before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before.image = image_;
        before.subresourceRange = {
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &before;
        vkCmdPipelineBarrier2(command, &dependency);
        const VkClearDepthStencilValue value{depth, stencil};
        const VkImageSubresourceRange range{
            VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, 0, 1, 0, 1};
        vkCmdClearDepthStencilImage(command, image_,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    &value, 1, &range);
        auto after = before;
        after.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                             VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        after.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                              VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        dependency.pImageMemoryBarriers = &after;
        vkCmdPipelineBarrier2(command, &dependency);
      })) {
    error_ = queue.error();
    return false;
  }
  layout_ = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  return true;
}

void DepthTargetImage::reset() noexcept {
  if (device_ && view_) vkDestroyImageView(device_, view_, nullptr);
  if (device_ && image_) vkDestroyImage(device_, image_, nullptr);
  if (device_ && memory_) vkFreeMemory(device_, memory_, nullptr);
  view_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  format_ = VK_FORMAT_UNDEFINED;
  layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  float24_ = false;
  width_ = height_ = 0;
  surface_ = {};
}

}  // namespace xenon::gpu::vulkan
