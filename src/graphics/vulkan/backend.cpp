#include "xenon/gpu/vulkan/backend.hpp"

#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <variant>
#include <unordered_set>
#include <unordered_map>

#include "xenon/gpu/vulkan/command_queue.hpp"
#include "xenon/gpu/vulkan/depth_target.hpp"
#include "xenon/gpu/vulkan/guest_memory_mirror.hpp"
#include "xenon/gpu/vulkan/pipeline.hpp"
#include "xenon/gpu/vulkan/resource_layout.hpp"
#include "xenon/gpu/vulkan/render_target.hpp"
#include "xenon/gpu/vulkan/texture.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/memory/types.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::vulkan {

class Backend::Impl {
 public:
  ~Impl() {
    if (memory && texture_callback) memory->remove_physical_write_callback(texture_callback);
  }
  Context context{};
  CommandQueue queue{};
  GuestMemoryMirror mirror{};
  ResourceLayout resources{};
  ResourceStateTracker resource_state{};
  std::unordered_set<std::uint64_t> pipeline_states{};
  std::unordered_map<std::uint64_t, std::unique_ptr<TextureImage>> textures{};
  std::unordered_map<std::uint64_t, std::unique_ptr<RenderTargetImage>> render_targets{};
  std::unordered_map<std::uint64_t, std::unique_ptr<DepthTargetImage>> depth_targets{};
  std::unordered_map<std::uint64_t, std::unique_ptr<GraphicsPipeline>> pipelines{};
  std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> shader_textures{};
  TextureDirtyTracker texture_dirty{};
  std::uint64_t texture_callback{};
  memory::AddressSpace* memory{};
  std::size_t command_count{};
  std::size_t draw_count{};
  std::size_t compiled_shader_count{};
#ifdef XENON_HAS_DXC
  std::shared_ptr<DxcShaderCompiler> compiler{std::make_shared<DxcShaderCompiler>()};
  ShaderCache shader_cache{compiler};
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>> shaders{};
#endif
  std::string error{};
  bool ready{};
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
  impl_->ready = true;
  return true;
}
void Backend::begin_submission(memory::AddressSpace& memory, Edram&) {
  impl_->command_count = 0;
  impl_->draw_count = 0;
  impl_->compiled_shader_count = 0;
  if (!impl_->ready) return;
  if (impl_->memory != &memory) {
    if (impl_->memory && impl_->texture_callback)
      impl_->memory->remove_physical_write_callback(impl_->texture_callback);
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
    auto* state = impl_.get();
    impl_->texture_callback = memory.add_physical_write_callback(
        [state](std::uint32_t address, std::uint32_t size) {
          state->texture_dirty.mark_dirty(address, size);
        });
    if (!impl_->resources.bind_guest_memory(impl_->mirror.buffer(),
                                            memory::kPhysicalMemorySize)) {
      impl_->error = impl_->resources.error();
      impl_->ready = false;
      return;
    }
  }
  if (!impl_->mirror.synchronize()) {
    impl_->error = impl_->mirror.error();
    impl_->ready = false;
  }
}
void Backend::consume(const ir::Command& command) {
  ++impl_->command_count;
  if (const auto* write = std::get_if<ir::RegisterWrite>(&command)) {
    impl_->resource_state.apply(*write);
  }
  if (const auto* draw = std::get_if<ir::DrawPacket>(&command)) {
    ++impl_->draw_count;
    const auto state = impl_->resource_state.snapshot();
    impl_->pipeline_states.insert(state.pipeline_hash(*draw));
    RenderTargetImage* active_target = nullptr;
    std::uint8_t active_write_mask = 0xFu;
    BlendState active_blend{};
    DepthTargetImage* active_depth = nullptr;
    if (state.edram_mode == EdramMode::ColorDepth &&
        state.raster.surface_pitch && state.raster.scissor_bottom > 0) {
      const auto height = std::uint32_t(state.raster.scissor_bottom);
      for (const auto& target : state.color_targets) {
        if (!target.enabled) continue;
        const auto format = static_cast<ColorRenderTargetFormat>(target.format);
        EdramSurfaceLayout surface{target.base_tile, state.raster.surface_pitch,
            height, static_cast<MsaaSamples>(state.raster.msaa_samples_log2),
            color_render_target_is_64bpp(format), false};
        const auto key = surface.hash() ^ (std::uint64_t(target.format) << 56u);
        if (!impl_->render_targets.contains(key)) {
          auto image = std::make_unique<RenderTargetImage>();
          if (!image->initialize(impl_->context.physical_device(),
                                 impl_->context.device(), impl_->queue,
                                 surface, format)) {
            impl_->error = image->error();
            continue;
          }
          impl_->render_targets.emplace(key, std::move(image));
        }
        if (!active_target) {
          active_target = impl_->render_targets.at(key).get();
          active_write_mask = target.write_mask;
          active_blend = target.blend;
        }
      }
    }
    if ((state.edram_mode == EdramMode::ColorDepth ||
         state.edram_mode == EdramMode::DepthOnly) &&
        state.raster.surface_pitch && state.raster.scissor_bottom > 0 &&
        (state.depth_target.test_enabled || state.depth_target.write_enabled ||
         state.depth_target.stencil_enabled)) {
      const EdramSurfaceLayout surface{
          state.depth_target.base_tile, state.raster.surface_pitch,
          static_cast<std::uint32_t>(state.raster.scissor_bottom),
          static_cast<MsaaSamples>(state.raster.msaa_samples_log2), false, true};
      const auto key = surface.hash() ^
                       (std::uint64_t(state.depth_target.format) << 60u);
      if (!impl_->depth_targets.contains(key)) {
        auto image = std::make_unique<DepthTargetImage>();
        if (!image->initialize(impl_->context.physical_device(),
                               impl_->context.device(), surface,
                               static_cast<DepthRenderTargetFormat>(
                                   state.depth_target.format))) {
          impl_->error = image->error();
        } else {
          impl_->depth_targets.emplace(key, std::move(image));
        }
      }
      if (const auto found = impl_->depth_targets.find(key);
          found != impl_->depth_targets.end())
        active_depth = found->second.get();
    }
    std::span<std::byte> constants;
    if (!impl_->resources.constants().map(constants) ||
        !impl_->resource_state.write_constant_buffer(constants)) {
      impl_->error = "Vulkan shader constant upload failed";
      impl_->ready = false;
    }
    std::array<bool, 32> used{};
    const auto mark_used = [&](const ir::ShaderReference& shader) {
      const auto found = impl_->shader_textures.find(shader.hash);
      if (shader.valid && found != impl_->shader_textures.end())
        for (const auto slot : found->second) if (slot < used.size()) used[slot] = true;
    };
    mark_used(draw->vertex_shader);
    mark_used(draw->pixel_shader);
    if (impl_->memory) {
      for (std::uint32_t slot = 0; slot < used.size(); ++slot) {
        if (!used[slot] || !state.textures[slot]) continue;
        const auto& descriptor = *state.textures[slot];
        const auto key = descriptor.hash();
        auto existing = impl_->textures.find(key);
        const bool refresh = existing == impl_->textures.end() ||
                             impl_->texture_dirty.consume_dirty(key);
        if (refresh) {
          const auto decoded = decode_texture(
              descriptor, {impl_->memory->physical_data(), memory::kPhysicalMemorySize});
          if (!decoded.valid) { impl_->error = decoded.error; continue; }
          auto image = std::make_unique<TextureImage>();
          if (!image->initialize(impl_->context.physical_device(), impl_->context.device(),
                                 impl_->queue, decoded) ||
              !impl_->resources.bind_texture(slot, descriptor.dimension,
                                             image->view(), image->sampler())) {
            impl_->error = image->error().empty() ? impl_->resources.error() : image->error();
            continue;
          }
          impl_->texture_dirty.track(key, decoded.layout);
          (void)impl_->texture_dirty.consume_dirty(key);
          impl_->textures[key] = std::move(image);
        } else if (!impl_->resources.bind_texture(slot, descriptor.dimension,
                                                  existing->second->view(),
                                                  existing->second->sampler())) {
          impl_->error = impl_->resources.error();
        }
      }
    }
#ifdef XENON_HAS_DXC
    if (active_target && impl_->memory && draw->vertex_shader.valid &&
        draw->pixel_shader.valid) {
      const auto vertex = impl_->shaders.find(draw->vertex_shader.hash);
      const auto pixel = impl_->shaders.find(draw->pixel_shader.hash);
      if (vertex == impl_->shaders.end() || pixel == impl_->shaders.end()) {
        impl_->error = "Vulkan draw references a shader that has not been compiled";
        return;
      }
      const auto batch = process_primitives(
          *draw, {impl_->memory->physical_data(), memory::kPhysicalMemorySize});
      if (!batch.valid) {
        impl_->error = batch.error;
        return;
      }
      const auto samples = static_cast<MsaaSamples>(state.raster.msaa_samples_log2);
      auto pipeline_key = state.pipeline_hash(*draw);
      pipeline_key ^= std::uint64_t(batch.topology) << 61u;
      pipeline_key ^= std::uint64_t(active_target->format()) << 24u;
      if (!impl_->pipelines.contains(pipeline_key)) {
        auto pipeline = std::make_unique<GraphicsPipeline>();
        const std::array formats{active_target->format()};
        const std::array write_masks{active_write_mask};
        const std::array blend_states{active_blend};
        if (!pipeline->initialize(
                impl_->context.device(), impl_->resources.pipeline_layout(),
                *vertex->second, *pixel->second, formats, samples,
                batch.topology, state.raster, write_masks, blend_states,
                state.blend_constant,
                active_depth ? active_depth->format() : VK_FORMAT_UNDEFINED,
                active_depth ? &state.depth_target : nullptr)) {
          impl_->error = pipeline->error();
          return;
        }
        impl_->pipelines.emplace(pipeline_key, std::move(pipeline));
      }
      Buffer index_buffer;
      if (batch.indexed) {
        const auto byte_size = batch.indices.size() * sizeof(std::uint32_t);
        if (!index_buffer.initialize(
                impl_->context.physical_device(), impl_->context.device(), byte_size,
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
          impl_->error = index_buffer.error();
          return;
        }
        std::span<std::byte> mapped;
        if (!index_buffer.map(mapped)) {
          impl_->error = index_buffer.error();
          return;
        }
        std::memcpy(mapped.data(), batch.indices.data(), byte_size);
        index_buffer.unmap();
      }
      const auto* pipeline = impl_->pipelines.at(pipeline_key).get();
      const auto descriptor_set = impl_->resources.descriptor_set();
      const auto scissor_left = std::max(0, state.raster.scissor_left);
      const auto scissor_top = std::max(0, state.raster.scissor_top);
      const auto scissor_right = std::clamp(
          state.raster.scissor_right, scissor_left,
          static_cast<std::int32_t>(active_target->width()));
      const auto scissor_bottom = std::clamp(
          state.raster.scissor_bottom, scissor_top,
          static_cast<std::int32_t>(active_target->height()));
      if (!impl_->queue.execute([&](VkCommandBuffer command_buffer) {
            active_target->transition_to_color_attachment(command_buffer);
            if (active_depth)
              active_depth->transition_to_depth_attachment(command_buffer);
            VkRenderingAttachmentInfo attachment{
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            attachment.imageView = active_target->view();
            attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
            attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
            rendering.renderArea.extent = {active_target->width(),
                                           active_target->height()};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount = 1;
            rendering.pColorAttachments = &attachment;
            VkRenderingAttachmentInfo depth_attachment{
                VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
            if (active_depth) {
              depth_attachment.imageView = active_depth->view();
              depth_attachment.imageLayout =
                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
              depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
              depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
              rendering.pDepthAttachment = &depth_attachment;
              rendering.pStencilAttachment = &depth_attachment;
            }
            vkCmdBeginRendering(command_buffer, &rendering);
            vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              pipeline->pipeline());
            vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    impl_->resources.pipeline_layout(), 0, 1,
                                    &descriptor_set, 0, nullptr);
            const auto& guest_viewport = state.raster.viewport;
            const bool has_viewport = std::abs(guest_viewport.x_scale) > 0.0001f &&
                                      std::abs(guest_viewport.y_scale) > 0.0001f;
            const auto depth_a = std::clamp(guest_viewport.z_offset, 0.0f, 1.0f);
            const auto depth_b = std::clamp(
                guest_viewport.z_offset + guest_viewport.z_scale, 0.0f, 1.0f);
            const VkViewport viewport = has_viewport
                ? VkViewport{guest_viewport.x_offset - guest_viewport.x_scale,
                             guest_viewport.y_offset - guest_viewport.y_scale,
                             guest_viewport.x_scale * 2.0f,
                             guest_viewport.y_scale * 2.0f,
                             std::min(depth_a, depth_b), std::max(depth_a, depth_b)}
                : VkViewport{0.0f, 0.0f, float(active_target->width()),
                             float(active_target->height()), 0.0f, 1.0f};
            const VkRect2D scissor{{scissor_left, scissor_top},
                                   {static_cast<std::uint32_t>(scissor_right - scissor_left),
                                    static_cast<std::uint32_t>(scissor_bottom - scissor_top)}};
            vkCmdSetViewport(command_buffer, 0, 1, &viewport);
            vkCmdSetScissor(command_buffer, 0, 1, &scissor);
            if (batch.indexed) {
              vkCmdBindIndexBuffer(command_buffer, index_buffer.buffer(), 0,
                                   VK_INDEX_TYPE_UINT32);
              vkCmdDrawIndexed(command_buffer,
                               static_cast<std::uint32_t>(batch.indices.size()),
                               1, 0, 0, 0);
            } else {
              vkCmdDraw(command_buffer, batch.vertex_count, 1, 0, 0);
            }
            vkCmdEndRendering(command_buffer);
          })) {
        impl_->error = impl_->queue.error();
      }
    }
#endif
  }
  if (const auto* load = std::get_if<ir::ShaderLoad>(&command)) {
    impl_->shader_textures[load->program.hash()] =
        load->decoded.reflection.texture_fetch_constants;
#ifdef XENON_HAS_DXC
    const auto lowered = HlslShaderLowerer::lower(load->decoded);
    ShaderCompileOptions options{};
    options.format = ShaderBinaryFormat::Spirv;
    const auto compiled = impl_->shader_cache.get_or_compile(lowered, options);
    if (compiled->succeeded) {
      ++impl_->compiled_shader_count;
      impl_->shaders[load->program.hash()] = compiled;
    } else {
      impl_->error = compiled->diagnostics.empty() ? "SPIR-V shader compilation failed"
                                                   : compiled->diagnostics.front();
    }
#endif
  }
}
void Backend::end_submission() {}
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
