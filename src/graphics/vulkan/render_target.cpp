#include "xenon/gpu/vulkan/render_target.hpp"

#include "xenon/gpu/vulkan/memory.hpp"

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

VkFormat color_render_target_format(ColorRenderTargetFormat format) noexcept {
  switch (format) {
    case ColorRenderTargetFormat::R8G8B8A8: return VK_FORMAT_R8G8B8A8_UNORM;
    case ColorRenderTargetFormat::R8G8B8A8Gamma: return VK_FORMAT_R8G8B8A8_SRGB;
    case ColorRenderTargetFormat::R10G10B10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case ColorRenderTargetFormat::R16G16Float: return VK_FORMAT_R16G16_SFLOAT;
    case ColorRenderTargetFormat::R16G16B16A16Float:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case ColorRenderTargetFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
    case ColorRenderTargetFormat::R32G32Float: return VK_FORMAT_R32G32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

RenderTargetImage::~RenderTargetImage() { reset(); }

bool RenderTargetImage::initialize(VkPhysicalDevice physical_device,
                                   VkDevice device, CommandQueue&,
                                   const EdramSurfaceLayout& surface,
                                   ColorRenderTargetFormat color_format) {
  reset();
  error_.clear();
  if (!physical_device || !device || !surface.valid() || surface.depth) {
    error_ = "Vulkan color target requires a valid color EDRAM surface";
    return false;
  }
  format_ = color_render_target_format(color_format);
  if (format_ == VK_FORMAT_UNDEFINED) {
    error_ = "Xenos color target format needs a non-native conversion path";
    return false;
  }
  device_ = device;
  mip_width_ = surface.pitch_pixels;
  mip_height_ = surface.height_pixels;
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format_;
  image_info.extent = {mip_width_, mip_height_, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = sample_count(surface.msaa);
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateImage(device_, &image_info, nullptr, &image_) != VK_SUCCESS) {
    error_ = "vkCreateImage failed for Xenos color target";
    reset(); return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, image_, &requirements);
  const auto memory_type = find_memory_type(physical_device,
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type;
  if (memory_type == UINT32_MAX ||
      vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS ||
      vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
    error_ = "Vulkan color-target memory allocation failed";
    reset(); return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image_;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format_;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &view_info, nullptr, &view_) != VK_SUCCESS) {
    error_ = "vkCreateImageView failed for Xenos color target";
    reset(); return false;
  }
  return true;
}

bool RenderTargetImage::clear(CommandQueue& queue,
                              const VkClearColorValue& value) {
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
        before.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &before;
        vkCmdPipelineBarrier2(command, &dependency);
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(command, image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &value, 1, &range);
        auto after = before;
        after.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
        after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        after.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        after.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                              VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        after.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        after.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        dependency.pImageMemoryBarriers = &after;
        vkCmdPipelineBarrier2(command, &dependency);
      })) {
    error_ = queue.error();
    return false;
  }
  layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  return true;
}

void RenderTargetImage::transition_to_color_attachment(
    VkCommandBuffer command) noexcept {
  if (!command || !image_ || layout_ == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
    return;
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = layout_ == VK_IMAGE_LAYOUT_UNDEFINED
                             ? VK_PIPELINE_STAGE_2_NONE
                             : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
  barrier.srcAccessMask = layout_ == VK_IMAGE_LAYOUT_UNDEFINED
                              ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT |
                          VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout = layout_;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.image = image_;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(command, &dependency);
  layout_ = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

void RenderTargetImage::reset() noexcept {
  if (device_ && view_) vkDestroyImageView(device_, view_, nullptr);
  if (device_ && image_) vkDestroyImage(device_, image_, nullptr);
  if (device_ && memory_) vkFreeMemory(device_, memory_, nullptr);
  view_ = VK_NULL_HANDLE; image_ = VK_NULL_HANDLE; memory_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE; format_ = VK_FORMAT_UNDEFINED;
  mip_width_ = mip_height_ = 0; layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

}  // namespace xenon::gpu::vulkan
