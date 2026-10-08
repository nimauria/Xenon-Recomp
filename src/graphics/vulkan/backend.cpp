#include "xenon/gpu/vulkan/backend.hpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <deque>
#include <variant>
#include <span>
#include <unordered_set>
#include <unordered_map>

#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/depth_target.hpp"
#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"
#include "xenon/gpu/vulkan/pipeline.hpp"
#include "xenon/gpu/vulkan/presentation.hpp"
#include "xenon/gpu/vulkan/resource_layout.hpp"
#include "xenon/gpu/vulkan/render_target.hpp"
#include "xenon/gpu/vulkan/texture.hpp"
#include "graphics/common/backend_core.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/edram_ownership.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/logging/logger.hpp"
#include "xenon/memory/types.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::vulkan {

struct BackendApi {
  static constexpr std::string_view kName = "Vulkan";
  using Context = vulkan::Context;
  using CommandQueue = vulkan::CommandQueue;
  using PresentationSwapchain = vulkan::PresentationSwapchain;
  using GuestMemoryMirror = vulkan::GuestMemoryMirror;
  using ResourceLayout = vulkan::ResourceLayout;
  using Buffer = vulkan::Buffer;
  using TextureImage = vulkan::TextureImage;
  using RenderTargetImage = vulkan::RenderTargetImage;
  using DepthTargetImage = vulkan::DepthTargetImage;
  using GraphicsPipeline = vulkan::GraphicsPipeline;
  using Format = VkFormat;
  static constexpr VkFormat kUndefinedFormat = VK_FORMAT_UNDEFINED;
  static VkFormat native_color_format(ColorRenderTargetFormat format) {
    return color_render_target_format(format);
  }
  // Dynamic rendering accepts an undefined format for an unused color slot.
  static VkFormat color_format(const vulkan::RenderTargetImage* target) {
    return target ? target->format() : VK_FORMAT_UNDEFINED;
  }
  // VK_CULL_MODE_FRONT_AND_BACK is native.
  static constexpr bool kEmulatesFrontAndBackCull = false;
#ifdef XENON_HAS_DXC
  static constexpr ShaderBinaryFormat kShaderBinaryFormat =
      ShaderBinaryFormat::Spirv;
  static constexpr std::string_view kShaderBinaryName = "SPIR-V";
#endif
};

class Backend::Impl : public detail::BackendCore<Backend::Impl, BackendApi> {
 public:
  ~Impl() {
    // Native draws may still be using render targets, textures, descriptor
    // snapshots and upload arenas owned by this Impl. Drain them before member
    // destruction starts.
    (void)queue.wait_idle();
  }

