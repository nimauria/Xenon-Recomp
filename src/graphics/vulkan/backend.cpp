#include "xenon/gpu/vulkan/backend.hpp"

#include <array>
#include <algorithm>
#include <cstring>
#include <cmath>
#include <variant>
#include <span>
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
#include "xenon/gpu/edram_ownership.hpp"
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
  static constexpr std::size_t kTransientUploadBytes = 16u * 1024u * 1024u;
  ~Impl() = default;
  Context context{};
  CommandQueue queue{};
  GuestMemoryMirror mirror{};
  ResourceLayout resources{};
  Buffer transient_upload{};
  std::span<std::byte> transient_upload_mapping{};
  std::size_t transient_upload_offset{};
  ResourceStateTracker resource_state{};
  std::unordered_set<std::uint64_t> pipeline_states{};
  std::unordered_map<std::uint64_t, std::unique_ptr<TextureImage>> textures{};
  std::unordered_map<std::uint64_t, std::unique_ptr<RenderTargetImage>> render_targets{};
  std::unordered_map<std::uint64_t, EdramOwnerId> render_target_owners{};
  std::unordered_map<EdramOwnerId, RenderTargetImage*> owner_render_targets{};
  std::unordered_map<std::uint64_t, std::unique_ptr<DepthTargetImage>> depth_targets{};
  std::unordered_map<std::uint64_t, EdramOwnerId> depth_target_owners{};
  std::unordered_map<EdramOwnerId, DepthTargetImage*> owner_depth_targets{};
  std::unordered_map<std::uint64_t, std::unique_ptr<GraphicsPipeline>> pipelines{};
  std::unordered_map<std::uint64_t, std::vector<std::uint8_t>> shader_textures{};
  TextureDirtyTracker texture_dirty{};
  memory::AddressSpace* memory{};
  Edram* edram{};
  EdramOwnershipTracker edram_ownership{};
  EdramOwnerId next_edram_owner{1};
  std::size_t command_count{};
  std::size_t draw_count{};
  std::size_t compiled_shader_count{};
#ifdef XENON_HAS_DXC
  std::shared_ptr<DxcShaderCompiler> compiler{std::make_shared<DxcShaderCompiler>()};
  ShaderCache shader_cache{compiler};
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>> shaders{};
  std::unordered_map<std::uint64_t, DecodedShader> decoded_shaders{};
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>>
      float20_depth_shaders{};
  std::shared_ptr<const CompiledShader> rectangle_list_shader{};
