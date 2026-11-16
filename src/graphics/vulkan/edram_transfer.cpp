#include "xenon/gpu/vulkan/edram_transfer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <type_traits>
#include <vector>

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

EdramTransferEndpoint endpoint_of(const RenderTargetImage& image) noexcept {
  return {false, static_cast<std::uint8_t>(image.guest_format())};
}
EdramTransferEndpoint endpoint_of(const DepthTargetImage& image) noexcept {
  return {true, static_cast<std::uint8_t>(image.guest_format())};
}
EdramTransferEndpoint endpoint_of(const CanonicalEdramSource&) noexcept { return {}; }

std::uint32_t layout_kind(const RenderTargetImage&) noexcept {
  return EdramTransferLayouts::kColorSource;
}
std::uint32_t layout_kind(const DepthTargetImage&) noexcept {
  return EdramTransferLayouts::kDepthSource;
}
std::uint32_t layout_kind(const CanonicalEdramSource&) noexcept {
  return EdramTransferLayouts::kCanonicalSource;
}

// The descriptor set a transfer reads its source through: a color image's
// transfer view (integer for 16-bit float storage), a depth image's planes.
VkDescriptorSet transfer_set(RenderTargetImage& source, VkDescriptorSetLayout layout) {
  return source.transfer_descriptor_set(layout);
}
VkDescriptorSet transfer_set(DepthTargetImage& source, VkDescriptorSetLayout layout) {
  return source.sampled_descriptor_set(layout);
}
VkDescriptorSet transfer_set(CanonicalEdramSource& source, VkDescriptorSetLayout) {
  return source.set;
}

// Makes a source image readable by the transfer's fragment shader, and
// returns it to attachment use afterwards.
void begin_sampling(VkCommandBuffer command, RenderTargetImage& source) {
  source.transition_to_color_attachment(command);
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
}
void end_sampling(VkCommandBuffer command, RenderTargetImage& source) {
  VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
  barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
  barrier.srcAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
  barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
  barrier.dstAccessMask =
      VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  barrier.image = source.image();
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
  dependency.imageMemoryBarrierCount = 1;
  dependency.pImageMemoryBarriers = &barrier;
  vkCmdPipelineBarrier2(command, &dependency);
}
void begin_sampling(VkCommandBuffer command, DepthTargetImage& source) {
  source.transition_to_sampled(command);
}
void end_sampling(VkCommandBuffer command, DepthTargetImage& source) {
  source.transition_to_depth_attachment(command);
}
// The store's upload makes its copies visible to the fragment shader, and
// the next upload waits for those reads (EdramCanonicalStore::upload()).
void begin_sampling(VkCommandBuffer, CanonicalEdramSource&) {}
void end_sampling(VkCommandBuffer, CanonicalEdramSource&) {}

void draw_transfer_regions(VkCommandBuffer command, std::span<const EdramSurfaceRegion> regions) {
  for (const auto& region : regions) {
    const VkRect2D scissor{{region.left, region.top},
                           {std::uint32_t(region.right - region.left),
                            std::uint32_t(region.bottom - region.top)}};
    vkCmdSetScissor(command, 0, 1, &scissor);
    vkCmdDraw(command, 3, 1, 0, 0);
  }
}

