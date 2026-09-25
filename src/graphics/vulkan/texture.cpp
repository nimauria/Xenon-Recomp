#include "xenon/gpu/vulkan/texture.hpp"

#include <algorithm>
#include <cstring>
#include <span>
#include <vector>

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

VkImageViewType view_type(const TextureDescriptor& descriptor) {
  switch (descriptor.dimension) {
    case TextureDimension::OneD: return VK_IMAGE_VIEW_TYPE_1D;
    case TextureDimension::ThreeD: return VK_IMAGE_VIEW_TYPE_3D;
    case TextureDimension::Cube: return VK_IMAGE_VIEW_TYPE_CUBE;
    case TextureDimension::TwoDOrStacked:
      return descriptor.stacked ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
  }
  return VK_IMAGE_VIEW_TYPE_2D;
}

VkImageType image_type(TextureDimension dimension) {
  return dimension == TextureDimension::OneD ? VK_IMAGE_TYPE_1D :
         dimension == TextureDimension::ThreeD ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
}

VkSamplerAddressMode address_mode(std::uint8_t clamp, std::uint64_t& unsupported_counter) {
  switch (clamp) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    default:
      // Real Xenos clamp mode is 0-3; anything else is guest-visible
      // garbage or a value this backend does not yet map. Previously
      // silently defaulted to CLAMP_TO_BORDER with zero observability -
      // Part 7 of the AC6 Runtime Readiness pass requires this be counted.
      ++unsupported_counter;
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
}

VkComponentSwizzle component_swizzle(std::uint16_t swizzle,
                                     std::uint32_t component) {
  switch ((swizzle >> (component * 3u)) & 7u) {
    case 0: return VK_COMPONENT_SWIZZLE_R;
    case 1: return VK_COMPONENT_SWIZZLE_G;
    case 2: return VK_COMPONENT_SWIZZLE_B;
    case 3: return VK_COMPONENT_SWIZZLE_A;
    case 4: return VK_COMPONENT_SWIZZLE_ZERO;
    case 5: return VK_COMPONENT_SWIZZLE_ONE;
    default: return VK_COMPONENT_SWIZZLE_IDENTITY;
  }
}

float max_anisotropy(std::uint8_t filter) {
  if (!filter) return 1.0f;
  return float(std::min(16u, 1u << std::min(4u, std::uint32_t(filter - 1u))));
}

}  // namespace

VkFormat host_texture_format(TextureHostFormat format) noexcept {
  switch (format) {
    case TextureHostFormat::R8Unorm: return VK_FORMAT_R8_UNORM;
    case TextureHostFormat::R8G8Unorm: return VK_FORMAT_R8G8_UNORM;
    case TextureHostFormat::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case TextureHostFormat::R8G8B8A8Srgb: return VK_FORMAT_R8G8B8A8_SRGB;
    case TextureHostFormat::B5G5R5A1Unorm: return VK_FORMAT_B5G5R5A1_UNORM_PACK16;
    case TextureHostFormat::B5G6R5Unorm: return VK_FORMAT_B5G6R5_UNORM_PACK16;
    case TextureHostFormat::B4G4R4A4Unorm: return VK_FORMAT_B4G4R4A4_UNORM_PACK16;
    case TextureHostFormat::R10G10B10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case TextureHostFormat::R11G11B10Float: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case TextureHostFormat::R16Unorm: return VK_FORMAT_R16_UNORM;
    case TextureHostFormat::R16G16Unorm: return VK_FORMAT_R16G16_UNORM;
    case TextureHostFormat::R16G16B16A16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case TextureHostFormat::R16Float: return VK_FORMAT_R16_SFLOAT;
    case TextureHostFormat::R16G16Float: return VK_FORMAT_R16G16_SFLOAT;
    case TextureHostFormat::R16G16B16A16Float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case TextureHostFormat::R32Uint: return VK_FORMAT_R32_UINT;
    case TextureHostFormat::R32G32Uint: return VK_FORMAT_R32G32_UINT;
    case TextureHostFormat::R32G32B32A32Uint: return VK_FORMAT_R32G32B32A32_UINT;
    case TextureHostFormat::R32Float: return VK_FORMAT_R32_SFLOAT;
    case TextureHostFormat::R32G32Float: return VK_FORMAT_R32G32_SFLOAT;
    case TextureHostFormat::R32G32B32Float: return VK_FORMAT_R32G32B32_SFLOAT;
    case TextureHostFormat::R32G32B32A32Float: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case TextureHostFormat::D24UnormS8Uint: return VK_FORMAT_D24_UNORM_S8_UINT;
    case TextureHostFormat::BC1Unorm: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case TextureHostFormat::BC2Unorm: return VK_FORMAT_BC2_UNORM_BLOCK;
    case TextureHostFormat::BC3Unorm: return VK_FORMAT_BC3_UNORM_BLOCK;
    case TextureHostFormat::BC4Unorm: return VK_FORMAT_BC4_UNORM_BLOCK;
    case TextureHostFormat::BC5Unorm: return VK_FORMAT_BC5_UNORM_BLOCK;
    case TextureHostFormat::Unsupported: break;
  }
  return VK_FORMAT_UNDEFINED;
}