  bool create_render_target(RenderTargetImage& image,
                            const EdramSurfaceLayout& surface,
                            ColorRenderTargetFormat format) {
    return image.initialize(context.physical_device(), context.device(), queue,
                            surface, format);
  }
  // Dynamic rendering leaves unused color attachments undefined; see
  // BackendApi::color_format().
  bool fill_unbound_color_slots(const DrawResourceState&, std::uint32_t,
                                std::size_t, std::array<RenderTargetImage*, 4>&,
                                std::array<std::uint8_t, 4>&,
                                std::array<BlendState, 4>&) {
    return true;
  }
  bool create_depth_target(DepthTargetImage& image,
                           const EdramSurfaceLayout& surface,
                           DepthRenderTargetFormat format) {
    return image.initialize(context.physical_device(), context.device(),
                            surface, format);
  }
  bool create_texture(TextureImage& image, const DecodedTexture& decoded) {
    const bool initialized = image.initialize(
        context.physical_device(), context.device(), queue, decoded);
    // Fold in regardless of overall success/failure: a texture can
    // successfully bind while its sampler still substituted an
    // unsupported clamp mode.
    unsupported.unsupported_sampler_behaviors +=
        image.unsupported_sampler_behaviors();
    return initialized;
  }
  bool bind_texture(std::uint32_t slot, const TextureDescriptor& descriptor,
                    TextureImage& image) {
    return resources.bind_texture(slot, descriptor.dimension, image.view(),
                                  image.sampler());
  }
  VkPipelineLayout pipeline_root() { return resources.pipeline_layout(); }
  std::uint32_t color_render_width(const DrawResourceState& state,
                                   const RenderTargetImage*) {
    return std::uint32_t(state.raster.surface_pitch);
  }
  std::uint32_t color_render_height(const DrawResourceState& state,
                                    const RenderTargetImage*) {
    return std::uint32_t(state.raster.scissor_bottom);
  }
  void forget_backend_render_targets() {}
  bool initialize_mirror(memory::AddressSpace& guest_memory) {
    return mirror.initialize(context.physical_device(), context.device(), queue,
                             guest_memory);
  }
  bool bind_guest_memory() {
    return resources.bind_guest_memory(mirror.buffer(),
                                       memory::kPhysicalMemorySize);
  }
  std::uint64_t pipeline_cache_misses() const { return pipelines.size(); }
  void add_backend_unsupported_counters(GpuUnsupportedCounters&) const {}
  bool record_draw(const DrawRecording& draw, bool& record_failed) {
    return queue.execute_async([&](VkCommandBuffer command_buffer,
                                   std::uint32_t frame_index,
                                   std::uint32_t draw_slot) {
      resources.prepare_draw(frame_index, draw_slot);
      const auto descriptor_set =
          resources.descriptor_set(frame_index, draw_slot);
      std::size_t index_offset = 0;
      if (!upload_draw_indices(draw, frame_index, draw_slot, index_offset)) {
        record_failed = true;
        return;
      }
      mirror.prepare_shader_access(command_buffer, draw.memexport_writable);
      for (std::size_t i = 0; i < draw.color_count; ++i)
        if (draw.targets[i])
          draw.targets[i]->transition_to_color_attachment(command_buffer);
      if (draw.depth)
        draw.depth->transition_to_depth_attachment(command_buffer);
      std::array<VkRenderingAttachmentInfo, 4> attachments{};
      for (std::size_t i = 0; i < draw.color_count; ++i) {
        attachments[i].sType =
            VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        if (draw.targets[i]) {
          attachments[i].imageView = draw.targets[i]->view();
          attachments[i].imageLayout =
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
          attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        } else {
          attachments[i].imageView = VK_NULL_HANDLE;
          attachments[i].imageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          attachments[i].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
          attachments[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
      }
      VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
      rendering.renderArea.extent = {draw.render_width, draw.render_height};
      rendering.layerCount = 1;
      rendering.colorAttachmentCount =
          static_cast<std::uint32_t>(draw.color_count);
      rendering.pColorAttachments =
          draw.color_count ? attachments.data() : nullptr;
      VkRenderingAttachmentInfo depth_attachment{
          VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
      if (draw.depth) {
        depth_attachment.imageView = draw.depth->view();
        depth_attachment.imageLayout =
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        rendering.pDepthAttachment = &depth_attachment;
        rendering.pStencilAttachment = &depth_attachment;
      }
      vkCmdBeginRendering(command_buffer, &rendering);
      vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        draw.pipeline.pipeline());
      vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              resources.pipeline_layout(), 0, 1,
                              &descriptor_set, 0, nullptr);
      const auto& guest_viewport = draw.state.raster.viewport;
      const bool has_viewport = std::abs(guest_viewport.x_scale) > 0.0001f &&
                                std::abs(guest_viewport.y_scale) > 0.0001f;
      const auto depth_a = (std::clamp)(guest_viewport.z_offset, 0.0f, 1.0f);
      const auto depth_b = (std::clamp)(
          guest_viewport.z_offset + guest_viewport.z_scale, 0.0f, 1.0f);
      const VkViewport viewport = has_viewport
          ? VkViewport{guest_viewport.x_offset - guest_viewport.x_scale,
                       guest_viewport.y_offset - guest_viewport.y_scale,
                       guest_viewport.x_scale * 2.0f,
                       guest_viewport.y_scale * 2.0f,
                       (std::min)(depth_a, depth_b), (std::max)(depth_a, depth_b)}
          : VkViewport{0.0f, 0.0f, float(draw.render_width),
                       float(draw.render_height), 0.0f, 1.0f};
      const VkRect2D scissor{{draw.scissor_left, draw.scissor_top},
                             {static_cast<std::uint32_t>(draw.scissor_right - draw.scissor_left),
                              static_cast<std::uint32_t>(draw.scissor_bottom - draw.scissor_top)}};
      vkCmdSetViewport(command_buffer, 0, 1, &viewport);
      vkCmdSetScissor(command_buffer, 0, 1, &scissor);
      vkCmdSetBlendConstants(command_buffer, draw.state.blend_constant.data());
      if (draw.depth && draw.state.depth_target.stencil_enabled) {
        vkCmdSetStencilCompareMask(
            command_buffer, VK_STENCIL_FACE_FRONT_BIT,
            draw.state.depth_target.stencil_read_mask);
        vkCmdSetStencilWriteMask(
            command_buffer, VK_STENCIL_FACE_FRONT_BIT,
            draw.state.depth_target.stencil_write_mask);
        vkCmdSetStencilReference(
            command_buffer, VK_STENCIL_FACE_FRONT_BIT,
            draw.state.depth_target.stencil_reference);
        const auto back_read = draw.state.depth_target.backface_stencil_enabled
                                   ? draw.state.depth_target.stencil_back_read_mask
                                   : draw.state.depth_target.stencil_read_mask;
        const auto back_write = draw.state.depth_target.backface_stencil_enabled
                                    ? draw.state.depth_target.stencil_back_write_mask
                                    : draw.state.depth_target.stencil_write_mask;
        const auto back_reference = draw.state.depth_target.backface_stencil_enabled
                                        ? draw.state.depth_target.stencil_back_reference
                                        : draw.state.depth_target.stencil_reference;
        vkCmdSetStencilCompareMask(command_buffer,
                                   VK_STENCIL_FACE_BACK_BIT, back_read);
        vkCmdSetStencilWriteMask(command_buffer,
                                 VK_STENCIL_FACE_BACK_BIT, back_write);
        vkCmdSetStencilReference(command_buffer,
                                 VK_STENCIL_FACE_BACK_BIT, back_reference);
      }
      if (draw.batch.indexed) {
        vkCmdBindIndexBuffer(
            command_buffer, transient_uploads[frame_index].buffer(),
            static_cast<VkDeviceSize>(index_offset), VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command_buffer,
                         static_cast<std::uint32_t>(draw.batch.indices.size()),
                         1, 0, 0, 0);
      } else {
        vkCmdDraw(command_buffer, draw.batch.vertex_count, 1, 0, 0);
      }
      vkCmdEndRendering(command_buffer);
    });
  }
};

Backend::Backend() : impl_(std::make_unique<Impl>()) {}
Backend::~Backend() = default;
bool Backend::initialize(const ContextConfig& config) {
  impl_ = std::make_unique<Impl>();
  if (!impl_->context.initialize(config)) {
    impl_->error = impl_->context.error();
    return false;
  }
  if (!impl_->queue.initialize(impl_->context.device(),
                               impl_->context.graphics_queue(),
                               impl_->context.graphics_queue_family())) {
    impl_->error = impl_->queue.error();
    return false;
  }
  if (!impl_->resources.initialize(impl_->context.physical_device(),
                                   impl_->context.device())) {
    impl_->error = impl_->resources.error();
    return false;
  }
  for (std::uint32_t i = 0; i < CommandQueue::kFrameCount; ++i) {
    if (!impl_->transient_uploads[i].initialize(
            impl_->context.physical_device(), impl_->context.device(),
            Impl::kTransientUploadBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
        !impl_->transient_uploads[i].map(impl_->transient_upload_mappings[i])) {
      impl_->error = impl_->transient_uploads[i].error();
      return false;
    }
  }
  impl_->ready = true;
  return true;
}
VkInstance Backend::instance() const noexcept {
  return impl_ ? impl_->context.instance() : VK_NULL_HANDLE;
}

bool Backend::configure_presentation(VkSurfaceKHR surface,
                                     const PresentationConfig& config,
                                     bool take_surface_ownership) {
  if (!impl_->ready) return false;
  if (!impl_->presentation.initialize(impl_->context, impl_->queue, surface,
                                      config, take_surface_ownership)) {
    impl_->error = impl_->presentation.error();
    return false;
  }
  return true;
}

void Backend::begin_submission(memory::AddressSpace& memory, Edram& edram) {
  impl_->begin_submission(memory, edram);
}
void Backend::consume(const ir::Command& command) {
  impl_->consume(command);
}
void Backend::end_submission() { impl_->end_submission(); }
bool Backend::make_guest_memory_cpu_visible(std::uint32_t physical_address,
                                            std::uint32_t size) {
  return impl_->make_guest_memory_cpu_visible(physical_address, size);
}

bool Backend::make_edram_canonical() {
  if (!impl_->ready || !impl_->edram) return false;
  if (!impl_->make_edram_canonical()) return false;
  return true;
}

bool Backend::invalidate_edram_native_state() {
  return impl_->invalidate_edram_native_state();
}

PresentStatus Backend::present(const PresentationFrame& frame) {
  return impl_->present(frame);
}

bool Backend::resize_presentation(std::uint32_t width, std::uint32_t height) {
  return impl_->resize_presentation(width, height);
}

bool Backend::presentation_ready() const noexcept {
  return impl_->presentation.ready();
}
GpuPerformanceCounters Backend::performance_counters() const noexcept {
  return impl_->performance_counters();
}

GpuUnsupportedCounters Backend::unsupported_counters() const noexcept {
  return impl_->unsupported_counters();
}

GpuShaderCoverage Backend::shader_coverage() const noexcept {
  return impl_->shader_coverage();
}
bool Backend::ready() const noexcept { return impl_->ready; }
std::size_t Backend::command_count() const noexcept { return impl_->command_count; }
std::size_t Backend::draw_count() const noexcept { return impl_->draw_count; }
std::size_t Backend::compiled_shader_count() const noexcept {
  return impl_->compiled_shader_count;
}
std::size_t Backend::pipeline_state_count() const noexcept {
  return impl_->pipeline_states.size();
}
bool Backend::resource_layout_ready() const noexcept {
  return impl_->resources.ready();
}
std::size_t Backend::realized_texture_count() const noexcept {
  return impl_->textures.size();
}
std::size_t Backend::realized_render_target_count() const noexcept {
  return impl_->render_targets.size();
}
const DeviceProperties& Backend::device_properties() const noexcept {
  return impl_->context.properties();
}
const std::string& Backend::error() const noexcept { return impl_->error; }

}  // namespace xenon::gpu::vulkan
