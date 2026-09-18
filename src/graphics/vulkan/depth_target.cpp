#include "xenon/gpu/vulkan/depth_target.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "xenon/gpu/depth_format.hpp"
#include "xenon/gpu/vulkan/memory.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
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

VkSampleCountFlagBits sample_count(MsaaSamples samples) {
  switch (samples) {
    case MsaaSamples::X1: return VK_SAMPLE_COUNT_1_BIT;
    case MsaaSamples::X2: return VK_SAMPLE_COUNT_2_BIT;
    case MsaaSamples::X4: return VK_SAMPLE_COUNT_4_BIT;
  }
  return VK_SAMPLE_COUNT_1_BIT;
}

bool create_transfer_image(VkPhysicalDevice physical_device, VkDevice device,
                           VkFormat format, std::uint32_t width,
                           std::uint32_t height, VkImageUsageFlags usage,
                           VkImageAspectFlags aspect, VkImage& image,
                           VkDeviceMemory& memory, VkImageView& view) {
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D; info.format = format;
  info.extent = {width, height, 1}; info.mipLevels = info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT; info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = usage;
  if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) return false;
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device, image, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = find_memory_type(
      physical_device, requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (allocation.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
      vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS) return false;
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image; view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {aspect, 0, 1, 0, 1};
  return vkCreateImageView(device, &view_info, nullptr, &view) == VK_SUCCESS;
}

bool supports_stencil_export(VkPhysicalDevice device) {
  std::uint32_t count{};
  if (vkEnumerateDeviceExtensionProperties(device,nullptr,&count,nullptr)!=VK_SUCCESS)
    return false;
  std::vector<VkExtensionProperties> extensions(count);
  if (vkEnumerateDeviceExtensionProperties(device,nullptr,&count,extensions.data())!=VK_SUCCESS)
    return false;
  return std::any_of(extensions.begin(),extensions.end(),[](const auto& extension){
    return std::strcmp(extension.extensionName,
                       VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME)==0;
  });
}

