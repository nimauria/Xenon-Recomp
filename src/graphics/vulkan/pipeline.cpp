#include "xenon/gpu/vulkan/pipeline.hpp"

#include <array>

namespace xenon::gpu::vulkan {
namespace {

VkPrimitiveTopology native_topology(HostPrimitiveTopology topology) {
  switch (topology) {
    case HostPrimitiveTopology::PointList: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case HostPrimitiveTopology::LineList: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case HostPrimitiveTopology::TriangleList: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkSampleCountFlagBits native_samples(MsaaSamples samples) {
  switch (samples) {
    case MsaaSamples::X1: return VK_SAMPLE_COUNT_1_BIT;
    case MsaaSamples::X2: return VK_SAMPLE_COUNT_2_BIT;
    case MsaaSamples::X4: return VK_SAMPLE_COUNT_4_BIT;
  }
  return VK_SAMPLE_COUNT_1_BIT;
}

VkBlendFactor native_blend_factor(BlendFactor factor) {
  switch (factor) {
    case BlendFactor::Zero: return VK_BLEND_FACTOR_ZERO;
    case BlendFactor::One: return VK_BLEND_FACTOR_ONE;
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_COLOR;
    case BlendFactor::InvSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case BlendFactor::SrcAlpha: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InvSrcAlpha: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestColor: return VK_BLEND_FACTOR_DST_COLOR;
    case BlendFactor::InvDestColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case BlendFactor::DestAlpha: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InvDestAlpha: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case BlendFactor::ConstantColor: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case BlendFactor::InvConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case BlendFactor::ConstantAlpha: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case BlendFactor::InvConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case BlendFactor::SrcAlphaSaturate: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkBlendFactor native_alpha_blend_factor(BlendFactor factor) {
  switch (factor) {
    case BlendFactor::SrcColor: return VK_BLEND_FACTOR_SRC_ALPHA;
    case BlendFactor::InvSrcColor: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case BlendFactor::DestColor: return VK_BLEND_FACTOR_DST_ALPHA;
    case BlendFactor::InvDestColor: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    default: return native_blend_factor(factor);
  }
}

VkBlendOp native_blend_operation(BlendOperation operation) {
  switch (operation) {
    case BlendOperation::Add: return VK_BLEND_OP_ADD;
    case BlendOperation::Subtract: return VK_BLEND_OP_SUBTRACT;
    case BlendOperation::Minimum: return VK_BLEND_OP_MIN;
    case BlendOperation::Maximum: return VK_BLEND_OP_MAX;
    case BlendOperation::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
  }
  return VK_BLEND_OP_ADD;
}

VkCompareOp native_compare(CompareFunction function) {
  return static_cast<VkCompareOp>(VK_COMPARE_OP_NEVER +
                                  static_cast<unsigned>(function));
}

VkStencilOp native_stencil(StencilOperation operation) {
  return static_cast<VkStencilOp>(VK_STENCIL_OP_KEEP +
                                  static_cast<unsigned>(operation));
}

VkStencilOpState native_stencil_face(const StencilFaceState& face,
                                     std::uint8_t compare_mask,
                                     std::uint8_t write_mask,
                                     std::uint8_t reference) {
  VkStencilOpState result{};
  result.failOp = native_stencil(face.fail);
  result.passOp = native_stencil(face.depth_pass);
  result.depthFailOp = native_stencil(face.depth_fail);
  result.compareOp = native_compare(face.function);
  result.compareMask = compare_mask;
  result.writeMask = write_mask;
  result.reference = reference;
  return result;
}

}  // namespace

GraphicsPipeline::~GraphicsPipeline() { reset(); }

bool GraphicsPipeline::initialize(VkDevice device, VkPipelineLayout layout,
                                  const CompiledShader& vertex_shader,
                                  const CompiledShader& pixel_shader,
                                  const CompiledShader* geometry_shader,
                                  std::span<const VkFormat> color_formats,
                                  MsaaSamples samples,
                                  HostPrimitiveTopology topology,
                                  const RasterState& raster,
                                  std::span<const std::uint8_t> color_write_masks,
                                  std::span<const BlendState> blend_states,
                                  VkFormat depth_format,
                                  const DepthTargetDescriptor* depth_state) {
  reset();
  error_.clear();
  if (!device || !layout || color_formats.size() > 4 ||
      (color_formats.empty() && depth_format == VK_FORMAT_UNDEFINED) ||
      color_write_masks.size() != color_formats.size() ||
      blend_states.size() != color_formats.size() ||
      !vertex_shader.succeeded || !pixel_shader.succeeded ||
      vertex_shader.format != ShaderBinaryFormat::Spirv ||
      pixel_shader.format != ShaderBinaryFormat::Spirv ||
      vertex_shader.binary.empty() || pixel_shader.binary.empty() ||
      (vertex_shader.binary.size() & 3u) || (pixel_shader.binary.size() & 3u) ||
      (geometry_shader &&
       (!geometry_shader->succeeded ||
        geometry_shader->format != ShaderBinaryFormat::Spirv ||
        geometry_shader->binary.empty() ||
        (geometry_shader->binary.size() & 3u)))) {
    error_ = "Vulkan graphics pipeline requires valid SPIR-V shaders and render state";
    return false;
  }
  for (std::size_t i = 0; i < color_formats.size(); ++i) {
    if (color_formats[i] == VK_FORMAT_UNDEFINED &&
        (color_write_masks[i] != 0 || blend_states[i].enabled)) {
      error_ =
          "Vulkan unused MRT slots must have writes and blending disabled";
      return false;
    }
  }
  device_ = device;
  VkShaderModuleCreateInfo module_info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  VkShaderModule vertex_module = VK_NULL_HANDLE;
  VkShaderModule pixel_module = VK_NULL_HANDLE;
  VkShaderModule geometry_module = VK_NULL_HANDLE;
  module_info.codeSize = vertex_shader.binary.size();
  module_info.pCode = reinterpret_cast<const std::uint32_t*>(vertex_shader.binary.data());
  if (vkCreateShaderModule(device_, &module_info, nullptr, &vertex_module) != VK_SUCCESS) {
    error_ = "vkCreateShaderModule failed for the translated Xenos vertex shader";
    reset();
    return false;
  }
  module_info.codeSize = pixel_shader.binary.size();
  module_info.pCode = reinterpret_cast<const std::uint32_t*>(pixel_shader.binary.data());
  if (vkCreateShaderModule(device_, &module_info, nullptr, &pixel_module) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, vertex_module, nullptr);
    error_ = "vkCreateShaderModule failed for the translated Xenos pixel shader";
    reset();
    return false;
  }
  if (geometry_shader) {
    module_info.codeSize = geometry_shader->binary.size();
    module_info.pCode = reinterpret_cast<const std::uint32_t*>(
        geometry_shader->binary.data());
    if (vkCreateShaderModule(device_, &module_info, nullptr,
                             &geometry_module) != VK_SUCCESS) {
      vkDestroyShaderModule(device_, pixel_module, nullptr);
      vkDestroyShaderModule(device_, vertex_module, nullptr);
      error_ = "vkCreateShaderModule failed for RectangleList expansion";
      reset();
      return false;
    }
  }

  std::array<VkPipelineShaderStageCreateInfo, 3> stages{
      VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                      nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                                      vertex_module, vertex_shader.entry_point.c_str(), nullptr},
      VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                      nullptr, 0, VK_SHADER_STAGE_GEOMETRY_BIT,
                                      geometry_module,
                                      geometry_shader ? geometry_shader->entry_point.c_str()
                                                      : nullptr,
                                      nullptr},
      VkPipelineShaderStageCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                      nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                                      pixel_module, pixel_shader.entry_point.c_str(), nullptr}};
  if (!geometry_shader) stages[1] = stages[2];
  VkPipelineVertexInputStateCreateInfo vertex_input{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo assembly{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  assembly.topology = native_topology(topology);
  VkPipelineViewportStateCreateInfo viewport{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  viewport.viewportCount = 1;
  viewport.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rasterization{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  switch (host_polygon_mode(raster)) {
    case HostPolygonMode::Point:
      rasterization.polygonMode = VK_POLYGON_MODE_POINT;
      break;
    case HostPolygonMode::Line:
      rasterization.polygonMode = VK_POLYGON_MODE_LINE;
      break;
    case HostPolygonMode::Fill:
      rasterization.polygonMode = VK_POLYGON_MODE_FILL;
      break;
  }
  rasterization.cullMode = raster.cull_front && raster.cull_back
                               ? VK_CULL_MODE_FRONT_AND_BACK
                               : raster.cull_front ? VK_CULL_MODE_FRONT_BIT
                               : raster.cull_back ? VK_CULL_MODE_BACK_BIT
                                                  : VK_CULL_MODE_NONE;
  rasterization.frontFace = raster.front_face_clockwise
                                ? VK_FRONT_FACE_CLOCKWISE
                                : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rasterization.lineWidth = 1.0f;
  const auto polygon_offset = preferred_polygon_offset(raster, topology);
  rasterization.depthBiasEnable = polygon_offset.enabled;
  rasterization.depthBiasConstantFactor = scaled_polygon_offset_constant(
      polygon_offset.offset,
      depth_state && depth_state->format ==
                         static_cast<std::uint8_t>(DepthRenderTargetFormat::D24FS8)
          ? DepthRenderTargetFormat::D24FS8
          : DepthRenderTargetFormat::D24S8);
  rasterization.depthBiasSlopeFactor = polygon_offset.scale / 16.0f;
  VkPipelineMultisampleStateCreateInfo multisample{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  multisample.rasterizationSamples = native_samples(samples);
  VkPipelineDepthStencilStateCreateInfo depth_stencil{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  if (depth_state) {
    depth_stencil.depthTestEnable = depth_state->test_enabled;
    depth_stencil.depthWriteEnable = depth_state->write_enabled;
    depth_stencil.depthCompareOp = native_compare(depth_state->function);
    depth_stencil.stencilTestEnable = depth_state->stencil_enabled;
    depth_stencil.front = native_stencil_face(
        depth_state->stencil_front, depth_state->stencil_read_mask,
        depth_state->stencil_write_mask, depth_state->stencil_reference);
    depth_stencil.back = depth_state->backface_stencil_enabled
        ? native_stencil_face(depth_state->stencil_back,
                              depth_state->stencil_back_read_mask,
                              depth_state->stencil_back_write_mask,
                              depth_state->stencil_back_reference)
        : depth_stencil.front;
  }
  std::array<VkPipelineColorBlendAttachmentState, 4> attachments{};
  for (std::size_t i = 0; i < color_formats.size(); ++i) {
    attachments[i].blendEnable = blend_states[i].enabled;
    attachments[i].srcColorBlendFactor = native_blend_factor(blend_states[i].color_source);
    attachments[i].dstColorBlendFactor = native_blend_factor(blend_states[i].color_destination);
    attachments[i].colorBlendOp = native_blend_operation(blend_states[i].color_operation);
    attachments[i].srcAlphaBlendFactor = native_alpha_blend_factor(blend_states[i].alpha_source);
    attachments[i].dstAlphaBlendFactor = native_alpha_blend_factor(blend_states[i].alpha_destination);
    attachments[i].alphaBlendOp = native_blend_operation(blend_states[i].alpha_operation);
    attachments[i].colorWriteMask =
        static_cast<VkColorComponentFlags>(color_write_masks[i] & 0xFu);
  }
  VkPipelineColorBlendStateCreateInfo blend{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  blend.attachmentCount = static_cast<std::uint32_t>(color_formats.size());
  blend.pAttachments = color_formats.empty() ? nullptr : attachments.data();
  const std::array dynamic_states{
      VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
      VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
      VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
  VkPipelineDynamicStateCreateInfo dynamic{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dynamic.dynamicStateCount = static_cast<std::uint32_t>(dynamic_states.size());
  dynamic.pDynamicStates = dynamic_states.data();
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.colorAttachmentCount = static_cast<std::uint32_t>(color_formats.size());
  rendering.pColorAttachmentFormats = color_formats.data();
  rendering.depthAttachmentFormat = depth_format;
  rendering.stencilAttachmentFormat = depth_format;
  VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  info.pNext = &rendering;
  info.stageCount = geometry_shader ? 3u : 2u;
  info.pStages = stages.data();
  info.pVertexInputState = &vertex_input;
  info.pInputAssemblyState = &assembly;
  info.pViewportState = &viewport;
  info.pRasterizationState = &rasterization;
  info.pMultisampleState = &multisample;
  info.pDepthStencilState = &depth_stencil;
  info.pColorBlendState = &blend;
  info.pDynamicState = &dynamic;
  info.layout = layout;
  const auto result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info,
                                                nullptr, &pipeline_);
  if (geometry_module) vkDestroyShaderModule(device_, geometry_module, nullptr);
  vkDestroyShaderModule(device_, pixel_module, nullptr);
  vkDestroyShaderModule(device_, vertex_module, nullptr);
  if (result != VK_SUCCESS) {
    error_ = "vkCreateGraphicsPipelines failed for translated Xenos shaders";
    reset();
    return false;
  }
  return true;
}

void GraphicsPipeline::reset() noexcept {
  if (device_ && pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  device_ = VK_NULL_HANDLE;
}

}  // namespace xenon::gpu::vulkan
