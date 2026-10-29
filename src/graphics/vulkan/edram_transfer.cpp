#include "xenon/gpu/vulkan/edram_transfer.hpp"

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

}  // namespace

EdramColorTransfer::~EdramColorTransfer() {
  if (!device_) return;
  for (const auto& [key, pipeline] : pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
  if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
}

bool EdramColorTransfer::create_layouts(VkDevice device) {
  if (pipeline_layout_) return true;
  device_ = device;
  const VkDescriptorSetLayoutBinding binding{16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                             RenderTargetImage::kSampledSourceStages, nullptr};
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &binding;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_) != VK_SUCCESS) {
    error_ = "Vulkan EDRAM transfer descriptor layout creation failed";
    return false;
  }
  const VkPushConstantRange constants{VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(EdramTransferConstants)};
  VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  layout_info.setLayoutCount = 1;
  layout_info.pSetLayouts = &set_layout_;
  layout_info.pushConstantRangeCount = 1;
  layout_info.pPushConstantRanges = &constants;
  if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout_) != VK_SUCCESS) {
    error_ = "Vulkan EDRAM transfer pipeline layout creation failed";
    return false;
  }
  return true;
}

VkPipeline EdramColorTransfer::pipeline(VkDevice device, VkFormat format,
                                        MsaaSamples source_host, MsaaSamples destination_host) {
#ifndef XENON_HAS_DXC
  error_ = "Vulkan EDRAM transfer requires DXC";
  return VK_NULL_HANDLE;
#else
  const auto key = std::uint64_t(format) | (std::uint64_t(source_host) << 32u) |
                   (std::uint64_t(destination_host) << 40u);
  if (const auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
  if (!create_layouts(device)) return VK_NULL_HANDLE;
  UtilityShaderCompiler compiler;
  ShaderCompileOptions options{};
  options.format = ShaderBinaryFormat::Spirv;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader(), options);
  const auto ps = compiler.compile(make_edram_color_transfer_shader(source_host), options);
  if (!vs.succeeded || !ps.succeeded) {
    error_ = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
             : !ps.diagnostics.empty()               ? ps.diagnostics.front()
                                                     : "Vulkan EDRAM transfer shader compilation failed";
    return VK_NULL_HANDLE;
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
    multisample.rasterizationSamples = sample_count_bits(destination_host);
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
    info.layout = pipeline_layout_;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS)
      pipeline = VK_NULL_HANDLE;
  }
  if (ps_module) vkDestroyShaderModule(device, ps_module, nullptr);
  if (vs_module) vkDestroyShaderModule(device, vs_module, nullptr);
  if (!pipeline) {
    error_ = "Vulkan EDRAM transfer pipeline creation failed";
    return VK_NULL_HANDLE;
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
#endif
}

bool EdramColorTransfer::transfer(VkDevice device, CommandQueue& queue, RenderTargetImage& source,
                                  RenderTargetImage& destination,
                                  const EdramTransferConstants& constants,
                                  std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.image() || !destination.image() ||
      source.format() != destination.format()) {
    error_ = "Vulkan EDRAM transfer needs two color images of one format";
    return false;
  }
  if (regions.empty()) return true;
  const auto state = pipeline(device, destination.format(), source.host_msaa(),
                              destination.host_msaa());
  if (!state) return false;
  const auto source_set = source.sampled_descriptor_set(set_layout_);
  if (!source_set) {
    error_ = source.error();
    return false;
  }
  if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
        // The source is read as a texture and returned to attachment use.
        source.transition_to_color_attachment(command);
        destination.transition_to_color_attachment(command);
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.image = source.image();
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.imageMemoryBarrierCount = 1;
        dependency.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);

        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = destination.view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {destination.width(), destination.height()};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(command, &rendering);
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, state);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                                1, &source_set, 0, nullptr);
        vkCmdPushConstants(command, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(constants), &constants);
        const VkViewport viewport{0.0f, 0.0f, float(destination.width()),
                                  float(destination.height()), 0.0f, 1.0f};
        vkCmdSetViewport(command, 0, 1, &viewport);
        for (const auto& region : regions) {
          const VkRect2D scissor{{region.left, region.top},
                                 {std::uint32_t(region.right - region.left),
                                  std::uint32_t(region.bottom - region.top)}};
          vkCmdSetScissor(command, 0, 1, &scissor);
          vkCmdDraw(command, 3, 1, 0, 0);
        }
        vkCmdEndRendering(command);

        barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
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

}  // namespace xenon::gpu::vulkan
