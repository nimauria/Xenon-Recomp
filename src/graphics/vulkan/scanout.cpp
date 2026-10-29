#include "xenon/gpu/vulkan/scanout.hpp"

#include <array>
#include <cstring>

#include "xenon/gpu/vulkan/memory.hpp"

#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::vulkan {
namespace {

// t0 with DXC's SPIR-V register shift for shader resources.
constexpr std::uint32_t kGuestBinding = 16u;

std::uint32_t find_memory_type(VkPhysicalDevice physical_device, std::uint32_t type_bits,
                               VkMemoryPropertyFlags properties) {
  VkPhysicalDeviceMemoryProperties memory{};
  vkGetPhysicalDeviceMemoryProperties(physical_device, &memory);
  for (std::uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) &&
        (memory.memoryTypes[i].propertyFlags & properties) == properties)
      return i;
  }
  return UINT32_MAX;
}

}  // namespace

ScanoutRenderer::~ScanoutRenderer() { reset(); }

void ScanoutRenderer::reset() noexcept {
  if (device_) {
    for (const auto& [format, pipeline] : pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
    if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  }
  pipelines_.clear();
  pool_ = VK_NULL_HANDLE;
  set_ = VK_NULL_HANDLE;
  set_buffer_ = VK_NULL_HANDLE;
  pipeline_layout_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

bool ScanoutRenderer::create_layouts(VkDevice device) {
  if (pipeline_layout_) return true;
  device_ = device;
  const VkDescriptorSetLayoutBinding binding{kGuestBinding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
    error_ = "Vulkan scan-out descriptor layout creation failed";
    return false;
  }
  const VkPushConstantRange constants{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(ScanoutConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &constants;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
    error_ = "Vulkan scan-out pipeline layout creation failed";
    return false;
  }
  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &set_layout_;
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_) != VK_SUCCESS ||
      (allocate.descriptorPool = pool_,
       vkAllocateDescriptorSets(device, &allocate, &set_) != VK_SUCCESS)) {
    error_ = "Vulkan scan-out descriptor set allocation failed";
    return false;
  }
  return true;
}

bool ScanoutRenderer::prepare(VkDevice device, VkFormat format, GuestMemoryMirror& mirror) {
#ifndef XENON_HAS_DXC
  (void)device;
  (void)format;
  (void)mirror;
  error_ = "Vulkan scan-out requires DXC";
  return false;
#else
  if (!create_layouts(device)) return false;
  if (set_buffer_ != mirror.buffer()) {
    // The whole mirror: the shader addresses it by physical address.
    const VkDescriptorBufferInfo buffer{mirror.buffer(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set_;
    write.dstBinding = kGuestBinding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    set_buffer_ = mirror.buffer();
  }
  if (pipelines_.contains(format)) return true;
  DxcShaderCompiler compiler;
  ShaderCompileOptions options{};
  options.format = ShaderBinaryFormat::Spirv;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader(), options);
  const auto ps = compiler.compile(make_scanout_shader(), options);
  if (!vs.succeeded || !ps.succeeded) {
    error_ = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
             : !ps.diagnostics.empty()               ? ps.diagnostics.front()
                                                     : "Vulkan scan-out shader compilation failed";
    return false;
  }
  VkShaderModule vs_module = VK_NULL_HANDLE;
  VkShaderModule ps_module = VK_NULL_HANDLE;
  const auto create_module = [&](const CompiledShader& shader, VkShaderModule& module) {
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = shader.binary.size();
    info.pCode = reinterpret_cast<const std::uint32_t*>(shader.binary.data());
    return vkCreateShaderModule(device, &info, nullptr, &module) == VK_SUCCESS;
  };
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (create_module(vs, vs_module) && create_module(ps, ps_module)) {
    const std::array stages{
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs_module,
                                        "main", nullptr},
        VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps_module,
                                        "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vertex{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport_state{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xFu;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
    dynamic.pDynamicStates = dynamic_states.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS)
      pipeline = VK_NULL_HANDLE;
  }
  if (ps_module) vkDestroyShaderModule(device, ps_module, nullptr);
  if (vs_module) vkDestroyShaderModule(device, vs_module, nullptr);
  if (!pipeline) {
    error_ = "Vulkan scan-out pipeline creation failed";
    return false;
  }
  pipelines_.emplace(static_cast<std::uint32_t>(format), pipeline);
  return true;
#endif
}

void ScanoutRenderer::record(VkCommandBuffer command, GuestMemoryMirror& mirror,
                             VkImageView target, VkFormat format,
                             const ScanoutConstants& constants) {
  const auto found = pipelines_.find(static_cast<std::uint32_t>(format));
  if (found == pipelines_.end()) return;
  mirror.prepare_shader_access(command, false);
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = target;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
  rendering.renderArea.extent = {constants.target_width, constants.target_height};
  rendering.layerCount = 1;
  rendering.colorAttachmentCount = 1;
  rendering.pColorAttachments = &color;
  vkCmdBeginRendering(command, &rendering);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, found->second);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                          &set_, 0, nullptr);
  vkCmdPushConstants(command, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                     sizeof(constants), &constants);
  const VkViewport viewport{0.0f, 0.0f, float(constants.target_width),
                            float(constants.target_height), 0.0f, 1.0f};
  const VkRect2D scissor{{0, 0}, {constants.target_width, constants.target_height}};
  vkCmdSetViewport(command, 0, 1, &viewport);
  vkCmdSetScissor(command, 0, 1, &scissor);
  vkCmdDraw(command, 3, 1, 0, 0);
  vkCmdEndRendering(command);
}