#endif
  std::string error{};
  bool ready{};

  bool flush_color_owner(EdramOwnerId owner) {
    const auto found = owner_render_targets.find(owner);
    if (!edram || found == owner_render_targets.end() || !found->second) {
      error = "Vulkan EDRAM alias transfer has no native color owner";
      return false;
    }
    auto* image = found->second;
    const auto& surface = image->surface();
    const auto canonical_pitch = image->width() *
        (surface.is_64bpp ? 8u : 4u);
    std::vector<std::byte> canonical(
        std::size_t(canonical_pitch) * image->height());
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
      std::vector<std::byte> pixels;
      std::uint32_t pitch{};
      if (!image->readback_sample(queue, sample, 0, 0, image->width(),
                                  image->height(), pixels, pitch) ||
          !host_color_to_edram(image->guest_format(), image->width(),
                               image->height(), pixels, pitch, canonical,
                               canonical_pitch) ||
          !store_edram_raw(*edram, surface, 0, 0, image->width(),
                           image->height(), sample, canonical,
                           canonical_pitch)) {
        error = image->error().empty()
                    ? "Vulkan EDRAM ownership sample readback failed"
                    : image->error();
        return false;
      }
    }
    edram_ownership.release(owner);
    return true;
  }

  bool flush_depth_owner(EdramOwnerId owner) {
    const auto found = owner_depth_targets.find(owner);
    if (!edram || found == owner_depth_targets.end() || !found->second) {
      error = "Vulkan EDRAM alias transfer has no native depth owner";
      return false;
    }
    auto* image = found->second;
    const auto& surface = image->surface();
    const auto canonical_pitch = image->width() * 4u;
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
      std::vector<std::uint32_t> pixels;
      std::uint32_t pitch{};
      if (!image->readback_sample(queue, sample, 0, 0, image->width(),
                                  image->height(), pixels, pitch) ||
          !store_edram_raw(*edram, surface, 0, 0, image->width(),
                           image->height(), sample, std::as_bytes(std::span(pixels)),
                           canonical_pitch)) {
        error = image->error().empty()
                    ? "Vulkan EDRAM depth ownership sample readback failed"
                    : image->error();
        return false;
      }
    }
    edram_ownership.release(owner);
    return true;
  }

  bool flush_owner(EdramOwnerId owner) {
    if (owner_render_targets.contains(owner)) return flush_color_owner(owner);
    if (owner_depth_targets.contains(owner)) return flush_depth_owner(owner);
    error = "Vulkan EDRAM alias transfer has no native owner";
    return false;
  }

  bool acquire_color_ownership(std::uint64_t key,
                               RenderTargetImage& requested,
                               const EdramSurfaceLayout& ownership_surface) {
    const auto owner_it = render_target_owners.find(key);
    if (!edram || owner_it == render_target_owners.end()) {
      error = "Vulkan EDRAM color surface has no ownership identity";
      return false;
    }
    const auto requested_owner = owner_it->second;
    auto plan = edram_ownership.plan(requested_owner, ownership_surface);
    if (!plan.valid || plan.changes.empty()) return plan.valid;

    if (edram_ownership.owned_tile_count(requested_owner) &&
        !flush_color_owner(requested_owner)) return false;
    plan = edram_ownership.plan(requested_owner, ownership_surface);
    for (const auto& change : plan.changes) {
      if (change.previous_owner != kCanonicalEdramOwner &&
           !flush_owner(change.previous_owner)) return false;
    }
    plan = edram_ownership.plan(requested_owner, ownership_surface);
    if (plan.requires_preservation()) {
      error = "Vulkan EDRAM ownership transfer did not reach canonical storage";
      return false;
    }

    const auto& surface = requested.surface();
    const auto canonical_pitch = requested.width() *
        (surface.is_64bpp ? 8u : 4u);
    const auto host_pitch = requested.width() *
        color_host_bytes_per_pixel(requested.guest_format());
    std::vector<std::byte> canonical(
        std::size_t(canonical_pitch) * requested.height());
    std::vector<std::byte> pixels(
        std::size_t(host_pitch) * requested.height());
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
      if (!resolve_edram_raw(*edram, surface, 0, 0, requested.width(),
                             requested.height(), sample, canonical,
                             canonical_pitch) ||
          !edram_color_to_host(requested.guest_format(), requested.width(),
                               requested.height(), canonical,
                               canonical_pitch, pixels, host_pitch) ||
          !requested.upload_sample(queue, sample, 0, 0, requested.width(),
                                   requested.height(), pixels, host_pitch)) {
        error = requested.error().empty()
                    ? "Vulkan canonical EDRAM sample upload failed"
                    : requested.error();
        return false;
      }
    }
    if (!edram_ownership.commit(plan)) {
      error = "Vulkan EDRAM ownership plan became stale";
      return false;
    }
    return true;
  }

  bool acquire_depth_ownership(std::uint64_t key,
                               DepthTargetImage& requested,
                               const EdramSurfaceLayout& ownership_surface) {
    const auto owner_it = depth_target_owners.find(key);
    if (!edram || owner_it == depth_target_owners.end()) {
      error = "Vulkan EDRAM depth surface has no ownership identity";
      return false;
    }
    const auto owner = owner_it->second;
    auto plan = edram_ownership.plan(owner, ownership_surface);
    if (!plan.valid || plan.changes.empty()) return plan.valid;
    if (edram_ownership.owned_tile_count(owner) && !flush_depth_owner(owner)) {
      return false;
    }
    plan = edram_ownership.plan(owner, ownership_surface);
    for (const auto& change : plan.changes) {
      if (change.previous_owner != kCanonicalEdramOwner &&
          !flush_owner(change.previous_owner)) return false;
    }
    plan = edram_ownership.plan(owner, ownership_surface);
    if (plan.requires_preservation()) {
      error = "Vulkan depth EDRAM ownership transfer did not reach canonical storage";
      return false;
    }
    const auto& surface = requested.surface();
    const auto canonical_pitch = requested.width() * 4u;
    std::vector<std::uint32_t> pixels(
        std::size_t(requested.width()) * requested.height());
    const auto sample_count = 1u << static_cast<unsigned>(surface.msaa);
    for (std::uint32_t sample = 0; sample < sample_count; ++sample) {
      if (!resolve_edram_raw(*edram, surface, 0, 0, requested.width(),
                             requested.height(), sample,
                             std::as_writable_bytes(std::span(pixels)),
                             canonical_pitch) ||
          !requested.upload_sample(queue, sample, 0, 0, requested.width(),
                                   requested.height(), pixels,
                                   canonical_pitch)) {
        error = requested.error().empty()
                    ? "Vulkan canonical EDRAM depth sample upload failed"
                    : requested.error();
        return false;
      }
    }
    if (!edram_ownership.commit(plan)) {
      error = "Vulkan depth EDRAM ownership plan became stale";
      return false;
    }
    return true;
  }

  bool make_region_canonical(const EdramSurfaceLayout& surface,
                             EdramSurfaceRegion region) {
    if (!edram || !surface.valid()) {
      error = "Vulkan Xenos resolve clear has an invalid EDRAM surface";
      return false;
    }
    std::unordered_set<EdramOwnerId> owners;
    for (const auto tile : covered_edram_tiles(surface, region)) {
      const auto owner = edram_ownership.owner(tile);
      if (owner != kCanonicalEdramOwner) owners.insert(owner);
    }
    for (const auto owner : owners)
      if (!flush_owner(owner)) return false;
    return true;
  }

  bool clear_depth_resolve_region(const DrawResourceState& state,
                                  const ResolveRectangle& rectangle,
                                  MsaaSamples samples) {
    if (!state.copy.depth_clear_enabled) return true;
    if (!state.raster.surface_pitch || rectangle.bottom <= 0) {
      error = "Vulkan Xenos post-resolve depth clear has an invalid surface extent";
      return false;
    }
    const EdramSurfaceLayout surface{
        state.depth_target.base_tile, state.raster.surface_pitch,
        static_cast<std::uint32_t>(rectangle.bottom), samples, false, true};
    const EdramSurfaceRegion region{rectangle.left, rectangle.top,
                                     rectangle.right, rectangle.bottom};
    if (!make_region_canonical(surface, region)) return false;
    if (!clear_edram_surface_region(*edram, surface, rectangle.left,
                                    rectangle.top, rectangle.right,
                                    rectangle.bottom,
                                    {state.copy.depth_clear, 0u})) {
      error = "Vulkan regional depth resolve clear failed";
      return false;
    }
    edram_ownership.make_canonical_region(surface, region);
    return true;
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
  if (!impl_->transient_upload.initialize(
          impl_->context.physical_device(), impl_->context.device(),
          Impl::kTransientUploadBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ||
      !impl_->transient_upload.map(impl_->transient_upload_mapping)) {
    impl_->error = impl_->transient_upload.error();
    return false;
  }
  impl_->ready = true;
  return true;
}
void Backend::begin_submission(memory::AddressSpace& memory, Edram& edram) {
  impl_->command_count = 0;
  impl_->draw_count = 0;
  impl_->compiled_shader_count = 0;
  impl_->transient_upload_offset = 0;
  if (!impl_->ready) return;
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
    if (state.edram_mode == EdramMode::Copy) {
      if (!impl_->memory || !impl_->memory->physical_data()) {
        impl_->error = "Vulkan Xenos resolve has no bound guest memory";
        return;
      }
      const auto resolve_plan = plan_resolve(
          state, {impl_->memory->physical_data(), memory::kPhysicalMemorySize});
      if (!resolve_plan.valid) {
        impl_->error = "Vulkan " + resolve_plan.error;
        return;
      }
      const auto& rectangle = resolve_plan.rectangle;
      if (rectangle.empty()) return;
      if (state.copy.command != CopyCommand::Raw &&
          state.copy.command != CopyCommand::Convert) {
        impl_->error = "Vulkan Xenos resolve command is unsupported";
        return;
      }
      const auto samples = resolve_plan.samples;
      if (resolve_plan.depth) {
        const auto format = static_cast<DepthRenderTargetFormat>(
            state.depth_target.format);
        DepthTargetImage* source = nullptr;
        std::uint64_t source_key{};
        for (auto& [key, candidate] : impl_->depth_targets) {
          const auto& surface = candidate->surface();
          if (surface.base_tile != state.depth_target.base_tile ||
              surface.pitch_pixels != state.raster.surface_pitch ||
              surface.msaa != samples || !surface.depth ||
              candidate->guest_format() != format ||
              candidate->height() <
                  static_cast<std::uint32_t>(rectangle.bottom)) {
            continue;
          }
          if (!source || candidate->height() < source->height()) {
            source = candidate.get();
            source_key = key;
          }
        }
        if (!source) {
          impl_->error =
              "Vulkan Xenos depth resolve source EDRAM surface is unavailable";
          return;
        }
        if (!impl_->acquire_depth_ownership(source_key, *source,
                                             source->surface())) return;
        if (resolve_plan.selected_sample_count != 1) {
          impl_->error = "Vulkan Xenos depth resolve selected multiple samples";
          return;
        }
        std::uint32_t guest_sample{};
        while (guest_sample < 4 &&
               !(resolve_plan.guest_sample_mask & (1u << guest_sample))) {
          ++guest_sample;
        }
        if (guest_sample >= 4) {
          impl_->error = "Vulkan Xenos depth resolve selected no sample";
          return;
        }
        std::vector<std::uint32_t> readback;
        std::uint32_t row_pitch{};
        if (!source->readback_sample(
                impl_->queue, guest_sample, rectangle.left, rectangle.top,
                rectangle.right, rectangle.bottom, readback, row_pitch)) {
          impl_->error = source->error();
          return;
        }
        const auto memory_span = std::span<std::byte>(
            impl_->memory->physical_data(), memory::kPhysicalMemorySize);
        const auto written = write_depth_resolve(
            resolve_plan.copy, format, rectangle, readback, row_pitch,
            memory_span);
        if (!written.valid) {
          impl_->error = written.error;
          return;
        }
        impl_->memory->notify_external_write(written.modified_address,
                                             written.modified_size);
        if (!impl_->clear_depth_resolve_region(state, rectangle, samples))
          return;
        return;
      }
      const auto source_slot =
          static_cast<std::size_t>(resolve_plan.source_color_slot);
      const auto& target = state.color_targets[source_slot];
      const auto format = static_cast<ColorRenderTargetFormat>(target.format);
      const auto raw_format = raw_resolve_texture_format(format);
      if (state.copy.command == CopyCommand::Raw &&
          (!raw_format || *raw_format != state.copy.destination_format)) {
        impl_->error = "Vulkan Xenos raw resolve source and destination formats differ";
        return;
      }
      RenderTargetImage* source = nullptr;
      std::uint64_t source_key{};
      for (auto& [key, candidate] : impl_->render_targets) {
        const auto& surface = candidate->surface();
        if (surface.base_tile != target.base_tile ||
            surface.pitch_pixels != state.raster.surface_pitch ||
            surface.msaa != samples || surface.depth ||
            surface.is_64bpp != color_render_target_is_64bpp(format) ||
            candidate->format() != color_render_target_format(format) ||
            candidate->height() < static_cast<std::uint32_t>(rectangle.bottom)) {
          continue;
        }
        if (!source || candidate->height() < source->height()) {
          source = candidate.get();
          source_key = key;
        }
      }
      if (!source) {
        impl_->error = "Vulkan Xenos resolve source EDRAM surface is unavailable";
        return;
      }
      if (!impl_->acquire_color_ownership(source_key, *source,
                                           source->surface())) return;
      std::vector<std::byte> readback;
      std::uint32_t row_pitch{};
      if (resolve_plan.native_color_average) {
        if (!source->readback(impl_->queue, rectangle.left, rectangle.top,
                              rectangle.right, rectangle.bottom, readback,
                              row_pitch)) {
          impl_->error = source->error();
          return;
        }
      } else {
        std::array<std::uint32_t, 2> selected_samples{};
        std::uint32_t selected_count{};
        for (std::uint32_t guest_sample = 0; guest_sample < 4; ++guest_sample) {
          if (resolve_plan.guest_sample_mask & (1u << guest_sample)) {
            if (selected_count < selected_samples.size())
              selected_samples[selected_count] = guest_sample;
            ++selected_count;
          }
        }
        if (selected_count == 1) {
          if (!source->readback_sample(
                  impl_->queue, selected_samples[0], rectangle.left,
                  rectangle.top, rectangle.right, rectangle.bottom, readback,
                  row_pitch)) {
            impl_->error = source->error();
            return;
          }
        } else if (selected_count == 2) {
          std::vector<std::byte> first;
          std::vector<std::byte> second;
          std::uint32_t first_pitch{};
          std::uint32_t second_pitch{};
          if (!source->readback_sample(
                  impl_->queue, selected_samples[0], rectangle.left,
                  rectangle.top, rectangle.right, rectangle.bottom, first,
                  first_pitch) ||
              !source->readback_sample(
                  impl_->queue, selected_samples[1], rectangle.left,
                  rectangle.top, rectangle.right, rectangle.bottom, second,
                  second_pitch)) {
            impl_->error = source->error();
            return;
          }
          const auto width = static_cast<std::uint32_t>(rectangle.right -
                                                        rectangle.left);
          const auto height = static_cast<std::uint32_t>(rectangle.bottom -
                                                         rectangle.top);
          if (!average_host_color_samples(format, width, height, first,
                                          first_pitch, second, second_pitch,
                                          readback, row_pitch)) {
            impl_->error = "Vulkan Xenos selected-sample resolve averaging failed";
            return;
          }
        } else {
          impl_->error = "Vulkan Xenos resolve selected an unsupported sample set";
          return;
        }
      }
      const auto memory_span = std::span<std::byte>(
          impl_->memory->physical_data(), memory::kPhysicalMemorySize);
      const auto written = state.copy.command == CopyCommand::Convert ||
                                   color_host_requires_conversion(format)
          ? write_converted_resolve(state.copy, format, rectangle, readback,
                                    row_pitch, memory_span)
          : write_raw_resolve(state.copy, rectangle, readback, row_pitch,
                              memory_span);
      if (!written.valid) {
        impl_->error = written.error;
        return;
      }
      impl_->memory->notify_external_write(written.modified_address,
                                           written.modified_size);
      if (state.copy.color_clear_enabled) {
        const auto owner = impl_->render_target_owners.find(source_key);
        if (owner == impl_->render_target_owners.end() ||
            !impl_->flush_color_owner(owner->second) ||
            !clear_edram_surface_region(
                *impl_->edram, source->surface(), rectangle.left,
                rectangle.top, rectangle.right, rectangle.bottom,
                state.copy.color_clear)) {
          if (impl_->error.empty())
            impl_->error = "Vulkan regional color resolve clear failed";
          return;
        }
        impl_->edram_ownership.make_canonical_region(
            source->surface(), {rectangle.left, rectangle.top,
                                rectangle.right, rectangle.bottom});
      }
      if (!impl_->clear_depth_resolve_region(state, rectangle, samples)) return;
      return;
    }
    impl_->pipeline_states.insert(state.pipeline_hash(*draw));
    std::array<RenderTargetImage*, 4> active_targets{};
    std::array<std::uint8_t, 4> active_write_masks{};
    std::array<BlendState, 4> active_blends{};
    const auto color_plan = plan_color_targets(state);
    const auto color_count =
        static_cast<std::size_t>(color_plan.attachment_count);
    DepthTargetImage* active_depth = nullptr;
    if (color_count && (!state.raster.surface_pitch ||
                        state.raster.scissor_bottom <= 0)) {
      impl_->error = "Vulkan Xenos MRT draw has an invalid EDRAM surface extent";
      return;
    }
    if (color_count && state.raster.surface_pitch &&
        state.raster.scissor_bottom > 0) {
      const auto height = std::uint32_t(state.raster.scissor_bottom);
      for (std::size_t slot = 0; slot < color_count; ++slot) {
        if (!color_plan.enabled[slot]) continue;
        const auto& target = state.color_targets[slot];
        const auto format = static_cast<ColorRenderTargetFormat>(target.format);
        EdramSurfaceLayout surface{target.base_tile, state.raster.surface_pitch,
            height, static_cast<MsaaSamples>(state.raster.msaa_samples_log2),
            color_render_target_is_64bpp(format), false};
        const auto key = surface.identity_hash() ^
                         (std::uint64_t(target.format) << 56u);
        auto existing = impl_->render_targets.find(key);
        if (existing != impl_->render_targets.end() &&
            existing->second->height() < height) {
          const auto owner = impl_->render_target_owners.at(key);
          if (impl_->edram_ownership.owned_tile_count(owner) &&
              !impl_->flush_color_owner(owner)) return;
          impl_->owner_render_targets.erase(owner);
          impl_->render_target_owners.erase(key);
          impl_->render_targets.erase(existing);
        }
        if (!impl_->render_targets.contains(key)) {
          auto image = std::make_unique<RenderTargetImage>();
          if (!image->initialize(impl_->context.physical_device(),
                                 impl_->context.device(), impl_->queue,
                                 surface, format)) {
            impl_->error = image->error();
            return;
          }
          auto* image_pointer = image.get();
          impl_->render_targets.emplace(key, std::move(image));
          const auto owner = impl_->next_edram_owner++;
          impl_->render_target_owners.emplace(key, owner);
          impl_->owner_render_targets.emplace(owner, image_pointer);
        }
        active_targets[slot] = impl_->render_targets.at(key).get();
        if (!impl_->acquire_color_ownership(key, *active_targets[slot],
                                            surface)) return;
        active_write_masks[slot] = target.write_mask;
        active_blends[slot] = target.blend;
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
      const auto key = surface.identity_hash() ^
                       (std::uint64_t(state.depth_target.format) << 60u);
      if (const auto existing = impl_->depth_targets.find(key);
          existing != impl_->depth_targets.end() &&
          existing->second->height() < surface.height_pixels) {
        const auto owner = impl_->depth_target_owners.at(key);
        if (impl_->edram_ownership.owned_tile_count(owner) &&
            !impl_->flush_depth_owner(owner)) return;
        impl_->owner_depth_targets.erase(owner);
        impl_->depth_target_owners.erase(key);
        impl_->depth_targets.erase(existing);
      }
      if (!impl_->depth_targets.contains(key)) {
        auto image = std::make_unique<DepthTargetImage>();
        if (!image->initialize(impl_->context.physical_device(),
                               impl_->context.device(), surface,
                               static_cast<DepthRenderTargetFormat>(
                                   state.depth_target.format))) {
          impl_->error = image->error();
          return;
        } else {
          auto* image_pointer = image.get();
          impl_->depth_targets.emplace(key, std::move(image));
          const auto owner = impl_->next_edram_owner++;
          impl_->depth_target_owners.emplace(key, owner);
          impl_->owner_depth_targets.emplace(owner, image_pointer);
        }
      }
      if (const auto found = impl_->depth_targets.find(key);
          found != impl_->depth_targets.end())
        active_depth = found->second.get();
      if (active_depth &&
          !impl_->acquire_depth_ownership(key, *active_depth, surface)) return;
    }
    std::span<std::byte> constants;
    if (!impl_->resources.constants().map(constants) ||
        !impl_->resource_state.write_constant_buffer(constants)) {
      impl_->error = "Vulkan shader constant upload failed";
      impl_->ready = false;
      return;
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
        const auto texture_epoch = impl_->memory->coherency().current_epoch();
        const bool refresh = existing == impl_->textures.end() ||
                             impl_->texture_dirty.consume_dirty(
                                 key, impl_->memory->coherency(), texture_epoch);
        if (refresh) {
          const auto decoded = decode_texture(
              descriptor, {impl_->memory->physical_data(), memory::kPhysicalMemorySize});
          if (!decoded.valid) {
            impl_->error = decoded.error;
            return;
          }
          auto image = std::make_unique<TextureImage>();
          if (!image->initialize(impl_->context.physical_device(), impl_->context.device(),
                                 impl_->queue, decoded) ||
              !impl_->resources.bind_texture(slot, descriptor.dimension,
                                             image->view(), image->sampler())) {
            impl_->error = image->error().empty() ? impl_->resources.error() : image->error();
            return;
          }
          impl_->texture_dirty.track_clean(key, decoded.layout, texture_epoch);
          impl_->textures[key] = std::move(image);
        } else if (!impl_->resources.bind_texture(slot, descriptor.dimension,
                                                  existing->second->view(),
                                                  existing->second->sampler())) {
          impl_->error = impl_->resources.error();
          return;
        }
      }
    }
#ifdef XENON_HAS_DXC
    if ((color_count || active_depth) && impl_->memory &&
        draw->vertex_shader.valid && draw->pixel_shader.valid) {
      const auto vertex = impl_->shaders.find(draw->vertex_shader.hash);
      const auto pixel = impl_->shaders.find(draw->pixel_shader.hash);
      if (vertex == impl_->shaders.end() || pixel == impl_->shaders.end()) {
        impl_->error = "Vulkan draw references a shader that has not been compiled";
        return;
      }

      auto pixel_shader = pixel->second;
      bool float20_depth_variant = false;
      const auto samples = static_cast<MsaaSamples>(state.raster.msaa_samples_log2);
      if (active_depth && active_depth->requires_float24_conversion() &&
          (state.depth_target.test_enabled || state.depth_target.write_enabled)) {
        const bool per_sample = samples != MsaaSamples::X1;
        const std::uint64_t variant_key =
            draw->pixel_shader.hash ^ 0xD24F20E4D24F20E4ull ^
            (per_sample ? 0x8000000000000000ull : 0ull);
        auto variant = impl_->float20_depth_shaders.find(variant_key);
        if (variant == impl_->float20_depth_shaders.end()) {
          const auto decoded = impl_->decoded_shaders.find(draw->pixel_shader.hash);
          if (decoded == impl_->decoded_shaders.end()) {
            impl_->error = "Vulkan D24FS8 draw lacks decoded pixel shader";
            return;
          }
          ShaderLoweringOptions lowering_options{};
          lowering_options.pixel_depth_output =
              PixelDepthOutputMode::Float20e4NearestEven;
          lowering_options.force_sample_frequency = per_sample;
          const auto lowered =
              HlslShaderLowerer::lower(decoded->second, lowering_options);
          if (!lowered.complete) {
            impl_->error = lowered.diagnostics.empty()
                               ? "D24FS8 pixel shader lowering failed"
                               : lowered.diagnostics.front();
            return;
          }
          ShaderCompileOptions compile_options{};
          compile_options.format = ShaderBinaryFormat::Spirv;
          const auto compiled =
              impl_->shader_cache.get_or_compile(lowered, compile_options);
          if (!compiled->succeeded) {
            impl_->error = compiled->diagnostics.empty()
                               ? "D24FS8 pixel shader compilation failed"
                               : compiled->diagnostics.front();
            return;
          }
          ++impl_->compiled_shader_count;
          variant = impl_->float20_depth_shaders
                        .emplace(variant_key, compiled)
                        .first;
        }
        pixel_shader = variant->second;
        float20_depth_variant = true;
      }

      const PrimitiveProcessingOptions primitive_options{
          state.primitive_assembly.reset_enabled,
          state.primitive_assembly.reset_index};
      const auto batch = process_primitives(
          *draw, {impl_->memory->physical_data(), memory::kPhysicalMemorySize},
          primitive_options);
      if (!batch.valid) {
        impl_->error = batch.error;
        return;
      }
      if (batch.requires_rectangle_expansion &&
          !impl_->rectangle_list_shader) {
        ShaderCompileOptions options{};
        options.format = ShaderBinaryFormat::Spirv;
        impl_->rectangle_list_shader = impl_->shader_cache.get_or_compile(
            make_rectangle_list_geometry_shader(), options);
        if (!impl_->rectangle_list_shader->succeeded) {
          impl_->error = impl_->rectangle_list_shader->diagnostics.empty()
                             ? "Vulkan RectangleList shader compilation failed"
                             : impl_->rectangle_list_shader->diagnostics.front();
          return;
        }
        ++impl_->compiled_shader_count;
      }
      // D3D12 has no FRONT_AND_BACK cull mode and Vulkan does, but the common
      // result is simpler: once Xenos has requested both faces culled, no
      // triangle can reach rasterization. Points and lines are unaffected by
      // polygon face culling.
      if (batch.topology == HostPrimitiveTopology::TriangleList &&
          state.raster.cull_front && state.raster.cull_back) {
        return;
      }
      auto pipeline_key = state.pipeline_hash(*draw);
      pipeline_key ^= std::uint64_t(batch.topology) << 61u;
      if (batch.requires_rectangle_expansion)
        pipeline_key ^= 0x52454354414E474Cull;
      if (color_count) {
        for (std::size_t i = 0; i < color_count; ++i) {
          const auto format = active_targets[i] ? active_targets[i]->format()
                                                : VK_FORMAT_UNDEFINED;
          pipeline_key ^= std::uint64_t(format) << (16u + i * 8u);
        }
      } else {
        pipeline_key ^= 0xD3F7000000000000ull;
      }
      if (float20_depth_variant) pipeline_key ^= 0x20E4D24F5A17A11Cull;
      if (!impl_->pipelines.contains(pipeline_key)) {
        auto pipeline = std::make_unique<GraphicsPipeline>();
        std::array<VkFormat, 4> formats{};
        std::array<std::uint8_t, 4> write_masks{};
        std::array<BlendState, 4> blend_states{};
        for (std::size_t i = 0; i < color_count; ++i) {
          if (active_targets[i]) {
            formats[i] = active_targets[i]->format();
            write_masks[i] = active_write_masks[i];
            blend_states[i] = active_blends[i];
          } else {
            formats[i] = VK_FORMAT_UNDEFINED;
          }
        }
        const std::span<const VkFormat> format_span(formats.data(), color_count);
        const std::span<const std::uint8_t> write_mask_span(
            write_masks.data(), color_count);
        const std::span<const BlendState> blend_state_span(
            blend_states.data(), color_count);
        if (!pipeline->initialize(
                impl_->context.device(), impl_->resources.pipeline_layout(),
                *vertex->second, *pixel_shader,
                batch.requires_rectangle_expansion
                    ? impl_->rectangle_list_shader.get() : nullptr,
                format_span, samples,
                batch.topology, state.raster, write_mask_span, blend_state_span,
                active_depth ? active_depth->format() : VK_FORMAT_UNDEFINED,
                active_depth ? &state.depth_target : nullptr)) {
          impl_->error = pipeline->error();
          return;
        }
        impl_->pipelines.emplace(pipeline_key, std::move(pipeline));
      }
      VkDeviceSize index_buffer_offset = 0;
      if (batch.indexed) {
        const auto byte_size = batch.indices.size() * sizeof(std::uint32_t);
        const auto aligned_offset = (impl_->transient_upload_offset + 3u) & ~std::size_t{3u};
        if (byte_size > impl_->transient_upload_mapping.size() -
                            std::min(aligned_offset,
                                     impl_->transient_upload_mapping.size())) {
          impl_->error = "Vulkan transient upload arena exhausted by one submission";
          return;
        }
        std::memcpy(impl_->transient_upload_mapping.data() + aligned_offset,
                    batch.indices.data(), byte_size);
        impl_->transient_upload_offset = aligned_offset + byte_size;
        index_buffer_offset = static_cast<VkDeviceSize>(aligned_offset);
      }
      const auto* pipeline = impl_->pipelines.at(pipeline_key).get();
      const auto descriptor_set = impl_->resources.descriptor_set();
      const auto render_width = color_count
                                    ? std::uint32_t(state.raster.surface_pitch)
                                    : active_depth->width();
      const auto render_height = color_count
                                     ? std::uint32_t(state.raster.scissor_bottom)
                                     : active_depth->height();
      const auto scissor_left = std::max(0, state.raster.scissor_left);
      const auto scissor_top = std::max(0, state.raster.scissor_top);
      const auto scissor_right = std::clamp(
          state.raster.scissor_right, scissor_left,
          static_cast<std::int32_t>(render_width));
      const auto scissor_bottom = std::clamp(
          state.raster.scissor_bottom, scissor_top,
          static_cast<std::int32_t>(render_height));
      if (!impl_->queue.execute([&](VkCommandBuffer command_buffer) {
            for (std::size_t i = 0; i < color_count; ++i)
              if (active_targets[i])
                active_targets[i]->transition_to_color_attachment(command_buffer);
            if (active_depth)
              active_depth->transition_to_depth_attachment(command_buffer);
            std::array<VkRenderingAttachmentInfo, 4> attachments{};
            for (std::size_t i = 0; i < color_count; ++i) {
              attachments[i].sType =
                  VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
              if (active_targets[i]) {
                attachments[i].imageView = active_targets[i]->view();
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
            rendering.renderArea.extent = {render_width, render_height};
            rendering.layerCount = 1;
            rendering.colorAttachmentCount =
                static_cast<std::uint32_t>(color_count);
            rendering.pColorAttachments =
                color_count ? attachments.data() : nullptr;
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
                : VkViewport{0.0f, 0.0f, float(render_width),
                             float(render_height), 0.0f, 1.0f};
            const VkRect2D scissor{{scissor_left, scissor_top},
                                   {static_cast<std::uint32_t>(scissor_right - scissor_left),
                                    static_cast<std::uint32_t>(scissor_bottom - scissor_top)}};
            vkCmdSetViewport(command_buffer, 0, 1, &viewport);
            vkCmdSetScissor(command_buffer, 0, 1, &scissor);
            vkCmdSetBlendConstants(command_buffer, state.blend_constant.data());
            if (active_depth && state.depth_target.stencil_enabled) {
              vkCmdSetStencilCompareMask(
                  command_buffer, VK_STENCIL_FACE_FRONT_BIT,
                  state.depth_target.stencil_read_mask);
              vkCmdSetStencilWriteMask(
                  command_buffer, VK_STENCIL_FACE_FRONT_BIT,
                  state.depth_target.stencil_write_mask);
              vkCmdSetStencilReference(
                  command_buffer, VK_STENCIL_FACE_FRONT_BIT,
                  state.depth_target.stencil_reference);
              const auto back_read = state.depth_target.backface_stencil_enabled
                                         ? state.depth_target.stencil_back_read_mask
                                         : state.depth_target.stencil_read_mask;
              const auto back_write = state.depth_target.backface_stencil_enabled
                                          ? state.depth_target.stencil_back_write_mask
                                          : state.depth_target.stencil_write_mask;
              const auto back_reference = state.depth_target.backface_stencil_enabled
                                              ? state.depth_target.stencil_back_reference
                                              : state.depth_target.stencil_reference;
              vkCmdSetStencilCompareMask(command_buffer,
                                         VK_STENCIL_FACE_BACK_BIT, back_read);
              vkCmdSetStencilWriteMask(command_buffer,
                                       VK_STENCIL_FACE_BACK_BIT, back_write);
              vkCmdSetStencilReference(command_buffer,
                                       VK_STENCIL_FACE_BACK_BIT, back_reference);
            }
            if (batch.indexed) {
              vkCmdBindIndexBuffer(command_buffer, impl_->transient_upload.buffer(),
                                   index_buffer_offset,
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
    impl_->decoded_shaders[load->program.hash()] = load->decoded;
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