#ifdef XENON_HAS_DXC
// The fullscreen transfer pipeline drawing `pixel` into a color attachment
// of `format`, or a depth/stencil attachment of it (`depth`, whose stencil
// reference and write mask are dynamic).
VkPipeline create_transfer_pipeline(VkDevice device, VkPipelineLayout layout,
                                    const LoweredShader& pixel, bool stencil_export,
                                    VkFormat format, bool depth, MsaaSamples destination_host,
                                    EdramDepthTransferPass pass, std::string& error) {
  UtilityShaderCompiler compiler;
  ShaderCompileOptions options{};
  options.format = ShaderBinaryFormat::Spirv;
  const auto vs = compiler.compile(make_transfer_fullscreen_vertex_shader(), options);
  options.spirv_stencil_export = stencil_export;
  const auto ps = compiler.compile(pixel, options);
  if (!vs.succeeded || !ps.succeeded) {
    error = !vs.succeeded && !vs.diagnostics.empty() ? vs.diagnostics.front()
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
    VkPipelineDepthStencilStateCreateInfo depth_stencil{
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth_stencil.depthTestEnable = VK_TRUE;
    depth_stencil.depthWriteEnable = pass != EdramDepthTransferPass::StencilBit;
    depth_stencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depth_stencil.stencilTestEnable = VK_TRUE;
    depth_stencil.front = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_KEEP,
                           VK_COMPARE_OP_ALWAYS, 0xFFu, 0xFFu, 0u};
    depth_stencil.back = depth_stencil.front;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = 0xFu;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    if (!depth) {
      blend.attachmentCount = 1;
      blend.pAttachments = &attachment;
    }
    // Depth: the stencil reference and write mask change between passes.
    const std::array dynamic_states{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                                    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = depth ? 4u : 2u;
    dynamic.pDynamicStates = dynamic_states.data();
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    if (depth) {
      rendering.depthAttachmentFormat = format;
      rendering.stencilAttachmentFormat = format;
    } else {
      rendering.colorAttachmentCount = 1;
      rendering.pColorAttachmentFormats = &format;
    }
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = static_cast<std::uint32_t>(stages.size());
    info.pStages = stages.data();
    info.pVertexInputState = &vertex;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = depth ? &depth_stencil : nullptr;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = layout;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) !=
        VK_SUCCESS)
      pipeline = VK_NULL_HANDLE;
  }
  if (ps_module) vkDestroyShaderModule(device, ps_module, nullptr);
  if (vs_module) vkDestroyShaderModule(device, vs_module, nullptr);
  if (!pipeline) error = "Vulkan EDRAM transfer pipeline creation failed";
  return pipeline;
}
#endif

}  // namespace

EdramTransferLayouts::~EdramTransferLayouts() {
  if (!device_) return;
  for (auto layout : pipeline_layouts_)
    if (layout) vkDestroyPipelineLayout(device_, layout, nullptr);
  for (auto layout : set_layouts_)
    if (layout) vkDestroyDescriptorSetLayout(device_, layout, nullptr);
}

bool EdramTransferLayouts::create(VkDevice device, std::string& error) {
  if (pipeline_layouts_[0] && pipeline_layouts_[1] && pipeline_layouts_[2]) return true;
  device_ = device;
  const VkDescriptorSetLayoutBinding color_binding{16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                   RenderTargetImage::kSampledSourceStages,
                                                   nullptr};
  const std::array depth_bindings{
      VkDescriptorSetLayoutBinding{16, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                   DepthTargetImage::kSampledSourceStages, nullptr},
      VkDescriptorSetLayoutBinding{17, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                   DepthTargetImage::kSampledSourceStages, nullptr}};
  const VkPushConstantRange constants{VK_SHADER_STAGE_FRAGMENT_BIT, 0, constant_bytes_};
  for (std::size_t kind = 0; kind < 3; ++kind) {
    if (pipeline_layouts_[kind]) continue;
    VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    set_info.bindingCount =
        kind == kDepthSource ? static_cast<std::uint32_t>(depth_bindings.size()) : 1u;
    set_info.pBindings = kind == kDepthSource    ? depth_bindings.data()
                         : kind == kColorSource ? &color_binding
                                                : &kCanonicalBinding;
    if (!set_layouts_[kind] &&
        vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layouts_[kind]) !=
            VK_SUCCESS) {
      set_layouts_[kind] = VK_NULL_HANDLE;
      error = "Vulkan EDRAM transfer descriptor layout creation failed";
      return false;
    }
    VkPipelineLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    layout_info.setLayoutCount = 1;
    layout_info.pSetLayouts = &set_layouts_[kind];
    layout_info.pushConstantRangeCount = 1;
    layout_info.pPushConstantRanges = &constants;
    if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layouts_[kind]) !=
        VK_SUCCESS) {
      pipeline_layouts_[kind] = VK_NULL_HANDLE;
      error = "Vulkan EDRAM transfer pipeline layout creation failed";
      return false;
    }
  }
  return true;
}

