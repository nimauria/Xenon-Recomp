#include "xenon/gpu/vulkan/texture.hpp"
#include "xenon/gpu/texture.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>
#include <vector>

#include "xenon/gpu/sampler_address.hpp"
#include "xenon/gpu/gpu_texture_load.hpp"
#include "xenon/gpu/vulkan/memory.hpp"
#include "xenon/memory/types.hpp"

#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#endif

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
  SamplerAddress address{};
  if (!xenos_sampler_address(clamp, address)) {
    ++unsupported_counter;
    return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  switch (address) {
    case SamplerAddress::Wrap: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case SamplerAddress::Mirror: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case SamplerAddress::ClampToEdge: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case SamplerAddress::MirrorOnceToEdge: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
    case SamplerAddress::ClampToBorder: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
  }
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
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

bool TextureImage::create(VkPhysicalDevice physical_device, VkDevice device,
                          const TextureLayout& layout) {
  reset();
  error_.clear();
  unsupported_format_ = false;
  unsupported_sampler_behaviors_ = 0;
  if (!layout.valid || !physical_device || !device) {
    error_ = "Vulkan texture image requires a valid texture layout and a device";
    return false;
  }
  device_ = device;
  format_ = host_texture_format(layout.format.host_format);
  if (format_ == VK_FORMAT_UNDEFINED) {
    error_ = "Xenos texture format has no Vulkan image mapping";
    unsupported_format_ = true;
    reset();
    unsupported_format_ = true;
    return false;
  }
  VkFormatProperties format_properties{};
  vkGetPhysicalDeviceFormatProperties(physical_device, format_, &format_properties);
  constexpr VkFormatFeatureFlags required_features =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  if ((format_properties.optimalTilingFeatures & required_features) != required_features) {
    error_ = "Vulkan device cannot sample and upload the mapped Xenos texture format";
    reset();
    return false;
  }
  const auto& descriptor = layout.descriptor;
  mip_levels_ = std::uint32_t(descriptor.mip_max_level) + 1u;
  layers_ = descriptor.dimension == TextureDimension::Cube ? 6u
            : descriptor.dimension == TextureDimension::TwoDOrStacked && descriptor.stacked
                ? descriptor.depth
                : 1u;
  aspect_ = layout.format.host_format == TextureHostFormat::D24UnormS8Uint
                ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT
                : VK_IMAGE_ASPECT_COLOR_BIT;
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.flags = descriptor.dimension == TextureDimension::Cube
                         ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
  image_info.imageType = image_type(descriptor.dimension);
  image_info.format = format_;
  image_info.extent = {descriptor.width, descriptor.height,
                       descriptor.dimension == TextureDimension::ThreeD ? descriptor.depth : 1u};
  image_info.mipLevels = mip_levels_;
  image_info.arrayLayers = layers_;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  // Readable as a copy source too, for captures and checks of loaded texels.
  image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT;
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
    error_ = "vkCreateImage failed for Xenos texture";
    reset();
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device_, image_, &requirements);
  const auto memory_type = find_memory_type(physical_device, requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (memory_type == UINT32_MAX) {
    error_ = "no device-local Vulkan memory type for Xenos texture";
    reset();
    return false;
  }
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = memory_type;
  if (vkAllocateMemory(device_, &allocation, nullptr, &memory_) != VK_SUCCESS ||
      vkBindImageMemory(device_, image_, memory_, 0) != VK_SUCCESS) {
    error_ = "Vulkan texture memory allocation or binding failed";
    reset();
    return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image_;
  view_info.viewType = view_type(descriptor);
  view_info.format = format_;
  const auto swizzle = host_view_swizzle(descriptor.format, descriptor.swizzle);
  view_info.components = {component_swizzle(swizzle, 0), component_swizzle(swizzle, 1),
                          component_swizzle(swizzle, 2), component_swizzle(swizzle, 3)};
  view_info.subresourceRange = {aspect_, 0, mip_levels_, 0, layers_};
  if (vkCreateImageView(device_, &view_info, nullptr, &view_) != VK_SUCCESS) {
    error_ = "vkCreateImageView failed for Xenos texture";
    reset();
    return false;
  }
  VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sampler_info.magFilter = descriptor.mag_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_info.minFilter = descriptor.min_filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  sampler_info.mipmapMode = descriptor.mip_filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                   : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampler_info.addressModeU = address_mode(descriptor.clamps[0], unsupported_sampler_behaviors_);
  sampler_info.addressModeV = address_mode(descriptor.clamps[1], unsupported_sampler_behaviors_);
  sampler_info.addressModeW = address_mode(descriptor.clamps[2], unsupported_sampler_behaviors_);
  sampler_info.borderColor = xenos_border_is_white(descriptor.border_color)
                                 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                 : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  sampler_info.mipLodBias = float(descriptor.lod_bias) / 32.0f;
  sampler_info.minLod = float(descriptor.mip_min_level);
  sampler_info.maxLod = float(descriptor.mip_max_level);
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(physical_device, &features);
  sampler_info.anisotropyEnable = descriptor.aniso_filter && features.samplerAnisotropy;
  sampler_info.maxAnisotropy =
      sampler_info.anisotropyEnable ? max_anisotropy(descriptor.aniso_filter) : 1.0f;
  if (vkCreateSampler(device_, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
    error_ = "vkCreateSampler failed for Xenos texture";
    reset();
    return false;
  }
  descriptor_ = descriptor;
  layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  return true;
}

bool TextureImage::matches(const TextureLayout& layout) const noexcept {
  const auto& d = layout.descriptor;
  return image_ && format_ == host_texture_format(layout.format.host_format) &&
         d.width == descriptor_.width && d.height == descriptor_.height &&
         d.depth == descriptor_.depth && d.dimension == descriptor_.dimension &&
         d.stacked == descriptor_.stacked && d.mip_max_level == descriptor_.mip_max_level;
}

bool TextureImage::initialize(VkPhysicalDevice physical_device, VkDevice device,
                              CommandQueue& queue, const DecodedTexture& texture) {
  if (!texture.valid) {
    reset();
    error_ = "Vulkan texture image requires decoded texture data and a device";
    return false;
  }
  if (!create(physical_device, device, texture.layout)) return false;
  Buffer staging;
  if (!staging.initialize(physical_device, device_, texture.linear_data.size(),
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = staging.error();
    reset();
    return false;
  }
  std::span<std::byte> mapped;
  if (!staging.map(mapped)) {
    error_ = staging.error();
    reset();
    return false;
  }
  std::memcpy(mapped.data(), texture.linear_data.data(), texture.linear_data.size());
  staging.unmap();
  std::vector<VkBufferImageCopy> copies;
  copies.reserve(texture.layout.subresources.size());
  for (const auto& sub : texture.layout.subresources) {
    VkBufferImageCopy copy{};
    copy.bufferOffset = sub.linear_offset_bytes;
    copy.imageSubresource.aspectMask = aspect_;
    copy.imageSubresource.mipLevel = sub.mip_level;
    copy.imageSubresource.baseArrayLayer = sub.array_layer;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent = {sub.width_texels, sub.height_texels, sub.depth_texels};
    copies.push_back(copy);
  }
  if (!queue.execute([&](VkCommandBuffer command) {
        VkImageMemoryBarrier2 to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_copy.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_copy.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_copy.image = image_;
        to_copy.subresourceRange = {aspect_, 0, mip_levels_, 0, layers_};
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
        to_shader.dstStageMask =
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_shader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDependencyInfo after{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        after.imageMemoryBarrierCount = 1;
        after.pImageMemoryBarriers = &to_shader;
        vkCmdPipelineBarrier2(command, &after);
      })) {
    error_ = queue.error();
    reset();
    return false;
  }
  layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  return true;
}

void TextureImage::reset() noexcept {
  if (device_ && sampler_) vkDestroySampler(device_, sampler_, nullptr);
  if (device_ && view_) vkDestroyImageView(device_, view_, nullptr);
  if (device_ && image_) vkDestroyImage(device_, image_, nullptr);
  if (device_ && memory_) vkFreeMemory(device_, memory_, nullptr);
  sampler_ = VK_NULL_HANDLE;
  view_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  format_ = VK_FORMAT_UNDEFINED;
  device_ = VK_NULL_HANDLE;
  layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  descriptor_ = {};
  mip_levels_ = layers_ = 0;
}

GpuTextureLoader::~GpuTextureLoader() { reset(); }

void GpuTextureLoader::reset() noexcept {
  transients_.clear();
  staging_.reset();
  upload_.reset();
  staging_ring_.reset(0);
  upload_ring_.reset(0);
  if (device_) {
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  }
  pipeline_ = VK_NULL_HANDLE;
  pool_ = VK_NULL_HANDLE;
  pipeline_layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  mirror_set_ = upload_set_ = VK_NULL_HANDLE;
  mirror_set_buffer_ = upload_set_buffer_ = VK_NULL_HANDLE;
  staging_copied_ = false;
  device_ = VK_NULL_HANDLE;
  physical_device_ = VK_NULL_HANDLE;
}

bool GpuTextureLoader::initialize(VkPhysicalDevice physical_device, VkDevice device) {
  if (pipeline_ && device == device_) return true;
#ifndef XENON_HAS_DXC
  (void)physical_device;
  (void)device;
  error_ = "Vulkan GPU texture loading requires DXC";
  return false;
#else
  reset();
  device_ = device;
  physical_device_ = physical_device;
  const std::array bindings{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  set_info.pBindings = bindings.data();
  const VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                      sizeof(GpuTextureLoadConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &constants;
  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       2u * (2u + kTransientSets)};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pool_info.maxSets = 2u + kTransientSets;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_) != VK_SUCCESS ||
      vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS ||
      vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
    error_ = "Vulkan texture load layout creation failed";
    reset();
    return false;
  }
  UtilityShaderCompiler compiler;
  ShaderCompileOptions options{};
  options.format = ShaderBinaryFormat::Spirv;
  const auto compiled = compiler.compile(make_gpu_texture_load_shader(), options);
  if (!compiled.succeeded) {
    error_ = compiled.diagnostics.empty() ? "Vulkan texture load shader compilation failed"
                                          : compiled.diagnostics.front();
    reset();
    return false;
  }
  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  module_info.codeSize = compiled.binary.size();
  module_info.pCode = reinterpret_cast<const std::uint32_t*>(compiled.binary.data());
  VkShaderModule module = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &module_info, nullptr, &module) != VK_SUCCESS) {
    error_ = "Vulkan texture load shader module creation failed";
    reset();
    return false;
  }
  VkComputePipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pipeline_info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                         VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
  pipeline_info.layout = pipeline_layout_;
  const auto created =
      vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline_);
  vkDestroyShaderModule(device, module, nullptr);
  if (created != VK_SUCCESS) {
    pipeline_ = VK_NULL_HANDLE;
    error_ = "Vulkan texture load pipeline creation failed";
    reset();
    return false;
  }
  if (!staging_.initialize(physical_device, device, kStagingSize,
                           VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ||
      !upload_.initialize(physical_device, device, kUploadSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = "Vulkan texture load staging allocation failed";
    reset();
    return false;
  }
  staging_ring_.reset(kStagingSize);
  upload_ring_.reset(kUploadSize);
  return true;
#endif
}

VkDescriptorSet GpuTextureLoader::source_set(VkDescriptorSet& set, VkBuffer& bound,
                                             VkBuffer source) {
  if (!set) {
    VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate.descriptorPool = pool_;
    allocate.descriptorSetCount = 1;
    allocate.pSetLayouts = &set_layout_;
    if (vkAllocateDescriptorSets(device_, &allocate, &set) != VK_SUCCESS) {
      set = VK_NULL_HANDLE;
      error_ = "Vulkan texture load descriptor set allocation failed";
      return VK_NULL_HANDLE;
    }
    bound = VK_NULL_HANDLE;
  }
  if (bound != source) {
    const std::array infos{VkDescriptorBufferInfo{source, 0, VK_WHOLE_SIZE},
                           VkDescriptorBufferInfo{staging_.buffer(), 0, VK_WHOLE_SIZE}};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (std::uint32_t i = 0; i < 2u; ++i) {
      writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
      writes[i].pBufferInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()), writes.data(), 0,
                           nullptr);
    bound = source;
  }
  return set;
}

void GpuTextureLoader::retire(std::uint64_t completed) {
  std::erase_if(transients_, [&](Transient& t) {
    if (t.value > completed) return false;
    if (t.set) vkFreeDescriptorSets(device_, pool_, 1, &t.set);
    return true;
  });
}

bool GpuTextureLoader::load_from_mirror(VkPhysicalDevice physical_device, VkDevice device,
                                        CommandQueue& queue, GuestMemoryMirror& mirror,
                                        TextureImage& image, const TextureLayout& layout) {
  if (!initialize(physical_device, device)) return false;
  const auto set = source_set(mirror_set_, mirror_set_buffer_, mirror.buffer());
  if (!set) return false;
  return run(queue, image, layout, set, 0u, 0u, memory::kPhysicalMemorySize, &mirror);
}

bool GpuTextureLoader::load_from_bytes(VkPhysicalDevice physical_device, VkDevice device,
                                       CommandQueue& queue, TextureImage& image,
                                       const TextureLayout& layout,
                                       std::span<const std::byte> bytes, std::uint32_t base) {
  if (!initialize(physical_device, device)) return false;
  if (bytes.empty() || bytes.size() > memory::kPhysicalMemorySize) {
    error_ = "Vulkan texture load has no guest bytes to read";
    return false;
  }
  retire(queue.completed_value());
  const auto size = (bytes.size() + 3u) & ~std::size_t{3u};
  if (size <= kUploadSize) {
    const auto reserved = upload_ring_.reserve(size, queue.completed_value(), [&](std::uint64_t v) {
      return queue.flush() && queue.wait_for_completion(v);
    });
    if (!reserved) {
      error_ = queue.error().empty() ? "Vulkan texture upload ring reservation failed"
                                     : queue.error();
      return false;
    }
    std::span<std::byte> mapped;
    if (!upload_.map(mapped)) {
      error_ = upload_.error();
      return false;
    }
    std::memcpy(mapped.data() + *reserved, bytes.data(), bytes.size());
    upload_ring_.commit(*reserved, size, queue.retirement_value());
    const auto set = source_set(upload_set_, upload_set_buffer_, upload_.buffer());
    if (!set) return false;
    return run(queue, image, layout, set, static_cast<std::uint32_t>(*reserved), base,
               static_cast<std::uint32_t>(*reserved + bytes.size()), nullptr);
  }
  // Too large for the ring: a buffer of its own, freed once the GPU read it.
  if (transients_.size() >= kTransientSets) {
    if (!(queue.flush() && queue.wait_for_completion(transients_.front().value))) {
      error_ = queue.error();
      return false;
    }
    retire(queue.completed_value());
  }
  Transient transient{};
  transient.buffer = std::make_unique<Buffer>();
  std::span<std::byte> mapped;
  if (!transient.buffer->initialize(physical_device_, device_,
                                    size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = transient.buffer->error();
    return false;
  }
  if (!transient.buffer->map(mapped)) {
    error_ = transient.buffer->error();
    return false;
  }
  std::memcpy(mapped.data(), bytes.data(), bytes.size());
  VkBuffer bound = VK_NULL_HANDLE;
  if (!source_set(transient.set, bound, transient.buffer->buffer())) return false;
  const auto set = transient.set;
  transient.value = queue.retirement_value();
  transients_.push_back(std::move(transient));
  return run(queue, image, layout, set, 0u, base, static_cast<std::uint32_t>(bytes.size()),
             nullptr);
}

bool GpuTextureLoader::run(CommandQueue& queue, TextureImage& image, const TextureLayout& layout,
                           VkDescriptorSet set, std::uint32_t source_bias,
                           std::uint32_t source_base, std::uint32_t source_limit,
                           GuestMemoryMirror* mirror) {
  // Destination rows hold host blocks (a widened texel is 16 bytes).
  const auto bpb = layout.format.host_bytes_per_block();
  const auto block_width = std::uint32_t(layout.format.block_width);
  const auto block_height = std::uint32_t(layout.format.block_height);
  // Buffer-to-image copies start on a multiple of the texel block size.
  const VkDeviceSize alignment = bpb == 12u ? 48u : 16u;
  const auto wait = [&](std::uint64_t value) {
    return queue.flush() && queue.wait_for_completion(value);
  };
  bool first = true;
  for (const auto& sub : layout.subresources) {
    if (sub.guest_address < source_base) {
      error_ = "Xenos texture lies outside the guest bytes it is loaded from";
      return false;
    }
    const auto row_pitch = gpu_texture_load_row_pitch(layout, sub, 4u);
    for (const auto& chunk : plan_gpu_texture_load_chunks(sub, row_pitch, kStagingSize / 2u)) {
      auto constants = make_gpu_texture_load_constants(layout, sub, chunk, row_pitch);
      const VkDeviceSize bytes = VkDeviceSize(row_pitch) * chunk.rows * chunk.slices;
      const auto reserved = staging_ring_.reserve(bytes + alignment, queue.completed_value(), wait);
      if (!reserved) {
        error_ = queue.error().empty() ? "Vulkan texture staging reservation failed"
                                       : queue.error();
        return false;
      }
      const auto offset = (*reserved + alignment - 1u) / alignment * alignment;
      constants.source_offset = sub.guest_address - source_base + source_bias;
      constants.source_limit = source_limit;
      constants.destination_offset = static_cast<std::uint32_t>(offset);
      const auto top = chunk.row_start * block_height;
      VkBufferImageCopy copy{};
      copy.bufferOffset = offset;
      copy.bufferRowLength = row_pitch / bpb * block_width;
      copy.bufferImageHeight = chunk.rows * block_height;
      copy.imageSubresource = {image.aspect(), sub.mip_level, sub.array_layer, 1u};
      const bool volume = layout.descriptor.dimension == TextureDimension::ThreeD;
      copy.imageOffset = {0, std::int32_t(top), volume ? std::int32_t(chunk.z_start) : 0};
      copy.imageExtent = {sub.width_texels,
                          (std::min)(chunk.rows * block_height, sub.height_texels - top),
                          chunk.slices};
      const bool to_transfer = first;
      const auto old_layout = image.layout();
      first = false;
      const bool staging_copied = staging_copied_;
      if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
            if (mirror) mirror->prepare_shader_access(command, false);
            std::array<VkBufferMemoryBarrier2, 1> buffer_barriers{};
            auto& staging_write = buffer_barriers[0];
            staging_write.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2;
            staging_write.srcStageMask =
                staging_copied ? VK_PIPELINE_STAGE_2_COPY_BIT : VK_PIPELINE_STAGE_2_NONE;
            staging_write.srcAccessMask = staging_copied ? VK_ACCESS_2_TRANSFER_READ_BIT : 0;
            staging_write.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            staging_write.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            staging_write.buffer = staging_.buffer();
            staging_write.offset = 0;
            staging_write.size = VK_WHOLE_SIZE;
            VkImageMemoryBarrier2 to_copy{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            to_copy.srcStageMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                       ? VK_PIPELINE_STAGE_2_NONE
                                       : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            to_copy.srcAccessMask =
                old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            to_copy.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            to_copy.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            to_copy.oldLayout = old_layout;
            to_copy.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_copy.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_copy.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            to_copy.image = image.image();
            to_copy.subresourceRange = {image.aspect(), 0, image.mip_levels(), 0, image.layers()};
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.bufferMemoryBarrierCount = 1;
            dependency.pBufferMemoryBarriers = buffer_barriers.data();
            dependency.imageMemoryBarrierCount = to_transfer ? 1u : 0u;
            dependency.pImageMemoryBarriers = &to_copy;
            vkCmdPipelineBarrier2(command, &dependency);
            vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_);
            vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout_, 0, 1,
                                    &set, 0, nullptr);
            vkCmdPushConstants(command, pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(constants), &constants);
            vkCmdDispatch(command, (constants.row_dwords + 7u) / 8u, (constants.rows + 7u) / 8u,
                          constants.slices);
            VkBufferMemoryBarrier2 staging_read{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            staging_read.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            staging_read.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
            staging_read.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            staging_read.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            staging_read.buffer = staging_.buffer();
            staging_read.offset = 0;
            staging_read.size = VK_WHOLE_SIZE;
            VkDependencyInfo before_copy{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            before_copy.bufferMemoryBarrierCount = 1;
            before_copy.pBufferMemoryBarriers = &staging_read;
            vkCmdPipelineBarrier2(command, &before_copy);
            vkCmdCopyBufferToImage(command, staging_.buffer(), image.image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
          })) {
        error_ = queue.error();
        return false;
      }
      staging_copied_ = true;
      image.set_layout(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
      staging_ring_.commit(*reserved, bytes + alignment, queue.retirement_value());
    }
  }
  if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
        VkImageMemoryBarrier2 to_shader{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        to_shader.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        to_shader.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        to_shader.dstStageMask =
            VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        to_shader.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_shader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_shader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_shader.image = image.image();
        to_shader.subresourceRange = {image.aspect(), 0, image.mip_levels(), 0, image.layers()};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &to_shader;
        vkCmdPipelineBarrier2(command, &dependency);
      })) {
    error_ = queue.error();
    return false;
  }
  image.set_layout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  return true;
}

}  // namespace xenon::gpu::vulkan
