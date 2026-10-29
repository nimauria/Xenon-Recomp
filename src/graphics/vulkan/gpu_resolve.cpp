#include "xenon/gpu/vulkan/gpu_resolve.hpp"

#include <array>

#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::vulkan {
namespace {

VkSampleCountFlagBits sample_count_bits(MsaaSamples samples) {
  switch (samples) {
    case MsaaSamples::X2: return VK_SAMPLE_COUNT_2_BIT;
    case MsaaSamples::X4: return VK_SAMPLE_COUNT_4_BIT;
    default: return VK_SAMPLE_COUNT_1_BIT;
  }
}

#ifdef XENON_HAS_DXC
bool create_module(VkDevice device, const LoweredShader& shader, VkShaderModule& module,
                   std::string& error) {
  DxcShaderCompiler compiler;
  ShaderCompileOptions options{};
  options.format = ShaderBinaryFormat::Spirv;
  const auto compiled = compiler.compile(shader, options);
  if (!compiled.succeeded) {
    error = compiled.diagnostics.empty() ? "Vulkan GPU resolve shader compilation failed"
                                         : compiled.diagnostics.front();
    return false;
  }
  VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  info.codeSize = compiled.binary.size();
  info.pCode = reinterpret_cast<const std::uint32_t*>(compiled.binary.data());
  if (vkCreateShaderModule(device, &info, nullptr, &module) != VK_SUCCESS) {
    error = "Vulkan GPU resolve shader module creation failed";
    return false;
  }
  return true;
}
#endif

}  // namespace

GpuResolver::~GpuResolver() { reset(); }

void GpuResolver::reset() noexcept {
  if (device_) {
    for (const auto& [key, pipeline] : resolve_pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
    for (const auto& [key, pipeline] : clear_pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
    if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
    if (resolve_layout_) vkDestroyPipelineLayout(device_, resolve_layout_, nullptr);
    if (clear_layout_) vkDestroyPipelineLayout(device_, clear_layout_, nullptr);
    if (source_layout_) vkDestroyDescriptorSetLayout(device_, source_layout_, nullptr);
    if (guest_layout_) vkDestroyDescriptorSetLayout(device_, guest_layout_, nullptr);
  }
  resolve_pipelines_.clear();
  clear_pipelines_.clear();
  pool_ = VK_NULL_HANDLE;
  guest_set_ = VK_NULL_HANDLE;
  guest_set_buffer_ = VK_NULL_HANDLE;
  resolve_layout_ = clear_layout_ = VK_NULL_HANDLE;
  source_layout_ = guest_layout_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

bool GpuResolver::create_layouts(VkDevice device) {
  if (resolve_layout_) return true;
  device_ = device;
  // Set 0: the owner image, defined as every sampled-source user defines it.
  const VkDescriptorSetLayoutBinding source{16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                            RenderTargetImage::kSampledSourceStages, nullptr};
  // Set 1: the guest-memory mirror, written by physical address.
  const VkDescriptorSetLayoutBinding guest{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                           VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &source;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &source_layout_) != VK_SUCCESS) {
    error_ = "Vulkan GPU resolve descriptor layout creation failed";
    return false;
  }
  set_info.pBindings = &guest;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &guest_layout_) != VK_SUCCESS) {
    error_ = "Vulkan GPU resolve descriptor layout creation failed";
    return false;
  }
  const std::array set_layouts{source_layout_, guest_layout_};
  const VkPushConstantRange resolve_constants{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                              sizeof(GpuResolveConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = static_cast<std::uint32_t>(set_layouts.size());
  layout_info.pSetLayouts = set_layouts.data();
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &resolve_constants;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &resolve_layout_) != VK_SUCCESS) {
    error_ = "Vulkan GPU resolve pipeline layout creation failed";
    return false;
  }
  const VkPushConstantRange clear_constants{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                            sizeof(GpuClearConstants)};
  VkPipelineLayoutCreateInfo clear_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  clear_info.pushConstantRangeCount = 1;
  clear_info.pPushConstantRanges = &clear_constants;
  if (vkCreatePipelineLayout(device, &clear_info, nullptr, &clear_layout_) != VK_SUCCESS) {
    error_ = "Vulkan GPU resolve clear pipeline layout creation failed";
    return false;
  }
  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &guest_layout_;
  if (vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_) != VK_SUCCESS ||
      (allocate.descriptorPool = pool_,
       vkAllocateDescriptorSets(device, &allocate, &guest_set_) != VK_SUCCESS)) {
    error_ = "Vulkan GPU resolve descriptor set allocation failed";
    return false;
  }
  return true;
}