EdramColorTransfer::~EdramColorTransfer() {
  if (!device_) return;
  for (const auto& [key, pipeline] : pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
}

VkPipeline EdramColorTransfer::pipeline(VkDevice device, EdramTransferEndpoint source,
                                        MsaaSamples source_host,
                                        const RenderTargetImage& destination, bool converting,
                                        bool canonical) {
#ifndef XENON_HAS_DXC
  error_ = "Vulkan EDRAM transfer requires DXC";
  return VK_NULL_HANDLE;
#else
  const auto format = destination.transfer_format();
  const auto destination_host = destination.host_msaa();
  // Same-format pipelines depend only on the formats and sample counts.
  const auto key = std::uint64_t(format) | (std::uint64_t(source_host) << 32u) |
                   (std::uint64_t(destination_host) << 36u) |
                   (converting ? (std::uint64_t(1u) << 40u) |
                                     (std::uint64_t(source.depth) << 41u) |
                                     (std::uint64_t(source.format) << 44u) |
                                     (std::uint64_t(destination.guest_format()) << 52u)
                               : 0u) |
                   (std::uint64_t(canonical) << 60u);
  if (const auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
  device_ = device;
  if (!layouts_.create(device, error_)) return VK_NULL_HANDLE;
  const auto pixel = canonical ? make_edram_canonical_color_load_shader(destination.guest_format())
                     : converting ? make_edram_converting_color_transfer_shader(
                                        source_host, source, destination.guest_format())
                                  : make_edram_color_transfer_shader(
                                        source_host, destination.transfer_format() !=
                                                         destination.format());
  const auto kind = canonical      ? EdramTransferLayouts::kCanonicalSource
                    : source.depth ? EdramTransferLayouts::kDepthSource
                                   : EdramTransferLayouts::kColorSource;
  const auto pipeline =
      create_transfer_pipeline(device, layouts_.pipeline_layout(kind), pixel, false,
                               format, false, destination_host,
                               EdramDepthTransferPass::Depth, error_);
  if (!pipeline) return VK_NULL_HANDLE;
  pipelines_.emplace(key, pipeline);
  return pipeline;
#endif
}

template <typename Source>
bool EdramColorTransfer::run(VkDevice device, CommandQueue& queue, Source& source,
                             RenderTargetImage& destination, bool converting,
                             const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions) {
  if (regions.empty()) return true;
  constexpr bool canonical = std::is_same_v<Source, CanonicalEdramSource>;
  const auto endpoint = endpoint_of(source);
  const auto state =
      pipeline(device, endpoint, source.host_msaa(), destination, converting, canonical);
  if (!state) return false;
  const auto layout = layouts_.pipeline_layout(layout_kind(source));
  const auto source_set = transfer_set(source, layouts_.set_layout(layout_kind(source)));
  if (!source_set) {
    error_ = source.error();
    return false;
  }
  if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
        // The source is read as a texture and returned to attachment use.
        begin_sampling(command, source);
        destination.transition_to_color_attachment(command);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = destination.transfer_view();
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
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                &source_set, 0, nullptr);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(constants),
                           &constants);
        const VkViewport viewport{0.0f, 0.0f, float(destination.width()),
                                  float(destination.height()), 0.0f, 1.0f};
        vkCmdSetViewport(command, 0, 1, &viewport);
        draw_transfer_regions(command, regions);
        vkCmdEndRendering(command);
        end_sampling(command, source);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
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
  return run(device, queue, source, destination, false, constants, regions);
}

bool EdramColorTransfer::convert(VkDevice device, CommandQueue& queue, RenderTargetImage& source,
                                 RenderTargetImage& destination,
                                 const EdramTransferConstants& constants,
                                 std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.image() || !destination.image() || &source == &destination ||
      !edram_converting_transfer_supported(endpoint_of(source), endpoint_of(destination))) {
    error_ = "Vulkan converting EDRAM transfer needs a supported source and color destination";
    return false;
  }
  return run(device, queue, source, destination, true, constants, regions);
}

bool EdramColorTransfer::convert(VkDevice device, CommandQueue& queue, DepthTargetImage& source,
                                 RenderTargetImage& destination,
                                 const EdramTransferConstants& constants,
                                 std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.image() || !destination.image() ||
      !edram_converting_transfer_supported(endpoint_of(source), endpoint_of(destination))) {
    error_ = "Vulkan converting EDRAM transfer needs a supported source and color destination";
    return false;
  }
  return run(device, queue, source, destination, true, constants, regions);
}

bool EdramColorTransfer::load_canonical(VkDevice device, CommandQueue& queue,
                                        CanonicalEdramSource source,
                                        RenderTargetImage& destination,
                                        const EdramTransferConstants& constants,
                                        std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.set || !destination.image() ||
      !edram_canonical_load_supported(endpoint_of(destination))) {
    error_ = "Vulkan canonical EDRAM load needs the canonical store and a supported color image";
    return false;
  }
  return run(device, queue, source, destination, true, constants, regions);
}

