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
  impl_->submission_started = std::chrono::steady_clock::now();
  ++impl_->performance.submissions;
  impl_->command_count = 0;
  impl_->draw_count = 0;
  impl_->compiled_shader_count = 0;
  if (!impl_->ready) return;
  if ((impl_->edram && impl_->edram != &edram) ||
      (impl_->memory && impl_->memory != &memory)) {
    if (!impl_->queue.wait_idle()) {
      impl_->error = impl_->queue.error();
      impl_->ready = false;
      return;
    }
  }
  if (impl_->edram != &edram) {
    impl_->render_targets.clear();
    impl_->depth_targets.clear();
    impl_->render_target_owners.clear();
    impl_->depth_target_owners.clear();
    impl_->owner_render_targets.clear();
    impl_->owner_depth_targets.clear();
    impl_->edram_ownership.reset();
    impl_->next_edram_owner = 1;
    impl_->edram = &edram;
  }
  if (impl_->memory != &memory) {
    impl_->textures.clear();
    impl_->texture_dirty.clear();
    if (!impl_->mirror.initialize(impl_->context.physical_device(),
                                  impl_->context.device(), impl_->queue,
                                  memory)) {
      impl_->error = impl_->mirror.error();
      impl_->ready = false;
      return;
    }
    impl_->memory = &memory;
    if (!impl_->resources.bind_guest_memory(impl_->mirror.buffer(),
                                            memory::kPhysicalMemorySize)) {
      impl_->error = impl_->resources.error();
      impl_->ready = false;
      return;
    }
  }
}
void Backend::consume(const ir::Command& command) {
  impl_->consume(command);
}
void Backend::end_submission() {
  impl_->performance.submission_time_ns += static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - impl_->submission_started).count());
  if (!impl_->ready) return;
  if (!impl_->queue.flush()) {
    impl_->error = impl_->queue.error();
    impl_->ready = false;
  }
}
bool Backend::make_guest_memory_cpu_visible(std::uint32_t physical_address,
                                            std::uint32_t size) {
  if (!impl_->ready || !impl_->memory) return false;
  if (!impl_->mirror.make_cpu_visible(physical_address, size)) {
    impl_->error = impl_->mirror.error();
    return false;
  }
  return true;
}

bool Backend::make_edram_canonical() {
  if (!impl_->ready || !impl_->edram) return false;
  if (!impl_->make_edram_canonical()) return false;
  return true;
}

bool Backend::invalidate_edram_native_state() {
  if (!impl_->ready || !impl_->edram) return false;
  if (!impl_->queue.wait_idle()) {
    impl_->error = impl_->queue.error();
    impl_->ready = false;
    return false;
  }
  // A portable capture has replaced the canonical byte store externally.
  // Forget only ownership: cached native images may be retained, but no tile
  // may remain authoritative until it is explicitly reacquired from canonical
  // EDRAM on the next draw/resolve.
  impl_->edram_ownership.reset();
  return true;
}


PresentStatus Backend::present(const PresentationFrame& frame) {
  if (!impl_->presentation.ready()) return PresentStatus::NotConfigured;
  if (!impl_->ready || !impl_->memory) return PresentStatus::Error;
  TextureDescriptor descriptor = frame.texture;
  descriptor.mip_min_level = 0;
  descriptor.mip_max_level = 0;
  descriptor.packed_mips = false;
  const auto layout = build_texture_layout(descriptor);
  if (!layout.valid) {
    impl_->error = layout.error;
    return PresentStatus::Error;
  }
  for (const auto& subresource : layout.subresources) {
    if (subresource.guest_size_bytes > UINT32_MAX ||
        !impl_->mirror.make_cpu_visible(
            subresource.guest_address,
            static_cast<std::uint32_t>(subresource.guest_size_bytes),
            memory::GpuRangeUsage::RenderReadback)) {
      impl_->error = impl_->mirror.error().empty()
                         ? "Vulkan scanout source range is invalid"
                         : impl_->mirror.error();
      return PresentStatus::Error;
    }
  }
  std::uint32_t presentation_snapshot_base = memory::kPhysicalMemorySize;
  std::uint64_t presentation_snapshot_end = 0u;
  for (const auto& subresource : layout.subresources) {
    presentation_snapshot_base =
        std::min(presentation_snapshot_base, subresource.guest_address);
    presentation_snapshot_end = std::max(
        presentation_snapshot_end,
        std::uint64_t{subresource.guest_address} +
            subresource.guest_size_bytes);
  }
  if (presentation_snapshot_base >= memory::kPhysicalMemorySize ||
      presentation_snapshot_end > memory::kPhysicalMemorySize ||
      presentation_snapshot_end <= presentation_snapshot_base) {
    impl_->error = "Vulkan presentation snapshot range is invalid";
    return PresentStatus::Error;
  }
  std::vector<std::byte> presentation_snapshot(
      static_cast<std::size_t>(presentation_snapshot_end -
                               presentation_snapshot_base));
  if (!impl_->memory->copy_physical_range(presentation_snapshot_base,
                                           presentation_snapshot)) {
    impl_->error = "Vulkan presentation snapshot failed";
    return PresentStatus::Error;
  }
  const auto prepared = prepare_presentation_frame(
      frame, presentation_snapshot, impl_->presentation.width(),
      impl_->presentation.height(),
      impl_->presentation.config().preserve_aspect_ratio,
      presentation_snapshot_base);
  if (!prepared.valid) {
    impl_->error = prepared.error;
    return PresentStatus::Unsupported;
  }
  const auto status = impl_->presentation.present(prepared);
  if (status == PresentStatus::Error || status == PresentStatus::SurfaceLost)
    impl_->error = impl_->presentation.error();
  return status;
}

bool Backend::resize_presentation(std::uint32_t width, std::uint32_t height) {
  if (!impl_->presentation.resize(width, height)) {
    impl_->error = impl_->presentation.error();
    return false;
  }
  return true;
}

bool Backend::presentation_ready() const noexcept {
  return impl_->presentation.ready();
}
GpuPerformanceCounters Backend::performance_counters() const noexcept {
  auto result = impl_->performance;
  result.shader_cache_misses = impl_->compiled_shader_count;
  result.pipeline_cache_misses = impl_->pipelines.size();
  return result;
}

GpuUnsupportedCounters Backend::unsupported_counters() const noexcept {
  return impl_->unsupported;
}

GpuShaderCoverage Backend::shader_coverage() const noexcept {
  GpuShaderCoverage coverage{};
  coverage.shaders_discovered = impl_->decoded_shaders.size();
  coverage.translation_failures = impl_->shader_translation_failures;
  coverage.shaders_translated =
      coverage.shaders_discovered >= coverage.translation_failures
          ? coverage.shaders_discovered - coverage.translation_failures
          : 0u;
  coverage.cache_hits = impl_->shader_cache.hits();
  coverage.cache_misses = impl_->shader_cache.misses();
  return coverage;
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