#ifdef XENON_HAS_DXC
bool create_module(VkDevice device, const CompiledShader& shader,
                   VkShaderModule& module) {
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = shader.binary.size();
  info.pCode = reinterpret_cast<const std::uint32_t*>(shader.binary.data());
  return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS;
}
#endif

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
                                  DepthRenderTargetFormat depth_format,
                                  bool native_2x_supported) {
  reset();
  error_.clear();
  if (!physical_device || !device || !surface.valid() || !surface.depth) {
    error_ = "Vulkan depth target requires a valid depth EDRAM surface";
    return false;
  }
  device_ = device;
  physical_device_ = physical_device;
  format_ = depth_render_target_format(depth_format);
  guest_format_ = depth_format;
  width_ = surface.pitch_pixels;
  height_ = surface.height_pixels;
  surface_ = surface;
  host_msaa_ = surface.msaa;
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
  if (surface.msaa == MsaaSamples::X2) {
    VkImageFormatProperties image_properties{};
    const auto query = vkGetPhysicalDeviceImageFormatProperties(
        physical_device, format_, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
            VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        0, &image_properties);
    const bool native_2x = query == VK_SUCCESS &&
        (image_properties.sampleCounts & VK_SAMPLE_COUNT_2_BIT);
    if (!native_2x_supported || !native_2x) host_msaa_ = MsaaSamples::X4;
  }
  info.samples = sample_count(host_msaa_);
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT |
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
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  if (vkCreateImageView(device_, &view, nullptr, &depth_view_) != VK_SUCCESS) {
    error_ = "vkCreateImageView failed for depth sampling";
    reset(); return false;
  }
  view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
  // D3D depth-stencil SRVs expose stencil in the green component. Swizzle the
  // Vulkan stencil aspect to the same shared transfer-shader ABI.
  view.components.g = VK_COMPONENT_SWIZZLE_R;
  view.components.r = VK_COMPONENT_SWIZZLE_ZERO;
  if (vkCreateImageView(device_, &view, nullptr, &stencil_view_) != VK_SUCCESS) {
    error_ = "vkCreateImageView failed for stencil sampling";
    reset(); return false;
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

bool DepthTargetImage::readback_sample(
    CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::vector<std::uint32_t>& destination, std::uint32_t& row_pitch) {
  const auto mapping = map_guest_sample_to_host(
      surface_.msaa, guest_sample, host_msaa_ == MsaaSamples::X2);
  if (!mapping) {
    destination.clear();
    row_pitch = 0;
    error_ = "invalid Vulkan selected depth-sample readback";
    return false;
  }
  return readback_native_sample(queue, mapping->sample, left, top, right,
                                bottom, destination, row_pitch);
}

bool DepthTargetImage::readback_native_sample(
    CommandQueue& queue, std::uint32_t host_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::vector<std::uint32_t>& destination, std::uint32_t& row_pitch) {
  destination.clear(); row_pitch = 0;
  const auto host_sample_count = 1u << static_cast<unsigned>(host_msaa_);
  if (host_sample >= host_sample_count || !image_ || left >= right ||
      top >= bottom || right > width_ || bottom > height_) {
    error_ = "invalid Vulkan native depth-sample readback"; return false;
  }
#ifndef XENON_HAS_DXC
  error_ = "Vulkan native depth-sample readback requires DXC"; return false;
#else
  const auto copy_width = right - left, copy_height = bottom - top;
  row_pitch = copy_width * 4u;
  DxcShaderCompiler compiler;
  ShaderCompileOptions options{}; options.format = ShaderBinaryFormat::Spirv;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader(), options);
  const auto ps = compiler.compile(make_depth_sample_read_shader(host_msaa_), options);
  if (!vs.succeeded || !ps.succeeded) {
    error_ = "Vulkan depth readback shader compilation failed"; return false;
  }
  VkImage depth_target{}, stencil_target{}; VkDeviceMemory depth_memory{}, stencil_memory{};
  VkImageView depth_target_view{}, stencil_target_view{};
  VkDescriptorSetLayout set_layout{}; VkPipelineLayout pipeline_layout{};
  VkDescriptorPool descriptor_pool{}; VkShaderModule vs_module{}, ps_module{};
  VkPipeline pipeline{};
  auto cleanup = [&] {
    if (pipeline) vkDestroyPipeline(device_, pipeline, nullptr);
    if (ps_module) vkDestroyShaderModule(device_, ps_module, nullptr);
    if (vs_module) vkDestroyShaderModule(device_, vs_module, nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(device_, descriptor_pool, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device_, pipeline_layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(device_, set_layout, nullptr);
    if (stencil_target_view) vkDestroyImageView(device_, stencil_target_view, nullptr);
    if (depth_target_view) vkDestroyImageView(device_, depth_target_view, nullptr);
    if (stencil_target) vkDestroyImage(device_, stencil_target, nullptr);
    if (depth_target) vkDestroyImage(device_, depth_target, nullptr);
    if (stencil_memory) vkFreeMemory(device_, stencil_memory, nullptr);
    if (depth_memory) vkFreeMemory(device_, depth_memory, nullptr);
  };
  const auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (!create_transfer_image(physical_device_, device_, VK_FORMAT_R32_SFLOAT,
          copy_width, copy_height, usage, VK_IMAGE_ASPECT_COLOR_BIT,
          depth_target, depth_memory, depth_target_view) ||
      !create_transfer_image(physical_device_, device_, VK_FORMAT_R8_UINT,
          copy_width, copy_height, usage, VK_IMAGE_ASPECT_COLOR_BIT,
          stencil_target, stencil_memory, stencil_target_view)) {
    error_ = "Vulkan depth readback targets could not be created"; cleanup(); return false;
  }
  struct Constants { std::uint32_t origin[2], sample, pad; };
  Buffer constants, depth_readback, stencil_readback;
  if (!constants.initialize(physical_device_, device_, sizeof(Constants),
          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
      !depth_readback.initialize(physical_device_, device_,
          std::uint64_t(row_pitch) * copy_height, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
      !stencil_readback.initialize(physical_device_, device_,
          std::uint64_t(copy_width) * copy_height, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = "Vulkan depth readback buffers could not be created"; cleanup(); return false;
  }
  std::span<std::byte> mapped;
  if (!constants.map(mapped)) { error_=constants.error(); cleanup(); return false; }
  const Constants values{{left,top},host_sample,0};
  std::memcpy(mapped.data(),&values,sizeof(values)); constants.unmap();
  const std::array bindings{
      VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
      VkDescriptorSetLayoutBinding{16,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},
      VkDescriptorSetLayoutBinding{17,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount=3; set_info.pBindings=bindings.data();
  if (vkCreateDescriptorSetLayout(device_,&set_info,nullptr,&set_layout)!=VK_SUCCESS) {
    error_="Vulkan depth readback descriptor layout failed"; cleanup(); return false;
  }
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount=1; layout_info.pSetLayouts=&set_layout;
  if (vkCreatePipelineLayout(device_,&layout_info,nullptr,&pipeline_layout)!=VK_SUCCESS) {
    error_="Vulkan depth readback pipeline layout failed"; cleanup(); return false;
  }
  const std::array pool_sizes{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets=1; pool_info.poolSizeCount=2; pool_info.pPoolSizes=pool_sizes.data();
  if (vkCreateDescriptorPool(device_,&pool_info,nullptr,&descriptor_pool)!=VK_SUCCESS) {
    error_="Vulkan depth readback descriptor pool failed"; cleanup(); return false;
  }
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool=descriptor_pool; allocate.descriptorSetCount=1; allocate.pSetLayouts=&set_layout;
  VkDescriptorSet set{};
  if (vkAllocateDescriptorSets(device_,&allocate,&set)!=VK_SUCCESS) { cleanup(); return false; }
  VkDescriptorBufferInfo cb{constants.buffer(),0,sizeof(Constants)};
  VkDescriptorImageInfo depth_info{}; depth_info.imageView=depth_view_;
  depth_info.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  VkDescriptorImageInfo stencil_info=depth_info; stencil_info.imageView=stencil_view_;
  std::array<VkWriteDescriptorSet,3> writes{};
  writes[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,0,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,nullptr,&cb,nullptr};
  writes[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,16,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,&depth_info,nullptr,nullptr};
  writes[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,17,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,&stencil_info,nullptr,nullptr};
  vkUpdateDescriptorSets(device_,3,writes.data(),0,nullptr);
  if (!create_module(device_,vs,vs_module)||!create_module(device_,ps,ps_module)) { cleanup(); return false; }
  const std::array stages{
      VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs_module,"main",nullptr},
      VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,ps_module,"main",nullptr}};
  VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  const VkViewport viewport{0,0,float(copy_width),float(copy_height),0,1}; const VkRect2D scissor{{0,0},{copy_width,copy_height}};
  VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; viewport_state.viewportCount=1;viewport_state.pViewports=&viewport;viewport_state.scissorCount=1;viewport_state.pScissors=&scissor;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  std::array<VkPipelineColorBlendAttachmentState,2> attachments{};attachments[0].colorWriteMask=0xF;attachments[1].colorWriteMask=0xF;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};blend.attachmentCount=2;blend.pAttachments=attachments.data();
  const std::array formats{VK_FORMAT_R32_SFLOAT,VK_FORMAT_R8_UINT};
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.colorAttachmentCount=2;rendering.pColorAttachmentFormats=formats.data();
  VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.pNext=&rendering;pi.stageCount=2;pi.pStages=stages.data();pi.pVertexInputState=&vertex;pi.pInputAssemblyState=&assembly;pi.pViewportState=&viewport_state;pi.pRasterizationState=&raster;pi.pMultisampleState=&multisample;pi.pColorBlendState=&blend;pi.layout=pipeline_layout;
  if(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline)!=VK_SUCCESS){error_="Vulkan depth readback pipeline failed";cleanup();return false;}
  const auto old_layout=layout_;
  const bool submitted=queue.execute([&](VkCommandBuffer command){
    std::array<VkImageMemoryBarrier2,3> b{};
    for(auto& x:b)x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    b[0].srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b[0].srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT;b[0].dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;b[0].dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;b[0].oldLayout=old_layout;b[0].newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;b[0].image=image_;b[0].subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    for(int i=1;i<3;++i){b[i].dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;b[i].dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;b[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b[i].newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b[i].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    b[1].image=depth_target;b[2].image=stencil_target;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=3;dep.pImageMemoryBarriers=b.data();vkCmdPipelineBarrier2(command,&dep);
    std::array<VkRenderingAttachmentInfo,2> ca{};for(auto& a:ca){a.sType=VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;a.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;a.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;a.storeOp=VK_ATTACHMENT_STORE_OP_STORE;}ca[0].imageView=depth_target_view;ca[1].imageView=stencil_target_view;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea=scissor;ri.layerCount=1;ri.colorAttachmentCount=2;ri.pColorAttachments=ca.data();vkCmdBeginRendering(command,&ri);vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&set,0,nullptr);vkCmdDraw(command,3,1,0,0);vkCmdEndRendering(command);
    for(int i=1;i<3;++i){b[i].srcStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;b[i].srcAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;b[i].dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;b[i].dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;b[i].oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b[i].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;}
    b[0].srcStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;b[0].srcAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;b[0].dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b[0].dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;b[0].oldLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;b[0].newLayout=old_layout;vkCmdPipelineBarrier2(command,&dep);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={copy_width,copy_height,1};vkCmdCopyImageToBuffer(command,depth_target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,depth_readback.buffer(),1,&copy);vkCmdCopyImageToBuffer(command,stencil_target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,stencil_readback.buffer(),1,&copy);
  });
  if(!submitted){error_=queue.error();cleanup();return false;}
  std::span<std::byte> db,sb;if(!depth_readback.map(db)||!stencil_readback.map(sb)){cleanup();return false;}
  destination.resize(std::size_t(copy_width)*copy_height);
  for(std::size_t i=0;i<destination.size();++i){float d{};std::memcpy(&d,db.data()+i*4,4);destination[i]=host_to_depth_stencil(guest_format_,d,std::to_integer<std::uint8_t>(sb[i]));}
  depth_readback.unmap();stencil_readback.unmap();cleanup();return true;
#endif
}

bool DepthTargetImage::upload_sample(
    CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::span<const std::uint32_t> source, std::uint32_t row_pitch) {
  const auto mapping=map_guest_sample_to_host(surface_.msaa,guest_sample,
                                               host_msaa_==MsaaSamples::X2);
  const auto w=right>left?right-left:0,h=bottom>top?bottom-top:0;
  if(!mapping||!image_||!w||!h||right>width_||bottom>height_||
     row_pitch<w*4u||row_pitch%4u||std::uint64_t(row_pitch)*h>source.size_bytes()){
    error_="invalid Vulkan selected depth-sample upload";return false;
  }
#ifndef XENON_HAS_DXC
  error_="Vulkan selected depth-sample upload requires DXC";return false;
#else
  DxcShaderCompiler compiler;ShaderCompileOptions options{};
  options.format=ShaderBinaryFormat::Spirv;
  const auto vs=compiler.compile(make_transfer_fullscreen_vertex_shader(),options);
  const auto ps=compiler.compile(make_depth_only_sample_write_shader(),options);
  const auto stencil_ps=compiler.compile(make_stencil_mask_write_shader(),options);
  if(!vs.succeeded||!ps.succeeded||!stencil_ps.succeeded){error_="Vulkan depth upload shader compilation failed";return false;}
  VkImage depth_source{},stencil_source{};VkDeviceMemory depth_memory{},stencil_memory{};
  VkImageView depth_source_view{},stencil_source_view{};VkDescriptorSetLayout set_layout{};
  VkPipelineLayout pipeline_layout{};VkDescriptorPool descriptor_pool{};
  VkShaderModule vs_module{},ps_module{},stencil_module{};
  VkPipeline pipeline{},stencil_pipeline{};
  auto cleanup=[&]{if(stencil_pipeline)vkDestroyPipeline(device_,stencil_pipeline,nullptr);if(pipeline)vkDestroyPipeline(device_,pipeline,nullptr);if(stencil_module)vkDestroyShaderModule(device_,stencil_module,nullptr);if(ps_module)vkDestroyShaderModule(device_,ps_module,nullptr);if(vs_module)vkDestroyShaderModule(device_,vs_module,nullptr);if(descriptor_pool)vkDestroyDescriptorPool(device_,descriptor_pool,nullptr);if(pipeline_layout)vkDestroyPipelineLayout(device_,pipeline_layout,nullptr);if(set_layout)vkDestroyDescriptorSetLayout(device_,set_layout,nullptr);if(stencil_source_view)vkDestroyImageView(device_,stencil_source_view,nullptr);if(depth_source_view)vkDestroyImageView(device_,depth_source_view,nullptr);if(stencil_source)vkDestroyImage(device_,stencil_source,nullptr);if(depth_source)vkDestroyImage(device_,depth_source,nullptr);if(stencil_memory)vkFreeMemory(device_,stencil_memory,nullptr);if(depth_memory)vkFreeMemory(device_,depth_memory,nullptr);};
  const auto usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
  if(!create_transfer_image(physical_device_,device_,VK_FORMAT_R32_SFLOAT,w,h,usage,VK_IMAGE_ASPECT_COLOR_BIT,depth_source,depth_memory,depth_source_view)||!create_transfer_image(physical_device_,device_,VK_FORMAT_R8_UINT,w,h,usage,VK_IMAGE_ASPECT_COLOR_BIT,stencil_source,stencil_memory,stencil_source_view)){error_="Vulkan depth upload images could not be created";cleanup();return false;}
  Buffer depth_upload,stencil_upload,constants;
  VkPhysicalDeviceProperties properties{};vkGetPhysicalDeviceProperties(physical_device_,&properties);const auto alignment=(std::max)(VkDeviceSize(16),properties.limits.minUniformBufferOffsetAlignment);const auto constant_stride=(VkDeviceSize(16)+alignment-1)/alignment*alignment;
  if(!depth_upload.initialize(physical_device_,device_,std::uint64_t(w)*h*4,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)||!stencil_upload.initialize(physical_device_,device_,std::uint64_t(w)*h,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)||!constants.initialize(physical_device_,device_,constant_stride*257,VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){error_="Vulkan depth upload buffers could not be created";cleanup();return false;}
  std::span<std::byte> db,sb,cb;if(!depth_upload.map(db)||!stencil_upload.map(sb)||!constants.map(cb)){cleanup();return false;}
  for(std::uint32_t y=0;y<h;++y)for(std::uint32_t x=0;x<w;++x){const auto packed=source[(std::uint64_t(y)*row_pitch)/4+x];const auto value=depth_stencil_to_host(guest_format_,packed);std::memcpy(db.data()+(std::size_t(y)*w+x)*4,&value.depth,4);sb[std::size_t(y)*w+x]=std::byte(value.stencil);}
  struct Constants{std::uint32_t origin[2],sample,pad;};const Constants values{{left,top},mapping->sample,0};std::memcpy(cb.data(),&values,16);for(std::uint32_t value=0;value<256;++value){const Constants sv{{left,top},mapping->sample,value};std::memcpy(cb.data()+constant_stride*(value+1),&sv,16);}depth_upload.unmap();stencil_upload.unmap();constants.unmap();
  const std::array bindings{VkDescriptorSetLayoutBinding{0,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},VkDescriptorSetLayoutBinding{16,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr},VkDescriptorSetLayoutBinding{17,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr}};
  VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};si.bindingCount=3;si.pBindings=bindings.data();if(vkCreateDescriptorSetLayout(device_,&si,nullptr,&set_layout)!=VK_SUCCESS){cleanup();return false;}
  VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};li.setLayoutCount=1;li.pSetLayouts=&set_layout;if(vkCreatePipelineLayout(device_,&li,nullptr,&pipeline_layout)!=VK_SUCCESS){cleanup();return false;}
  const std::array sizes{VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,1},VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2}};VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dpi.maxSets=1;dpi.poolSizeCount=2;dpi.pPoolSizes=sizes.data();if(vkCreateDescriptorPool(device_,&dpi,nullptr,&descriptor_pool)!=VK_SUCCESS){cleanup();return false;}
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptor_pool;ai.descriptorSetCount=1;ai.pSetLayouts=&set_layout;VkDescriptorSet set{};if(vkAllocateDescriptorSets(device_,&ai,&set)!=VK_SUCCESS){cleanup();return false;}
  VkDescriptorBufferInfo cbi{constants.buffer(),0,16};VkDescriptorImageInfo di{};di.imageView=depth_source_view;di.imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;VkDescriptorImageInfo sti=di;sti.imageView=stencil_source_view;std::array<VkWriteDescriptorSet,3>writes{};writes[0]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,0,0,1,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,nullptr,&cbi,nullptr};writes[1]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,16,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,&di,nullptr,nullptr};writes[2]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,nullptr,set,17,0,1,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,&sti,nullptr,nullptr};vkUpdateDescriptorSets(device_,3,writes.data(),0,nullptr);
  if(!create_module(device_,vs,vs_module)||!create_module(device_,ps,ps_module)||!create_module(device_,stencil_ps,stencil_module)){cleanup();return false;}
  std::array stages{VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_VERTEX_BIT,vs_module,"main",nullptr},VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_FRAGMENT_BIT,ps_module,"main",nullptr}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};ia.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;const VkViewport viewport{float(left),float(top),float(w),float(h),0,1};const VkRect2D scissor{{static_cast<std::int32_t>(left),static_cast<std::int32_t>(top)},{w,h}};VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};rs.polygonMode=VK_POLYGON_MODE_FILL;rs.cullMode=VK_CULL_MODE_NONE;rs.lineWidth=1;VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=sample_count(host_msaa_);ms.sampleShadingEnable=VK_TRUE;ms.minSampleShading=1;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};ds.depthTestEnable=VK_TRUE;ds.depthWriteEnable=VK_TRUE;ds.depthCompareOp=VK_COMPARE_OP_ALWAYS;ds.stencilTestEnable=VK_FALSE;ds.front.compareOp=VK_COMPARE_OP_ALWAYS;ds.front.passOp=VK_STENCIL_OP_REPLACE;ds.front.failOp=ds.front.depthFailOp=VK_STENCIL_OP_KEEP;ds.front.compareMask=ds.front.writeMask=0xFF;ds.back=ds.front;
  const VkDynamicState dynamic_state=VK_DYNAMIC_STATE_STENCIL_REFERENCE;VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};dynamic.dynamicStateCount=1;dynamic.pDynamicStates=&dynamic_state;VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};rendering.depthAttachmentFormat=format_;rendering.stencilAttachmentFormat=format_;VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pi.pNext=&rendering;pi.stageCount=2;pi.pStages=stages.data();pi.pVertexInputState=&vi;pi.pInputAssemblyState=&ia;pi.pViewportState=&vp;pi.pRasterizationState=&rs;pi.pMultisampleState=&ms;pi.pDepthStencilState=&ds;pi.pColorBlendState=&blend;pi.pDynamicState=&dynamic;pi.layout=pipeline_layout;if(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&pi,nullptr,&pipeline)!=VK_SUCCESS){error_="Vulkan depth upload pipeline failed";cleanup();return false;}stages[1].module=stencil_module;ds.depthWriteEnable=VK_FALSE;ds.stencilTestEnable=VK_TRUE;if(vkCreateGraphicsPipelines(device_,VK_NULL_HANDLE,1,&pi,nullptr,&stencil_pipeline)!=VK_SUCCESS){error_="Vulkan stencil upload pipeline failed";cleanup();return false;}
  const auto old_layout=layout_;
  const bool submitted=queue.execute([&](VkCommandBuffer command){
    std::array<VkImageMemoryBarrier2,3>b{};
    for(auto&x:b)x.sType=VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    for(int i=0;i<2;++i){b[i].dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;b[i].dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;b[i].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b[i].newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b[i].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};}
    b[0].image=depth_source;b[1].image=stencil_source;b[2].srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;b[2].srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT;b[2].dstStageMask=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;b[2].dstAccessMask=VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;b[2].oldLayout=old_layout;b[2].newLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;b[2].image=image_;b[2].subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT|VK_IMAGE_ASPECT_STENCIL_BIT,0,1,0,1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};dep.imageMemoryBarrierCount=3;dep.pImageMemoryBarriers=b.data();vkCmdPipelineBarrier2(command,&dep);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={w,h,1};vkCmdCopyBufferToImage(command,depth_upload.buffer(),depth_source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);vkCmdCopyBufferToImage(command,stencil_upload.buffer(),stencil_source,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    for(int i=0;i<2;++i){b[i].srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;b[i].srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;b[i].dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;b[i].dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;b[i].oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b[i].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
    dep.imageMemoryBarrierCount=2;vkCmdPipelineBarrier2(command,&dep);
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};da.imageView=view_;da.imageLayout=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;da.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;da.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};ri.renderArea=scissor;ri.layerCount=1;ri.pDepthAttachment=&da;ri.pStencilAttachment=&da;vkCmdBeginRendering(command,&ri);
    const std::uint32_t depth_offset=0;
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&set,1,&depth_offset);vkCmdDraw(command,3,1,0,0);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,stencil_pipeline);
    for(std::uint32_t value=0;value<256;++value){const auto stencil_offset=static_cast<std::uint32_t>(constant_stride*(value+1));vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,0,1,&set,1,&stencil_offset);vkCmdSetStencilReference(command,VK_STENCIL_FACE_FRONT_AND_BACK,value);vkCmdDraw(command,3,1,0,0);}
    vkCmdEndRendering(command);
  });
  if(!submitted){error_=queue.error();cleanup();return false;}layout_=VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;cleanup();return true;
#endif
}

void DepthTargetImage::reset() noexcept {
  if (device_ && stencil_view_) vkDestroyImageView(device_, stencil_view_, nullptr);
  if (device_ && depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
  if (device_ && view_) vkDestroyImageView(device_, view_, nullptr);
  if (device_ && image_) vkDestroyImage(device_, image_, nullptr);
  if (device_ && memory_) vkFreeMemory(device_, memory_, nullptr);
  view_ = VK_NULL_HANDLE;
  depth_view_ = stencil_view_ = VK_NULL_HANDLE;
  image_ = VK_NULL_HANDLE;
  memory_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
  physical_device_ = VK_NULL_HANDLE;
  format_ = VK_FORMAT_UNDEFINED;
  layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
  float24_ = false;
  guest_format_ = DepthRenderTargetFormat::D24S8;
  host_msaa_ = MsaaSamples::X1;
  width_ = height_ = 0;
  surface_ = {};
}

}  // namespace xenon::gpu::vulkan