EdramDepthTransfer::~EdramDepthTransfer() {
  if (!device_) return;
  for (const auto& [key, pipeline] : pipelines_) vkDestroyPipeline(device_, pipeline, nullptr);
}

VkPipeline EdramDepthTransfer::pipeline(VkDevice device, EdramTransferEndpoint source,
                                        MsaaSamples source_host,
                                        const DepthTargetImage& destination, bool converting,
                                        EdramDepthTransferPass pass, bool canonical) {
#ifndef XENON_HAS_DXC
  error_ = "Vulkan EDRAM depth transfer requires DXC";
  return VK_NULL_HANDLE;
#else
  const auto format = destination.format();
  const auto destination_host = destination.host_msaa();
  const auto key = std::uint64_t(format) | (std::uint64_t(source_host) << 32u) |
                   (std::uint64_t(destination_host) << 36u) | (std::uint64_t(pass) << 40u) |
                   (converting ? (std::uint64_t(1u) << 43u) |
                                     (std::uint64_t(source.depth) << 44u) |
                                     (std::uint64_t(source.format) << 46u) |
                                     (std::uint64_t(destination.guest_format()) << 54u)
                               : 0u) |
                   (std::uint64_t(destination.unorm24_depth_write()) << 63u) |
                   (std::uint64_t(canonical) << 62u);
  if (const auto found = pipelines_.find(key); found != pipelines_.end()) return found->second;
  device_ = device;
  if (!layouts_.create(device, error_)) return VK_NULL_HANDLE;
  const auto write = destination.unorm24_depth_write();
  const auto pixel =
      canonical ? make_edram_canonical_depth_load_shader(destination.guest_format(), pass, write)
      : converting ? make_edram_converting_depth_transfer_shader(
                         source_host, source, destination.guest_format(), pass, write)
                   : make_edram_depth_transfer_shader(source_host, pass,
                                                      destination.guest_format(), write);
  const auto kind = canonical      ? EdramTransferLayouts::kCanonicalSource
                    : source.depth ? EdramTransferLayouts::kDepthSource
                                   : EdramTransferLayouts::kColorSource;
  const auto pipeline = create_transfer_pipeline(
      device, layouts_.pipeline_layout(kind), pixel,
      pass == EdramDepthTransferPass::DepthAndStencil, format, true, destination_host, pass,
      error_);
  if (!pipeline) return VK_NULL_HANDLE;
  pipelines_.emplace(key, pipeline);
  return pipeline;
#endif
}

