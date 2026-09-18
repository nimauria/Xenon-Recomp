#include "xenon/gpu/vulkan/render_target.hpp"

#include <cstring>
#include <span>

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

std::uint32_t color_bytes_per_pixel(ColorRenderTargetFormat format) noexcept {
  return color_host_bytes_per_pixel(format);
}

#ifdef XENON_HAS_DXC
bool read_msaa_sample(VkPhysicalDevice physical_device, VkDevice device,
                      CommandQueue& queue, VkImage source,
                      VkImageLayout source_layout, VkImageView source_view,
                      VkFormat format, MsaaSamples samples,
                      std::uint32_t host_sample, std::uint32_t left,
                      std::uint32_t top, std::uint32_t width,
                      std::uint32_t height, std::uint32_t bytes_per_pixel,
                      std::vector<std::byte>& destination,
                      std::uint32_t& row_pitch, std::string& error) {
  DxcShaderCompiler compiler;
  ShaderCompileOptions options{};
  options.format = ShaderBinaryFormat::Spirv;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader(), options);
  const auto ps = compiler.compile(make_color_sample_read_shader(samples), options);
  if (!vs.succeeded || !ps.succeeded) {
    error = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
            : !ps.diagnostics.empty() ? ps.diagnostics.front()
                                      : "Vulkan sample transfer shader compilation failed";
    return false;
  }

  VkImage target = VK_NULL_HANDLE;
  VkDeviceMemory target_memory = VK_NULL_HANDLE;
  VkImageView target_view = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
  VkDescriptorPool descriptor_pool = VK_NULL_HANDLE;
  VkShaderModule vs_module = VK_NULL_HANDLE;
  VkShaderModule ps_module = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  auto cleanup = [&] {
    if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
    if (ps_module) vkDestroyShaderModule(device, ps_module, nullptr);
    if (vs_module) vkDestroyShaderModule(device, vs_module, nullptr);
    if (descriptor_pool) vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
    if (pipeline_layout) vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    if (set_layout) vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
    if (target_view) vkDestroyImageView(device, target_view, nullptr);
    if (target) vkDestroyImage(device, target, nullptr);
    if (target_memory) vkFreeMemory(device, target_memory, nullptr);
  };

  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format;
  image_info.extent = {width, height, 1};
  image_info.mipLevels = image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (vkCreateImage(device, &image_info, nullptr, &target) != VK_SUCCESS) {
    error = "Vulkan sample readback target creation failed"; cleanup(); return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device, target, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = find_memory_type(
      physical_device, requirements.memoryTypeBits,
      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (allocation.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device, &allocation, nullptr, &target_memory) != VK_SUCCESS ||
      vkBindImageMemory(device, target, target_memory, 0) != VK_SUCCESS) {
    error = "Vulkan sample readback target allocation failed"; cleanup(); return false;
  }
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = target; view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = format;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &view_info, nullptr, &target_view) != VK_SUCCESS) {
    error = "Vulkan sample readback view creation failed"; cleanup(); return false;
  }

  struct TransferConstants { std::uint32_t origin[2], sample, pad; };
  Buffer constants;
  if (!constants.initialize(physical_device, device, sizeof(TransferConstants),
                            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error = constants.error(); cleanup(); return false;
  }
  std::span<std::byte> mapped;
  if (!constants.map(mapped)) { error = constants.error(); cleanup(); return false; }
  const TransferConstants values{{left, top}, host_sample, 0};
  std::memcpy(mapped.data(), &values, sizeof(values)); constants.unmap();
  row_pitch = width * bytes_per_pixel;
  Buffer readback;
  if (!readback.initialize(physical_device, device,
                           VkDeviceSize(row_pitch) * height,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error = readback.error(); cleanup(); return false;
  }

  const std::array bindings{
      VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      VkDescriptorSetLayoutBinding{16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  VkDescriptorSetLayoutCreateInfo set_info{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = static_cast<std::uint32_t>(bindings.size());
  set_info.pBindings = bindings.data();
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout) != VK_SUCCESS) {
    error = "Vulkan sample transfer descriptor layout failed"; cleanup(); return false;
  }
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1; layout_info.pSetLayouts = &set_layout;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
    error = "Vulkan sample transfer pipeline layout failed"; cleanup(); return false;
  }
  const std::array pool_sizes{
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
      VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1; pool_info.poolSizeCount = 2;
  pool_info.pPoolSizes = pool_sizes.data();
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &descriptor_pool) != VK_SUCCESS) {
    error = "Vulkan sample transfer descriptor pool failed"; cleanup(); return false;
  }
  VkDescriptorSetAllocateInfo set_allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  set_allocate.descriptorPool = descriptor_pool; set_allocate.descriptorSetCount = 1;
  set_allocate.pSetLayouts = &set_layout;
  VkDescriptorSet descriptor_set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device, &set_allocate, &descriptor_set) != VK_SUCCESS) {
    error = "Vulkan sample transfer descriptor allocation failed"; cleanup(); return false;
  }
  VkDescriptorBufferInfo constant_info{constants.buffer(), 0, sizeof(TransferConstants)};
  VkDescriptorImageInfo source_info{};
  source_info.imageView = source_view;
  source_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  std::array<VkWriteDescriptorSet, 2> writes{};
  writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, 0,
               0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &constant_info, nullptr};
  writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descriptor_set, 16,
               0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &source_info, nullptr, nullptr};
  vkUpdateDescriptorSets(device, 2, writes.data(), 0, nullptr);

  auto create_module = [&](const CompiledShader& shader, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = shader.binary.size();
    info.pCode = reinterpret_cast<const std::uint32_t*>(shader.binary.data());
    return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS;
  };
  if (!create_module(vs, vs_module) || !create_module(ps, ps_module)) {
    error = "Vulkan sample transfer shader module creation failed"; cleanup(); return false;
  }
  const std::array stages{
      VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs_module, "main", nullptr},
      VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps_module, "main", nullptr}};
  VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  const VkViewport viewport{0, 0, float(width), float(height), 0, 1};
  const VkRect2D scissor{{0,0},{width,height}};
  VkPipelineViewportStateCreateInfo viewport_state{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport_state.viewportCount=1; viewport_state.pViewports=&viewport;
  viewport_state.scissorCount=1; viewport_state.pScissors=&scissor;
  VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  raster.polygonMode=VK_POLYGON_MODE_FILL; raster.cullMode=VK_CULL_MODE_NONE; raster.lineWidth=1;
  VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState attachment{}; attachment.colorWriteMask=0xF;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount=1; blend.pAttachments=&attachment;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount=1; rendering.pColorAttachmentFormats=&format;
  VkGraphicsPipelineCreateInfo pipeline_info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  pipeline_info.pNext=&rendering; pipeline_info.stageCount=2; pipeline_info.pStages=stages.data();
  pipeline_info.pVertexInputState=&vertex; pipeline_info.pInputAssemblyState=&assembly;
  pipeline_info.pViewportState=&viewport_state; pipeline_info.pRasterizationState=&raster;
  pipeline_info.pMultisampleState=&multisample; pipeline_info.pColorBlendState=&blend;
  pipeline_info.layout=pipeline_layout;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info,
                                nullptr, &pipeline) != VK_SUCCESS) {
    error = "Vulkan sample transfer pipeline creation failed"; cleanup(); return false;
  }

  const bool submitted = queue.execute([&](VkCommandBuffer command) {
    std::array<VkImageMemoryBarrier2,2> barriers{};
    barriers[0]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barriers[0].srcStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].srcAccessMask=VK_ACCESS_2_MEMORY_WRITE_BIT;
    barriers[0].dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barriers[0].dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barriers[0].oldLayout=source_layout;
    barriers[0].newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].image=source;
    barriers[0].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    barriers[1]={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barriers[1].dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[1].dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[1].oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[1].image=target;
    barriers[1].subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount=2; dependency.pImageMemoryBarriers=barriers.data();
    vkCmdPipelineBarrier2(command,&dependency);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView=target_view; color.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; color.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo render{VK_STRUCTURE_TYPE_RENDERING_INFO};
    render.renderArea.extent={width,height}; render.layerCount=1;
    render.colorAttachmentCount=1; render.pColorAttachments=&color;
    vkCmdBeginRendering(command,&render);
    vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline_layout,
                            0,1,&descriptor_set,0,nullptr);
    vkCmdDraw(command,3,1,0,0); vkCmdEndRendering(command);
    barriers[0].srcStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    barriers[0].srcAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    barriers[0].dstStageMask=VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    barriers[0].dstAccessMask=VK_ACCESS_2_MEMORY_READ_BIT|VK_ACCESS_2_MEMORY_WRITE_BIT;
    barriers[0].oldLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barriers[0].newLayout=source_layout;
    barriers[1].srcStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barriers[1].srcAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barriers[1].dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;
    barriers[1].dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
    barriers[1].oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barriers[1].newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier2(command,&dependency);
    VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
    copy.imageExtent={width,height,1};
    vkCmdCopyImageToBuffer(command,target,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer(),1,&copy);
    VkBufferMemoryBarrier2 host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT; host.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host.dstStageMask=VK_PIPELINE_STAGE_2_HOST_BIT; host.dstAccessMask=VK_ACCESS_2_HOST_READ_BIT;
    host.buffer=readback.buffer(); host.size=VK_WHOLE_SIZE;
    dependency.imageMemoryBarrierCount=0; dependency.pImageMemoryBarriers=nullptr;
    dependency.bufferMemoryBarrierCount=1; dependency.pBufferMemoryBarriers=&host;
    vkCmdPipelineBarrier2(command,&dependency);
  });
  if (!submitted) { error=queue.error(); cleanup(); return false; }
  if (!readback.map(mapped)) { error=readback.error(); cleanup(); return false; }
  destination.assign(mapped.begin(), mapped.begin()+std::size_t(row_pitch)*height);
  readback.unmap(); cleanup(); return true;
}
#endif

}  // namespace