TextureImage::~TextureImage() { reset(); }

bool TextureImage::initialize(VkPhysicalDevice physical_device, VkDevice device,
                              CommandQueue& queue,
                              const DecodedTexture& texture) {
  reset();
  error_.clear();
  unsupported_format_ = false;
  unsupported_sampler_behaviors_ = 0;
  if (!texture.valid || !physical_device || !device) {
    error_ = "Vulkan texture image requires decoded texture data and a device";
    return false;
  }
  device_ = device;
  format_ = host_texture_format(texture.layout.format.host_format);
  if (format_ == VK_FORMAT_UNDEFINED) {
    error_ = "Xenos texture format has no Vulkan image mapping";
    unsupported_format_ = true;
    reset();
    return false;
  }
  VkFormatProperties format_properties{};
  vkGetPhysicalDeviceFormatProperties(physical_device, format_, &format_properties);
  constexpr VkFormatFeatureFlags required_features =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((format_properties.optimalTilingFeatures & required_features) !=
      required_features) {
    error_ = "Vulkan device cannot sample and upload the mapped Xenos texture format";
    reset();
    return false;
  }
  const auto& descriptor = texture.layout.descriptor;
  const auto mip_levels = std::uint32_t(descriptor.mip_max_level) + 1u;
  const auto layers = descriptor.dimension == TextureDimension::Cube ? 6u :
                      descriptor.dimension == TextureDimension::TwoDOrStacked && descriptor.stacked
                          ? descriptor.depth : 1u;
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.flags = descriptor.dimension == TextureDimension::Cube
                         ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  image_info.imageType = image_type(descriptor.dimension);
  image_info.format = format_;
  image_info.extent = {descriptor.width, descriptor.height,
                       descriptor.dimension == TextureDimension::ThreeD
                           ? descriptor.depth : 1u};
  image_info.mipLevels = mip_levels;
  image_info.arrayLayers = layers;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageFormatProperties image_properties{};
  if (vkGetPhysicalDeviceImageFormatProperties(
          physical_device, format_, image_info.imageType, image_info.tiling,
          image_info.usage, image_info.flags, &image_properties) != VK_SUCCESS ||
      image_info.extent.width > image_properties.maxExtent.width ||
      image_info.extent.height > image_properties.maxExtent.height ||
      image_info.extent.depth > image_properties.maxExtent.depth ||
      image_info.mipLevels > image_properties.maxMipLevels ||
      image_info.arrayLayers > image_properties.maxArrayLayers) {
    error_ = "Vulkan device cannot create the requested Xenos texture image";
    reset();
    return false;
  }
  if (vkCreateImage(device_, &image_info, nullptr, &image_) != VK_SUCCESS) {
    error_ = "vkCreateImage failed for decoded Xenos texture";
    reset(); return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, image_, &requirements);
  const auto memory_type = find_memory_type(physical_device,
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memory_type == UINT32_MAX) {
    error_ = "no device-local Vulkan memory type for Xenos texture";
    reset(); return false;
  }
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS ||
      vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
    error_ = "Vulkan texture memory allocation or binding failed";
    reset(); return false;
  }
  Buffer staging;
  if (!staging.initialize(physical_device, device_, texture.linear_data.size(),
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = staging.error(); reset(); return false;
  }
  std::span<std::byte> mapped;
  if (!staging.map(mapped)) { error_ = staging.error(); reset(); return false; }
  std::memcpy(mapped.data(), texture.linear_data.data(), texture.linear_data.size());
  staging.unmap();
  std::vector<VkBufferImageCopy> copies;
  copies.reserve(texture.layout.subresources.size());
  for (const auto& sub : texture.layout.subresources) {
    VkBufferImageCopy copy{};
    copy.bufferOffset = sub.linear_offset_bytes;
    copy.imageSubresource.aspectMask =
        texture.layout.format.host_format == TextureHostFormat::D24UnormS8Uint
            ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
            : VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.mipLevel = sub.mip_level;
    copy.imageSubresource.baseArrayLayer = sub.array_layer;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {sub.width_texels, sub.height_texels, sub.depth_texels};
    copies.push_back(copy);
  }
  const auto aspect = copies.front().imageSubresource.aspectMask;
  if (!queue.execute([&](VkCommandBuffer command) {
        VkImageMemoryBarrier2 to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_copy.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_copy.image = image_;
        to_copy.subresourceRange = {aspect, 0, mip_levels, 0, layers};
        VkDependencyInfo before{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        before.imageMemoryBarrierCount = 1;
        before.pImageMemoryBarriers = &to_copy;
        vkCmdPipelineBarrier2(command, &before);
        vkCmdCopyBufferToImage(command, staging.buffer(), image_,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               static_cast<std::uint32_t>(copies.size()), copies.data());
        VkImageMemoryBarrier2 to_shader = to_copy;
        to_shader.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_shader.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT |
                                 VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_shader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDependencyInfo after{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        after.imageMemoryBarrierCount = 1;
        after.pImageMemoryBarriers = &to_shader;
        vkCmdPipelineBarrier2(command, &after);
      })) {
    error_ = queue.error(); reset(); return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image_;
  view_info.viewType = view_type(descriptor);
  view_info.format = format_;
  view_info.components = {
      component_swizzle(descriptor.swizzle, 0),
      component_swizzle(descriptor.swizzle, 1),
      component_swizzle(descriptor.swizzle, 2),
      component_swizzle(descriptor.swizzle, 3)};
  view_info.subresourceRange = {aspect, 0, mip_levels, 0, layers};
  if (vkCreateImageView(device_, &view_info, nullptr, &view_) != VK_SUCCESS) {
    error_ = "vkCreateImageView failed for Xenos texture";
    reset(); return false;
  }
  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = descriptor.mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_info.minFilter = descriptor.min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_info.mipmapMode = descriptor.mip_filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                   : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = address_mode(descriptor.clamps[0], unsupported_sampler_behaviors_);
  sampler_info.addressModeV = address_mode(descriptor.clamps[1], unsupported_sampler_behaviors_);
  sampler_info.addressModeW = address_mode(descriptor.clamps[2], unsupported_sampler_behaviors_);
  sampler_info.mipLodBias = float(descriptor.lod_bias) / 32.0f;
  sampler_info.minLod = float(descriptor.mip_min_level);
  sampler_info.maxLod = float(descriptor.mip_max_level);
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(physical_device, &features);
  sampler_info.anisotropyEnable = descriptor.aniso_filter && features.samplerAnisotropy;
  sampler_info.maxAnisotropy = sampler_info.anisotropyEnable
                                   ? max_anisotropy(descriptor.aniso_filter) : 1.0f;
  if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    error_ = "vkCreateSampler failed for Xenos texture";
    reset(); return false;
  }
  return true;
}

void TextureImage::reset() noexcept {
  if (device_ && sampler_) vkDestroySampler(device_, sampler_, nullptr);
  if (device_ && view_) vkDestroyImageView(device_, view_, nullptr);
  if (device_ && image_) vkDestroyImage(device_, image_, nullptr);
  if (device_ && memory_) vkFreeMemory(device_, memory_, nullptr);
  sampler_ = VK_NULL_HANDLE; view_ = VK_NULL_HANDLE; image_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE; format_ = VK_FORMAT_UNDEFINED; device_ = VK_NULL_HANDLE;
}

}  // namespace xenon::gpu::vulkan