template <typename Source>
bool EdramDepthTransfer::run(VkDevice device, VkPhysicalDevice physical_device,
                             CommandQueue& queue, Source& source, DepthTargetImage& destination,
                             bool converting, const EdramTransferConstants& constants,
                             std::span<const EdramSurfaceRegion> regions) {
  if (regions.empty()) return true;
  if (stencil_export_ < 0) {
    // The context enables VK_EXT_shader_stencil_export whenever the device
    // offers it.
    std::uint32_t count{};
    std::vector<VkExtensionProperties> extensions;
    if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count, nullptr) ==
        VK_SUCCESS) {
      extensions.resize(count);
      if (vkEnumerateDeviceExtensionProperties(physical_device, nullptr, &count,
                                               extensions.data()) != VK_SUCCESS)
        extensions.clear();
    }
    stencil_export_ = std::any_of(extensions.begin(), extensions.end(), [](const auto& e) {
      return std::strcmp(e.extensionName, VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME) == 0;
    });
  }
  const bool combined = stencil_export_ == 1;
  constexpr bool canonical = std::is_same_v<Source, CanonicalEdramSource>;
  const auto endpoint = endpoint_of(source);
  const auto first = pipeline(device, endpoint, source.host_msaa(), destination, converting,
                              combined ? EdramDepthTransferPass::DepthAndStencil
                                       : EdramDepthTransferPass::Depth,
                              canonical);
  if (!first) return false;
  VkPipeline stencil_bits = VK_NULL_HANDLE;
  if (!combined) {
    stencil_bits = pipeline(device, endpoint, source.host_msaa(), destination, converting,
                            EdramDepthTransferPass::StencilBit, canonical);
    if (!stencil_bits) return false;
  }
  const auto layout = layouts_.pipeline_layout(layout_kind(source));
  const auto source_set = transfer_set(source, layouts_.set_layout(layout_kind(source)));
  if (!source_set) {
    error_ = source.error();
    return false;
  }
  EdramDepthTransferConstants values{};
  values.transfer = constants;
  if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
        begin_sampling(command, source);
        destination.transition_to_depth_attachment(command);
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = destination.view();
        attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea.extent = {destination.width(), destination.height()};
        rendering.layerCount = 1;
        rendering.pDepthAttachment = &attachment;
        rendering.pStencilAttachment = &attachment;
        vkCmdBeginRendering(command, &rendering);
        vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                                &source_set, 0, nullptr);
        const VkViewport viewport{0.0f, 0.0f, float(destination.width()),
                                  float(destination.height()), 0.0f, 1.0f};
        vkCmdSetViewport(command, 0, 1, &viewport);
        // Depth (and stencil, or stencil cleared to zero).
        vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, first);
        vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(values),
                           &values);
        vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0u);
        vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFFu);
        draw_transfer_regions(command, regions);
        if (!combined) {
          // Each stencil bit the source sample has set, written alone.
          vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, stencil_bits);
          vkCmdSetStencilReference(command, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFFu);
          for (std::uint32_t bit = 0; bit < 8u; ++bit) {
            values.stencil[0] = 1u << bit;
            vkCmdPushConstants(command, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(values),
                               &values);
            vkCmdSetStencilWriteMask(command, VK_STENCIL_FACE_FRONT_AND_BACK, 1u << bit);
            draw_transfer_regions(command, regions);
          }
        }
        vkCmdEndRendering(command);
        end_sampling(command, source);
      })) {
    error_ = queue.error();
    return false;
  }
  return true;
}

bool EdramDepthTransfer::transfer(VkDevice device, VkPhysicalDevice physical_device,
                                  CommandQueue& queue, DepthTargetImage& source,
                                  DepthTargetImage& destination,
                                  const EdramTransferConstants& constants,
                                  std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.image() || !destination.image() ||
      source.format() != destination.format() ||
      source.guest_format() != destination.guest_format()) {
    error_ = "Vulkan EDRAM depth transfer needs two depth images of one format";
    return false;
  }
  return run(device, physical_device, queue, source, destination, false, constants, regions);
}

bool EdramDepthTransfer::convert(VkDevice device, VkPhysicalDevice physical_device,
                                 CommandQueue& queue, DepthTargetImage& source,
                                 DepthTargetImage& destination,
                                 const EdramTransferConstants& constants,
                                 std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.image() || !destination.image() || &source == &destination ||
      !edram_converting_transfer_supported(endpoint_of(source), endpoint_of(destination))) {
    error_ = "Vulkan converting EDRAM transfer needs a supported source and depth destination";
    return false;
  }
  return run(device, physical_device, queue, source, destination, true, constants, regions);
}

bool EdramDepthTransfer::convert(VkDevice device, VkPhysicalDevice physical_device,
                                 CommandQueue& queue, RenderTargetImage& source,
                                 DepthTargetImage& destination,
                                 const EdramTransferConstants& constants,
                                 std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.image() || !destination.image() ||
      !edram_converting_transfer_supported(endpoint_of(source), endpoint_of(destination))) {
    error_ = "Vulkan converting EDRAM transfer needs a supported source and depth destination";
    return false;
  }
  return run(device, physical_device, queue, source, destination, true, constants, regions);
}

bool EdramDepthTransfer::load_canonical(VkDevice device, VkPhysicalDevice physical_device,
                                        CommandQueue& queue, CanonicalEdramSource source,
                                        DepthTargetImage& destination,
                                        const EdramTransferConstants& constants,
                                        std::span<const EdramSurfaceRegion> regions) {
  if (!device || !source.set || !destination.image() ||
      !edram_canonical_load_supported(endpoint_of(destination))) {
    error_ = "Vulkan canonical EDRAM load needs the canonical store and a supported depth image";
    return false;
  }
  return run(device, physical_device, queue, source, destination, true, constants, regions);
}