VkFormat color_render_target_format(ColorRenderTargetFormat format) noexcept {
  switch (color_host_storage(format)) {
    case ColorHostStorage::R8G8B8A8Unorm: return VK_FORMAT_R8G8B8A8_UNORM;
    case ColorHostStorage::R16G16B16A16Unorm: return VK_FORMAT_R16G16B16A16_UNORM;
    case ColorHostStorage::R10G10B10A2Unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case ColorHostStorage::R16G16Float: return VK_FORMAT_R16G16_SFLOAT;
    case ColorHostStorage::R16G16B16A16Float:
      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case ColorHostStorage::R32Float: return VK_FORMAT_R32_SFLOAT;
    case ColorHostStorage::R32G32Float: return VK_FORMAT_R32G32_SFLOAT;
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
  guest_format_ = color_format;
  bytes_per_pixel_ = color_bytes_per_pixel(color_format);
  if (format_ == VK_FORMAT_UNDEFINED) {
    error_ = "Xenos color target format needs a non-native conversion path";
    return false;
  }
  device_ = device;
  physical_device_ = physical_device;
  mip_width_ = surface.pitch_pixels;
  mip_height_ = surface.height_pixels;
  surface_ = surface;
  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = format_;
  image_info.extent = {mip_width_, mip_height_, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = sample_count(surface.msaa);
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT |
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

bool RenderTargetImage::upload(CommandQueue& queue,
                               std::span<const std::byte> source,
                               std::uint32_t row_pitch) {
  if (!physical_device_ || !device_ || !image_ || !bytes_per_pixel_ ||
      surface_.msaa != MsaaSamples::X1 ||
      row_pitch < mip_width_ * bytes_per_pixel_ ||
      row_pitch % bytes_per_pixel_ ||
      std::uint64_t(row_pitch) * mip_height_ > source.size()) {
    error_ = "invalid Vulkan color-target ownership upload";
    return false;
  }
  Buffer staging;
  if (!staging.initialize(physical_device_, device_,
                          VkDeviceSize(row_pitch) * mip_height_,
                          VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = staging.error();
    return false;
  }
  std::span<std::byte> mapped;
  if (!staging.map(mapped)) {
    error_ = staging.error();
    return false;
  }
  std::memcpy(mapped.data(), source.data(),
              std::size_t(row_pitch) * mip_height_);
  staging.unmap();
  const auto old_layout = layout_;
  if (!queue.execute([&](VkCommandBuffer command) {
        VkImageMemoryBarrier2 before{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        before.srcStageMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                  ? VK_PIPELINE_STAGE_2_NONE
                                  : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        before.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                   ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
        before.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        before.oldLayout = old_layout;
        before.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        before.image = image_;
        before.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &before;
        vkCmdPipelineBarrier2(command, &dependency);
        VkBufferImageCopy copy{};
        copy.bufferRowLength = row_pitch / bytes_per_pixel_;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {mip_width_, mip_height_, 1};
        vkCmdCopyBufferToImage(command, staging.buffer(), image_,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        auto after = before;
        after.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
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

bool RenderTargetImage::readback(CommandQueue& queue, std::uint32_t left,
                                 std::uint32_t top, std::uint32_t right,
                                 std::uint32_t bottom,
                                 std::vector<std::byte>& destination,
                                 std::uint32_t& row_pitch) {
  destination.clear();
  row_pitch = 0;
  if (!physical_device_ || !device_ || !image_ || !bytes_per_pixel_ ||
      left >= right || top >= bottom || right > mip_width_ ||
      bottom > mip_height_) {
    error_ = "invalid Vulkan color-target readback rectangle";
    return false;
  }
  const auto copy_width = right - left;
  const auto copy_height = bottom - top;
  row_pitch = copy_width * bytes_per_pixel_;
  Buffer readback;
  if (!readback.initialize(physical_device_, device_,
                           VkDeviceSize(row_pitch) * copy_height,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = readback.error();
    return false;
  }

  VkImage resolved = VK_NULL_HANDLE;
  VkDeviceMemory resolved_memory = VK_NULL_HANDLE;
  if (surface_.msaa != MsaaSamples::X1) {
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format_;
    image_info.extent = {copy_width, copy_height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &image_info, nullptr, &resolved) != VK_SUCCESS) {
      error_ = "vkCreateImage failed for resolve readback";
      return false;
    }
    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, resolved, &requirements);
    const auto memory_type = find_memory_type(
        physical_device_, requirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = memory_type;
    if (memory_type == UINT32_MAX ||
        vkAllocateMemory(device_, &allocation, nullptr, &resolved_memory) !=
            VK_SUCCESS ||
        vkBindImageMemory(device_, resolved, resolved_memory, 0) != VK_SUCCESS) {
      if (resolved_memory) vkFreeMemory(device_, resolved_memory, nullptr);
      vkDestroyImage(device_, resolved, nullptr);
      error_ = "Vulkan resolve readback image allocation failed";
      return false;
    }
  }

  const auto old_layout = layout_;
  const bool submitted = queue.execute([&](VkCommandBuffer command) {
    VkImageMemoryBarrier2 source_barrier{
        VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    source_barrier.srcStageMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                      ? VK_PIPELINE_STAGE_2_NONE
                                      : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    source_barrier.srcAccessMask = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                       ? 0 : VK_ACCESS_2_MEMORY_WRITE_BIT;
    source_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    source_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    source_barrier.oldLayout = old_layout;
    source_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    source_barrier.image = image_;
    source_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &source_barrier;
    vkCmdPipelineBarrier2(command, &dependency);

    VkImage copy_source = image_;
    VkOffset3D copy_offset{static_cast<std::int32_t>(left),
                           static_cast<std::int32_t>(top), 0};
    if (resolved) {
      VkImageMemoryBarrier2 resolved_barrier{
          VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
      resolved_barrier.dstStageMask = VK_PIPELINE_STAGE_2_RESOLVE_BIT;
      resolved_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      resolved_barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      resolved_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      resolved_barrier.image = resolved;
      resolved_barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      dependency.pImageMemoryBarriers = &resolved_barrier;
      vkCmdPipelineBarrier2(command, &dependency);
      VkImageResolve region{};
      region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.srcOffset = copy_offset;
      region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.extent = {copy_width, copy_height, 1};
      vkCmdResolveImage(command, image_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        resolved, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                        &region);
      resolved_barrier.srcStageMask = VK_PIPELINE_STAGE_2_RESOLVE_BIT;
      resolved_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
      resolved_barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
      resolved_barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
      resolved_barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      resolved_barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      dependency.pImageMemoryBarriers = &resolved_barrier;
      vkCmdPipelineBarrier2(command, &dependency);
      copy_source = resolved;
      copy_offset = {};
    }
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageOffset = copy_offset;
    copy.imageExtent = {copy_width, copy_height, 1};
    vkCmdCopyImageToBuffer(command, copy_source,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer(), 1, &copy);
    VkBufferMemoryBarrier2 host_barrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    host_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    host_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    host_barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    host_barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    host_barrier.buffer = readback.buffer();
    host_barrier.size = VK_WHOLE_SIZE;
    dependency.imageMemoryBarrierCount = 0;
    dependency.pImageMemoryBarriers = nullptr;
    dependency.bufferMemoryBarrierCount = 1;
    dependency.pBufferMemoryBarriers = &host_barrier;
    vkCmdPipelineBarrier2(command, &dependency);
    std::swap(source_barrier.oldLayout, source_barrier.newLayout);
    source_barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    source_barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    source_barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    source_barrier.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT |
                                   VK_ACCESS_2_MEMORY_WRITE_BIT;
    dependency.bufferMemoryBarrierCount = 0;
    dependency.pBufferMemoryBarriers = nullptr;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &source_barrier;
    vkCmdPipelineBarrier2(command, &dependency);
  });
  if (resolved_memory) vkFreeMemory(device_, resolved_memory, nullptr);
  if (resolved) vkDestroyImage(device_, resolved, nullptr);
  if (!submitted) {
    error_ = queue.error();
    return false;
  }
  std::span<std::byte> mapped;
  if (!readback.map(mapped)) {
    error_ = readback.error();
    return false;
  }
  destination.assign(mapped.begin(), mapped.end());
  readback.unmap();
  return true;
}

bool RenderTargetImage::readback_sample(
    CommandQueue& queue, std::uint32_t guest_sample, std::uint32_t left,
    std::uint32_t top, std::uint32_t right, std::uint32_t bottom,
    std::vector<std::byte>& destination, std::uint32_t& row_pitch) {
  const auto mapping = map_guest_sample_to_host(surface_.msaa, guest_sample);
  if (!mapping || left >= right || top >= bottom || right > mip_width_ ||
      bottom > mip_height_) {
    error_ = "invalid Vulkan selected-sample readback";
    return false;
  }
  if (surface_.msaa == MsaaSamples::X1)
    return readback(queue,left,top,right,bottom,destination,row_pitch);
#ifdef XENON_HAS_DXC
  return read_msaa_sample(physical_device_,device_,queue,image_,layout_,view_,
                          format_,surface_.msaa,mapping->sample,left,top,
                          right-left,bottom-top,bytes_per_pixel_,destination,
                          row_pitch,error_);
#else
  error_ = "Vulkan selected-sample readback requires DXC";
  return false;
#endif
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
  guest_format_ = ColorRenderTargetFormat::R8G8B8A8;
  physical_device_ = VK_NULL_HANDLE;
  mip_width_ = mip_height_ = bytes_per_pixel_ = 0;
  surface_ = {};
  layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

}  // namespace xenon::gpu::vulkan