VkPipeline GpuResolver::resolve_pipeline(VkDevice device, MsaaSamples samples) {
#ifndef XENON_HAS_DXC
  (void)device;
  (void)samples;
  error_ = "Vulkan GPU resolve requires DXC";
  return VK_NULL_HANDLE;
#else
  const auto key = static_cast<std::uint32_t>(samples);
  if (const auto found = resolve_pipelines_.find(key); found != resolve_pipelines_.end())
    return found->second;
  if (!create_layouts(device)) return VK_NULL_HANDLE;
  VkShaderModule module = VK_NULL_HANDLE;
  if (!create_module(device, make_gpu_resolve_shader(samples), module, error_))
    return VK_NULL_HANDLE;
  VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  info.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
  info.layout = resolve_layout_;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
      VK_SUCCESS)
    pipeline = VK_NULL_HANDLE;
  vkDestroyShaderModule(device, module, nullptr);
  if (!pipeline) {
    error_ = "Vulkan GPU resolve pipeline creation failed";
    return VK_NULL_HANDLE;
  }
  resolve_pipelines_.emplace(key, pipeline);
  return pipeline;
#endif
}

VkPipeline GpuResolver::clear_pipeline(VkDevice device, VkFormat format, MsaaSamples samples) {
#ifndef XENON_HAS_DXC
  (void)device;
  (void)format;
  (void)samples;
  error_ = "Vulkan GPU resolve clear requires DXC";
  return VK_NULL_HANDLE;
#else
  const auto key = std::uint64_t(format) | (std::uint64_t(samples) << 32u);
  if (const auto found = clear_pipelines_.find(key); found != clear_pipelines_.end())
    return found->second;
  if (!create_layouts(device)) return VK_NULL_HANDLE;
  VkShaderModule vs_module = VK_NULL_HANDLE;
  VkShaderModule ps_module = VK_NULL_HANDLE;
  VkPipeline pipeline = VK_NULL_HANDLE;
  if (create_module(device, make_transfer_fullscreen_vertex_shader(), vs_module, error_) &&
      create_module(device, make_edram_clear_shader(), ps_module, error_)) {
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
    multisample.rasterizationSamples = sample_count_bits(samples);
    multisample.sampleShadingEnable = VK_TRUE;
    multisample.minSampleShading = 1.0f;
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
    info.layout = clear_layout_;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS) {
      pipeline = VK_NULL_HANDLE;
      error_ = "Vulkan GPU resolve clear pipeline creation failed";
    }
  }
  if (ps_module) vkDestroyShaderModule(device, ps_module, nullptr);
  if (vs_module) vkDestroyShaderModule(device, vs_module, nullptr);
  if (!pipeline) return VK_NULL_HANDLE;
  clear_pipelines_.emplace(key, pipeline);
  return pipeline;
#endif
}

bool GpuResolver::resolve(VkDevice device, CommandQueue& queue, GuestMemoryMirror& mirror,
                          RenderTargetImage& owner, const GpuResolveConstants& constants) {
  if (!constants.width || !constants.height) return true;
  const auto pipeline = resolve_pipeline(device, owner.host_msaa());
  if (!pipeline) return false;
  const auto source_set = owner.sampled_descriptor_set(source_layout_);
  if (!source_set) {
    error_ = owner.error();
    return false;
  }
  if (guest_set_buffer_ != mirror.buffer()) {
    const VkDescriptorBufferInfo buffer{mirror.buffer(), 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = guest_set_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = &buffer;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    guest_set_buffer_ = mirror.buffer();
  }
  if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
        owner.transition_to_color_attachment(command);
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = owner.image();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
        mirror.prepare_shader_access(command, true);

        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        const std::array sets{source_set, guest_set_};
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, resolve_layout_, 0,
                                static_cast<std::uint32_t>(sets.size()), sets.data(), 0,
                                nullptr);
        vkCmdPushConstants(command, resolve_layout_, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(constants), &constants);
        vkCmdDispatch(command, (constants.width + 7u) / 8u, (constants.height + 7u) / 8u, 1u);

        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask =
            VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier2(command, &dependency);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

bool GpuResolver::clear(VkDevice device, CommandQueue& queue, RenderTargetImage& owner,
                        const GpuClearConstants& constants) {
  const auto pipeline = clear_pipeline(device, owner.format(), owner.host_msaa());
  if (!pipeline) return false;
  if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
        owner.transition_to_color_attachment(command);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = owner.view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {owner.width(), owner.height()};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(command, &rendering);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdPushConstants(command, clear_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(constants), &constants);
        const VkViewport viewport{0.0f, 0.0f, float(owner.width()), float(owner.height()),
                                  0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, {owner.width(), owner.height()}};
        vkCmdSetViewport(command, 0, 1, &viewport);
        vkCmdSetScissor(command, 0, 1, &scissor);
        vkCmdDraw(command, 3, 1, 0, 0);
        vkCmdEndRendering(command);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

}  // namespace xenon::gpu::vulkan