EdramCanonicalStore::~EdramCanonicalStore() {
  if (!device_) return;
  if (pool_) vkDestroyDescriptorPool(device_, pool_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
}

bool EdramCanonicalStore::create(VkPhysicalDevice physical_device, VkDevice device) {
  if (set_) return true;
  device_ = device;
  if (!buffer_.initialize(physical_device, device, Edram::kSize,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
    error_ = "Vulkan canonical EDRAM buffer: " + buffer_.error();
    return false;
  }
  if (!staging_.initialize(physical_device, device, Edram::kSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                           VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                               VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
      !staging_.map(mapped_)) {
    error_ = "Vulkan canonical EDRAM staging buffer: " + staging_.error();
    return false;
  }
  VkDescriptorSetLayoutCreateInfo set_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  set_info.bindingCount = 1;
  set_info.pBindings = &EdramTransferLayouts::kCanonicalBinding;
  const VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo pool_info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pool_info.maxSets = 1;
  pool_info.poolSizeCount = 1;
  pool_info.pPoolSizes = &pool_size;
  if (vkCreateDescriptorSetLayout(device, &set_info, nullptr, &set_layout_) != VK_SUCCESS ||
      vkCreateDescriptorPool(device, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
    error_ = "Vulkan canonical EDRAM descriptor creation failed";
    return false;
  }
  VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  allocate.descriptorPool = pool_;
  allocate.descriptorSetCount = 1;
  allocate.pSetLayouts = &set_layout_;
  VkDescriptorSet set = VK_NULL_HANDLE;
  if (vkAllocateDescriptorSets(device, &allocate, &set) != VK_SUCCESS) {
    error_ = "Vulkan canonical EDRAM descriptor set allocation failed";
    return false;
  }
  const VkDescriptorBufferInfo info{buffer_.buffer(), 0, Edram::kSize};
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = set;
  write.dstBinding = 16;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  write.pBufferInfo = &info;
  vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
  set_ = set;
  return true;
}

bool EdramCanonicalStore::upload(VkPhysicalDevice physical_device, VkDevice device,
                                 CommandQueue& queue, const Edram& edram,
                                 std::span<const std::uint16_t> tiles) {
  if (tiles.empty()) return true;
  if (!device || !create(physical_device, device)) {
    if (error_.empty()) error_ = "Vulkan canonical EDRAM upload has no device";
    return false;
  }
  // A tile's staging bytes are reused only once the copy reading them is done.
  std::uint64_t busy = 0;
  for (const auto tile : tiles) busy = (std::max)(busy, tile_busy_until_[tile % Edram::kTileCount]);
  if (busy > queue.completed_value()) {
    if (busy > queue.last_submitted_value() && !queue.flush()) {
      error_ = queue.error();
      return false;
    }
    if (!queue.wait_for_completion(busy)) {
      error_ = queue.error();
      return false;
    }
  }
  std::vector<std::uint16_t> sorted(tiles.begin(), tiles.end());
  std::sort(sorted.begin(), sorted.end());
  sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
  std::vector<VkBufferCopy> copies;
  const auto bytes = edram.bytes();
  for (const auto tile : sorted) {
    const auto offset = VkDeviceSize(tile % Edram::kTileCount) * Edram::kTileBytes;
    std::memcpy(mapped_.data() + offset, bytes.data() + offset, Edram::kTileBytes);
    if (!copies.empty() && copies.back().srcOffset + copies.back().size == offset)
      copies.back().size += Edram::kTileBytes;
    else
      copies.push_back({offset, offset, Edram::kTileBytes});
  }
  if (!queue.execute_async([&](VkCommandBuffer command, std::uint32_t, std::uint32_t) {
        // Earlier loads' reads finish before the copy, which finishes before
        // the loads that follow read it.
        VkBufferMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = buffer_.buffer();
        barrier.size = VK_WHOLE_SIZE;
        VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dependency.bufferMemoryBarrierCount = 1;
        dependency.pBufferMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(command, &dependency);
        vkCmdCopyBuffer(command, staging_.buffer(), buffer_.buffer(),
                        static_cast<std::uint32_t>(copies.size()), copies.data());
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
        vkCmdPipelineBarrier2(command, &dependency);
      })) {
    error_ = queue.error();
    return false;
  }
  const auto retirement = queue.retirement_value();
  for (const auto tile : sorted) tile_busy_until_[tile] = retirement;
  return true;
}

}  // namespace xenon::gpu::vulkan