bool ScanoutRenderer::capture(VkPhysicalDevice physical_device, VkDevice device,
                              CommandQueue& queue, GuestMemoryMirror& mirror,
                              const ScanoutConstants& constants,
                              PreparedPresentationFrame& output) {
  constexpr VkFormat kFormat = VK_FORMAT_R8G8B8A8_UNORM;
  output = {};
  if (!constants.target_width || !constants.target_height) {
    error_ = "Vulkan scan-out capture needs a non-zero extent";
    return false;
  }
  if (!prepare(device, kFormat, mirror)) return false;

  VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  image_info.imageType = VK_IMAGE_TYPE_2D;
  image_info.format = kFormat;
  image_info.extent = {constants.target_width, constants.target_height, 1};
  image_info.mipLevels = 1;
  image_info.arrayLayers = 1;
  image_info.samples = VK_SAMPLE_COUNT_1_BIT;
  image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
  image_info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  const auto cleanup = [&] {
    if (view) vkDestroyImageView(device, view, nullptr);
    if (image) vkDestroyImage(device, image, nullptr);
    if (memory) vkFreeMemory(device, memory, nullptr);
  };
  if (vkCreateImage(device, &image_info, nullptr, &image) != VK_SUCCESS) {
    error_ = "Vulkan scan-out capture image creation failed";
    return false;
  }
  VkMemoryRequirements requirements{};
  vkGetImageMemoryRequirements(device, image, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = find_memory_type(physical_device, requirements.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  view_info.image = image;
  view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
  view_info.format = kFormat;
  view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (allocation.memoryTypeIndex == UINT32_MAX ||
      vkAllocateMemory(device, &allocation, nullptr, &memory) != VK_SUCCESS ||
      vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS ||
      vkCreateImageView(device, &view_info, nullptr, &view) != VK_SUCCESS) {
    error_ = "Vulkan scan-out capture image allocation failed";
    cleanup();
    return false;
  }
  const auto row_pitch = constants.target_width * 4u;
  Buffer readback;
  if (!readback.initialize(physical_device, device,
                           VkDeviceSize(row_pitch) * constants.target_height,
                           VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
    error_ = readback.error();
    cleanup();
    return false;
  }
  const bool executed = queue.execute([&](VkCommandBuffer command) {
    VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = view_info.subresourceRange;
    VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(command, &dependency);

    record(command, mirror, view, kFormat, constants);

    barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier2(command, &dependency);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {constants.target_width, constants.target_height, 1};
    vkCmdCopyImageToBuffer(command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer(), 1, &copy);
    VkBufferMemoryBarrier2 to_host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    to_host.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    to_host.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    to_host.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    to_host.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    to_host.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_host.buffer = readback.buffer();
    to_host.size = VK_WHOLE_SIZE;
    VkDependencyInfo host_dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    host_dependency.bufferMemoryBarrierCount = 1;
    host_dependency.pBufferMemoryBarriers = &to_host;
    vkCmdPipelineBarrier2(command, &host_dependency);
  });
  cleanup();
  if (!executed) {
    error_ = queue.error();
    return false;
  }
  std::span<std::byte> bytes;
  if (!readback.map(bytes)) {
    error_ = readback.error();
    return false;
  }
  output.width = constants.target_width;
  output.height = constants.target_height;
  output.row_pitch = row_pitch;
  output.rgba8.assign(bytes.begin(), bytes.begin() + std::size_t(row_pitch) * output.height);
  readback.unmap();
  output.valid = true;
  return true;
}

}  // namespace xenon::gpu::vulkan
