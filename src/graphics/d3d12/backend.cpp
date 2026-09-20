#include "xenon/gpu/d3d12/backend.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <cmath>
#include <deque>
#include <variant>
#include <span>
#include <unordered_set>
#include <unordered_map>

#include "xenon/gpu/d3d12/command_queue.hpp"
#include "xenon/gpu/d3d12/depth_target.hpp"
#include "xenon/gpu/d3d12/guest_memory_mirror.hpp"
#include "xenon/gpu/d3d12/pipeline.hpp"
#include "xenon/gpu/d3d12/presentation.hpp"
#include "xenon/gpu/d3d12/resource_layout.hpp"
#include "xenon/gpu/d3d12/render_target.hpp"
#include "xenon/gpu/d3d12/texture.hpp"
#include "xenon/gpu/resource_ir.hpp"
#include "xenon/gpu/edram_ownership.hpp"
#include "xenon/gpu/texture.hpp"
#include "xenon/gpu/primitive_processor.hpp"
#include "xenon/memory/types.hpp"
#ifdef XENON_HAS_DXC
#include "xenon/gpu/dxc_shader_compiler.hpp"
#include "xenon/gpu/shader_translation.hpp"
#endif

namespace xenon::gpu::d3d12 {

class Backend::Impl {
 public:
  static constexpr std::size_t kTransientUploadBytes = 16u * 1024u * 1024u;
  ~Impl() {
    // Native draws may still be using render targets, textures, descriptor
    // snapshots and upload arenas owned by this Impl. Drain them before member
    // destruction starts.
    (void)queue.wait_idle();
  }
  Context context{};
  CommandQueue queue{};
  PresentationSwapchain presentation{};
  GuestMemoryMirror mirror{};
  ResourceLayout resources{};
  std::array<Buffer, CommandQueue::kFrameCount> transient_uploads{};
  std::array<std::span<std::byte>, CommandQueue::kFrameCount>
      transient_upload_mappings{};
  std::array<std::size_t, CommandQueue::kFrameCount> transient_upload_offsets{};
  ResourceStateTracker resource_state{};
  std::unordered_set<std::uint64_t> pipeline_states{};
  std::unordered_map<std::uint64_t, std::unique_ptr<TextureImage>> textures{};
  std::unordered_map<std::uint64_t, std::unique_ptr<RenderTargetImage>> render_targets{};
  std::unordered_map<std::uint64_t, EdramOwnerId> render_target_owners{};
  std::unordered_map<EdramOwnerId, RenderTargetImage*> owner_render_targets{};
  std::unordered_map<std::uint64_t, std::unique_ptr<RenderTargetImage>> dummy_render_targets{};
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
  GpuPerformanceCounters performance{};
  std::chrono::steady_clock::time_point submission_started{};
  std::deque<std::pair<std::uint64_t, std::shared_ptr<void>>> retired_resources{};

  void collect_retired_resources() {
    const auto completed = queue.completed_value();
    while (!retired_resources.empty() &&
           retired_resources.front().first <= completed) {
      retired_resources.pop_front();
    }
  }

  template <typename T>
  void retire_resource(std::unique_ptr<T> resource) {
    if (!resource) return;
    collect_retired_resources();
    const auto fence_value = queue.retirement_value();
    if (!fence_value || queue.completed_value() >= fence_value) return;
    retired_resources.emplace_back(
        fence_value, std::shared_ptr<void>(std::move(resource)));
  }
#ifdef XENON_HAS_DXC
  std::shared_ptr<DxcShaderCompiler> compiler{std::make_shared<DxcShaderCompiler>()};
  ShaderCache shader_cache{compiler};
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>> shaders{};
  std::unordered_map<std::uint64_t, DecodedShader> decoded_shaders{};
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>>
      float20_depth_shaders{};
  std::unordered_map<std::uint64_t, std::shared_ptr<const CompiledShader>>
      writable_guest_memory_shaders{};
  std::shared_ptr<const CompiledShader> rectangle_list_shader{};
#endif
  std::string error{};
  bool ready{};

  bool flush_color_owner(EdramOwnerId owner) {
    const auto found = owner_render_targets.find(owner);
    if (!edram || found == owner_render_targets.end() || !found->second) {
      error = "D3D12 EDRAM alias transfer has no native color owner";
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
                    ? "D3D12 EDRAM ownership sample readback failed"
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
      error = "D3D12 EDRAM alias transfer has no native depth owner";
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
                    ? "D3D12 EDRAM depth ownership sample readback failed"
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
    error = "D3D12 EDRAM alias transfer has no native owner";
    return false;
  }

  bool make_edram_canonical() {
    if (!edram) {
      error = "D3D12 canonical EDRAM checkpoint has no bound EDRAM";
      return false;
    }

    // Snapshot owner IDs before flushing because each flush releases all tiles
    // held by that owner. Owner IDs are unique across color and depth images.
    std::vector<EdramOwnerId> owners;
    owners.reserve(owner_render_targets.size() + owner_depth_targets.size());
    for (const auto& [owner, image] : owner_render_targets) {
      if (image && edram_ownership.owned_tile_count(owner))
        owners.push_back(owner);
    }
    for (const auto& [owner, image] : owner_depth_targets) {
      if (image && edram_ownership.owned_tile_count(owner))
        owners.push_back(owner);
    }
    for (const auto owner : owners) {
      if (edram_ownership.owned_tile_count(owner) && !flush_owner(owner))
        return false;
    }
    return true;
  }

  bool acquire_color_ownership(std::uint64_t key,
                               RenderTargetImage& requested,
                               const EdramSurfaceLayout& ownership_surface) {
    const auto owner_it = render_target_owners.find(key);
    if (!edram || owner_it == render_target_owners.end()) {
      error = "D3D12 EDRAM color surface has no ownership identity";
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
      error = "D3D12 EDRAM ownership transfer did not reach canonical storage";
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
                    ? "D3D12 canonical EDRAM sample upload failed"
                    : requested.error();
        return false;
      }
    }
    if (!edram_ownership.commit(plan)) {
      error = "D3D12 EDRAM ownership plan became stale";
      return false;
    }
    return true;
  }

  bool acquire_depth_ownership(std::uint64_t key,
                               DepthTargetImage& requested,
                               const EdramSurfaceLayout& ownership_surface) {
    const auto owner_it = depth_target_owners.find(key);
    if (!edram || owner_it == depth_target_owners.end()) {
      error = "D3D12 EDRAM depth surface has no ownership identity";
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
      error = "D3D12 depth EDRAM ownership transfer did not reach canonical storage";
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
                    ? "D3D12 canonical EDRAM depth sample upload failed"
                    : requested.error();
        return false;
      }
    }
    if (!edram_ownership.commit(plan)) {
      error = "D3D12 depth EDRAM ownership plan became stale";
      return false;
    }
    return true;
  }

  bool make_region_canonical(const EdramSurfaceLayout& surface,
                             EdramSurfaceRegion region) {
    if (!edram || !surface.valid()) {
      error = "D3D12 Xenos resolve clear has an invalid EDRAM surface";
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
      error = "D3D12 Xenos post-resolve depth clear has an invalid surface extent";
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
      error = "D3D12 regional depth resolve clear failed";
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
  if (!impl_->queue.initialize(impl_->context.device())) {
    impl_->error = impl_->queue.error();
    return false;
  }
  if (!impl_->resources.initialize(impl_->context.device())) {
    impl_->error = impl_->resources.error();
    return false;
  }
  for (std::uint32_t i = 0; i < CommandQueue::kFrameCount; ++i) {
    if (!impl_->transient_uploads[i].initialize(
            impl_->context.device(), Impl::kTransientUploadBytes,
            D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ) ||
        !impl_->transient_uploads[i].map(impl_->transient_upload_mappings[i])) {
      impl_->error = impl_->transient_uploads[i].error();
      return false;
    }
  }
  impl_->ready = true;
  return true;
}

bool Backend::configure_presentation(void* native_window,
                                     const PresentationConfig& config) {
  if (!impl_->ready) return false;
  if (!impl_->presentation.initialize(impl_->context, impl_->queue,
                                      native_window, config)) {
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
    impl_->dummy_render_targets.clear();
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
    if (!impl_->mirror.initialize(impl_->context.device(), impl_->queue,
                                  memory)) {
      impl_->error = impl_->mirror.error();
      impl_->ready = false;
      return;
    }
    impl_->memory = &memory;
    if (!impl_->resources.bind_guest_memory(impl_->mirror.resource(),
                                            memory::kPhysicalMemorySize)) {
      impl_->error = impl_->resources.error();
      impl_->ready = false;
      return;
    }
  }
}

void Backend::consume(const ir::Command& command) {
  impl_->collect_retired_resources();
  ++impl_->command_count;
  ++impl_->performance.commands;
  if (const auto* write = std::get_if<ir::RegisterWrite>(&command)) {
    impl_->resource_state.apply(*write);
  }
  if (const auto* draw = std::get_if<ir::DrawPacket>(&command)) {
    ++impl_->draw_count;
    ++impl_->performance.draws;
    const auto state = impl_->resource_state.snapshot();
    if (state.edram_mode == EdramMode::Copy) {
      if (!impl_->memory) {
        impl_->error = "D3D12 Xenos resolve has no bound guest memory";
        return;
      }
      const auto& resolve_vertices = state.vertex_buffers[0][0];
      if (!resolve_vertices || !resolve_vertices->valid ||
          resolve_vertices->size_dwords != 6u) {
        impl_->error = "D3D12 Xenos resolve has no valid rectangle vertices";
        return;
      }
      // Resolve rectangles are read by the CPU-side common planner from the
      // conventional six-dword vertex stream. If that stream was produced by
      // an earlier memexport, download only those bytes before decoding it.
      if (!impl_->mirror.make_cpu_visible(
              resolve_vertices->physical_address, 6u * sizeof(std::uint32_t),
              memory::GpuRangeUsage::CommandData)) {
        impl_->error = impl_->mirror.error();
        return;
      }
      std::array<std::byte, 6u * sizeof(std::uint32_t)>
          resolve_vertex_snapshot{};
      const auto resolve_vertex_base =
          state.vertex_buffers[0][0]->physical_address;
      if (!impl_->memory->copy_physical_range(resolve_vertex_base,
                                               resolve_vertex_snapshot)) {
        impl_->error = "D3D12 resolve vertex snapshot is outside physical memory";
        return;
      }
      const auto resolve_plan =
          plan_resolve(state, resolve_vertex_snapshot, resolve_vertex_base);
      if (!resolve_plan.valid) {
        impl_->error = "D3D12 " + resolve_plan.error;
        return;
      }
      const auto& rectangle = resolve_plan.rectangle;
      if (rectangle.empty()) return;
      if (state.copy.command != CopyCommand::Raw &&
          state.copy.command != CopyCommand::Convert) {
        impl_->error = "D3D12 Xenos resolve command is unsupported";
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
              "D3D12 Xenos depth resolve source EDRAM surface is unavailable";
          return;
        }
        if (!impl_->acquire_depth_ownership(source_key, *source,
                                             source->surface())) return;
        if (resolve_plan.selected_sample_count != 1) {
          impl_->error = "D3D12 Xenos depth resolve selected multiple samples";
          return;
        }
        std::uint32_t guest_sample{};
        while (guest_sample < 4 &&
               !(resolve_plan.guest_sample_mask & (1u << guest_sample))) {
          ++guest_sample;
        }
        if (guest_sample >= 4) {
          impl_->error = "D3D12 Xenos depth resolve selected no sample";
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
        const auto written = write_depth_resolve(
            resolve_plan.copy, format, rectangle, readback, row_pitch,
            *impl_->memory);
        if (!written.valid) {
          impl_->error = written.error;
          return;
        }
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
        impl_->error = "D3D12 Xenos raw resolve source and destination formats differ";
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
        impl_->error = "D3D12 Xenos resolve source EDRAM surface is unavailable";
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
        std::array<std::uint32_t, 4> selected_samples{};
        std::uint32_t selected_count{};
        for (std::uint32_t guest_sample = 0; guest_sample < 4; ++guest_sample) {
          if (resolve_plan.guest_sample_mask & (1u << guest_sample)) {
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
        } else if (selected_count == 2 || selected_count == 4) {
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
            impl_->error = "D3D12 Xenos selected-sample resolve averaging failed";
            return;
          }
          if (selected_count == 4) {
            std::vector<std::byte> third;
            std::vector<std::byte> fourth;
            std::vector<std::byte> second_average;
            std::vector<std::byte> all_average;
            std::uint32_t third_pitch{};
            std::uint32_t fourth_pitch{};
            std::uint32_t second_average_pitch{};
            std::uint32_t all_average_pitch{};
            if (!source->readback_sample(
                    impl_->queue, selected_samples[2], rectangle.left,
                    rectangle.top, rectangle.right, rectangle.bottom, third,
                    third_pitch) ||
                !source->readback_sample(
                    impl_->queue, selected_samples[3], rectangle.left,
                    rectangle.top, rectangle.right, rectangle.bottom, fourth,
                    fourth_pitch) ||
                !average_host_color_samples(
                    format, width, height, third, third_pitch, fourth,
                    fourth_pitch, second_average, second_average_pitch) ||
                !average_host_color_samples(
                    format, width, height, readback, row_pitch,
                    second_average, second_average_pitch, all_average,
                    all_average_pitch)) {
              impl_->error =
                  "D3D12 Xenos four-sample resolve averaging failed";
              return;
            }
            readback = std::move(all_average);
            row_pitch = all_average_pitch;
          }
        } else {
          impl_->error = "D3D12 Xenos resolve selected an unsupported sample set";
          return;
        }
      }
      const auto written = state.copy.command == CopyCommand::Convert ||
                                   color_host_requires_conversion(format)
          ? write_converted_resolve(state.copy, format, rectangle, readback,
                                    row_pitch, *impl_->memory)
          : write_raw_resolve(state.copy, rectangle, readback, row_pitch,
                              *impl_->memory);
      if (!written.valid) {
        impl_->error = written.error;
        return;
      }
      if (state.copy.color_clear_enabled) {
        const auto owner = impl_->render_target_owners.find(source_key);
        if (owner == impl_->render_target_owners.end() ||
            !impl_->flush_color_owner(owner->second) ||
            !clear_edram_surface_region(
                *impl_->edram, source->surface(), rectangle.left,
                rectangle.top, rectangle.right, rectangle.bottom,
                state.copy.color_clear)) {
          if (impl_->error.empty())
            impl_->error = "D3D12 regional color resolve clear failed";
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
      impl_->error = "D3D12 Xenos MRT draw has an invalid EDRAM surface extent";
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
          auto retired = std::move(existing->second);
          impl_->owner_render_targets.erase(owner);
          impl_->render_target_owners.erase(key);
          impl_->render_targets.erase(existing);
          impl_->retire_resource(std::move(retired));
        }
        if (!impl_->render_targets.contains(key)) {
          auto image = std::make_unique<RenderTargetImage>();
          if (!image->initialize(impl_->context.device(), surface, format)) {
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
      // D3D12 does not expose Vulkan-style undefined dynamic-rendering slots.
      // Preserve sparse Xenos export locations by binding per-slot throwaway
      // RTVs with a zero write mask. This keeps SV_TargetN attached to native
      // RT slot N without compacting or aliasing a real EDRAM surface.
      for (std::size_t slot = 0; slot < color_count; ++slot) {
        if (active_targets[slot]) continue;
        EdramSurfaceLayout dummy_surface{
            0, state.raster.surface_pitch, height,
            static_cast<MsaaSamples>(state.raster.msaa_samples_log2), false,
            false};
        const auto key = dummy_surface.hash() ^ 0xD00D000000000000ull ^
                         (std::uint64_t(slot) << 56u);
        if (!impl_->dummy_render_targets.contains(key)) {
          auto image = std::make_unique<RenderTargetImage>();
          if (!image->initialize(impl_->context.device(), dummy_surface,
                                 ColorRenderTargetFormat::R8G8B8A8)) {
            impl_->error = image->error();
            return;
          }
          impl_->dummy_render_targets.emplace(key, std::move(image));
        }
        active_targets[slot] = impl_->dummy_render_targets.at(key).get();
        active_write_masks[slot] = 0;
        active_blends[slot] = {};
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
        auto retired = std::move(existing->second);
        impl_->owner_depth_targets.erase(owner);
        impl_->depth_target_owners.erase(key);
        impl_->depth_targets.erase(existing);
        impl_->retire_resource(std::move(retired));
      }
      if (!impl_->depth_targets.contains(key)) {
        auto image = std::make_unique<DepthTargetImage>();
        if (!image->initialize(impl_->context.device(), surface,
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
    // Phase 15: the guest-memory mirror is populated lazily. Vertex fetches
    // request only the physical ranges described by the active Xenos fetch
    // constants instead of synchronizing the entire 512 MiB aperture.
    if (impl_->memory) {
      for (const auto& fetch_group : state.vertex_buffers) {
        for (const auto& fetch : fetch_group) {
          if (!fetch || !fetch->valid || !fetch->size_dwords) continue;
          const auto bytes64 = std::uint64_t{fetch->size_dwords} * 4u;
          if (bytes64 > UINT32_MAX ||
              !impl_->mirror.synchronize_range(
                  fetch->physical_address, static_cast<std::uint32_t>(bytes64),
                  memory::GpuRangeUsage::VertexBuffer)) {
            impl_->error = impl_->mirror.error().empty()
                               ? "D3D12 vertex fetch range is invalid"
                               : impl_->mirror.error();
            return;
          }
        }
      }
    }
    auto constants = impl_->resources.constants();
    if (!impl_->resource_state.write_constant_buffer(constants)) {
      impl_->error = "D3D12 shader constant upload failed";
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
      const auto dirty_epoch = impl_->memory->coherency().current_epoch();
      for (std::uint32_t slot = 0; slot < used.size(); ++slot) {
        if (!used[slot] || !state.textures[slot]) continue;
        const auto& descriptor = *state.textures[slot];
        const auto key = descriptor.hash();
        auto existing = impl_->textures.find(key);
        const bool refresh = existing == impl_->textures.end() ||
                             impl_->texture_dirty.consume_dirty(
                                 key, impl_->memory->coherency(), dirty_epoch);
        if (refresh) {
          // Texture decoding is still CPU-side. Preserve GPU-resident
          // memexport normally, but if a later draw actually samples a
          // GPU-authored texture, download only that texture's subresources.
          const auto source_layout = build_texture_layout(descriptor);
          if (!source_layout.valid) {
            impl_->error = source_layout.error;
            return;
          }
          for (const auto& subresource : source_layout.subresources) {
            if (subresource.guest_size_bytes > UINT32_MAX ||
                !impl_->mirror.make_cpu_visible(
                    subresource.guest_address,
                    static_cast<std::uint32_t>(subresource.guest_size_bytes),
                    memory::GpuRangeUsage::Texture)) {
              impl_->error = impl_->mirror.error().empty()
                                 ? "D3D12 GPU-authored texture range is invalid"
                                 : impl_->mirror.error();
              return;
            }
          }
          std::uint32_t texture_snapshot_base = memory::kPhysicalMemorySize;
          std::uint64_t texture_snapshot_end = 0u;
          for (const auto& subresource : source_layout.subresources) {
            texture_snapshot_base =
                (std::min)(texture_snapshot_base, subresource.guest_address);
            texture_snapshot_end = (std::max)(
                texture_snapshot_end,
                std::uint64_t{subresource.guest_address} +
                    subresource.guest_size_bytes);
          }
          if (texture_snapshot_base >= memory::kPhysicalMemorySize ||
              texture_snapshot_end > memory::kPhysicalMemorySize ||
              texture_snapshot_end <= texture_snapshot_base) {
            impl_->error = "D3D12 texture snapshot range is invalid";
            return;
          }
          std::vector<std::byte> texture_snapshot(
              static_cast<std::size_t>(texture_snapshot_end -
                                       texture_snapshot_base));
          if (!impl_->memory->copy_physical_range(texture_snapshot_base,
                                                   texture_snapshot)) {
            impl_->error = "D3D12 texture snapshot failed";
            return;
          }
          const auto decoded =
              decode_texture(descriptor, texture_snapshot, texture_snapshot_base);
          if (!decoded.valid) {
            impl_->error = decoded.error;
            return;
          }
          auto image = std::make_unique<TextureImage>();
          if (!image->initialize(impl_->context.device(), impl_->queue, decoded) ||
              !impl_->resources.bind_texture(slot, descriptor.dimension,
                                             image->resource(), image->format(),
                                             descriptor.mip_max_level + 1u, descriptor)) {
            impl_->error = image->error().empty() ? impl_->resources.error() : image->error();
            return;
          }
          impl_->texture_dirty.track_clean(key, decoded.layout, dirty_epoch);
          if (existing != impl_->textures.end()) {
            auto retired = std::move(existing->second);
            existing->second = std::move(image);
            impl_->retire_resource(std::move(retired));
          } else {
            impl_->textures.emplace(key, std::move(image));
          }
        } else if (!impl_->resources.bind_texture(
                       slot, descriptor.dimension, existing->second->resource(),
                       existing->second->format(), descriptor.mip_max_level + 1u,
                       descriptor)) {
          impl_->error = impl_->resources.error();
          return;
        }
      }
    }
#ifdef XENON_HAS_DXC
    if (impl_->memory && draw->vertex_shader.valid &&
        draw->pixel_shader.valid) {
      const auto vertex = impl_->shaders.find(draw->vertex_shader.hash);
      const auto pixel = impl_->shaders.find(draw->pixel_shader.hash);
      if (vertex == impl_->shaders.end() || pixel == impl_->shaders.end()) {
        impl_->error = "D3D12 draw references a shader that has not been compiled";
        return;
      }
      const auto decoded_vertex =
          impl_->decoded_shaders.find(draw->vertex_shader.hash);
      const auto decoded_pixel =
          impl_->decoded_shaders.find(draw->pixel_shader.hash);
      if (decoded_vertex == impl_->decoded_shaders.end() ||
          decoded_pixel == impl_->decoded_shaders.end()) {
        impl_->error = "D3D12 draw lacks decoded shader reflection";
        return;
      }
      const bool vertex_memexport =
          decoded_vertex->second.reflection.memory_exports != 0;
      const bool pixel_memexport =
          decoded_pixel->second.reflection.memory_exports != 0;
      const bool memexport_writable = vertex_memexport || pixel_memexport;
      std::vector<MemExportRange> memexport_ranges;
      bool dynamic_memexport_range = false;
      auto gather_memexport_ranges = [&](const DecodedShader& decoded) {
        if (!decoded.reflection.memory_exports) return;
        const auto plan = impl_->resource_state.plan_memexport(decoded);
        dynamic_memexport_range |= plan.requires_dynamic_address_analysis;
        memexport_ranges.insert(memexport_ranges.end(), plan.ranges.begin(),
                                plan.ranges.end());
      };
      gather_memexport_ranges(decoded_vertex->second);
      gather_memexport_ranges(decoded_pixel->second);
      if (!color_count && !active_depth && !memexport_writable) return;

      // Known memexport targets stay range-driven. Only shaders whose export
      // address cannot be resolved statically request the deliberate full-range
      // fallback required for genuinely unrestricted guest-memory access.
      if (memexport_writable) {
        if (dynamic_memexport_range) {
          if (!impl_->mirror.synchronize()) {
            impl_->error = impl_->mirror.error();
            return;
          }
        } else {
          for (const auto& range : memexport_ranges) {
            const auto address = range.base_address_dwords << 2u;
            if (range.size_bytes &&
                !impl_->mirror.synchronize_range(
                    address, range.size_bytes,
                    memory::GpuRangeUsage::MemoryExport)) {
              impl_->error = impl_->mirror.error().empty()
                                 ? "D3D12 memexport range is invalid"
                                 : impl_->mirror.error();
              return;
            }
          }
        }
      }

      auto vertex_shader = vertex->second;
      auto pixel_shader = pixel->second;
      auto writable_variant = [&](std::uint64_t shader_hash,
                                  const DecodedShader& decoded,
                                  std::shared_ptr<const CompiledShader> base) {
        if (!memexport_writable || decoded.reflection.memory_exports != 0)
          return base;
        const std::uint64_t variant_key =
            shader_hash ^ 0x4D454D4558505257ull;
        auto found = impl_->writable_guest_memory_shaders.find(variant_key);
        if (found != impl_->writable_guest_memory_shaders.end())
          return found->second;
        ShaderLoweringOptions lowering_options{};
        lowering_options.force_guest_memory_rw = true;
        const auto lowered = HlslShaderLowerer::lower(decoded, lowering_options);
        if (!lowered.complete) {
          impl_->error = lowered.diagnostics.empty()
                             ? "D3D12 writable guest-memory shader lowering failed"
                             : lowered.diagnostics.front();
          return std::shared_ptr<const CompiledShader>{};
        }
        ShaderCompileOptions compile_options{};
        compile_options.format = ShaderBinaryFormat::Dxil;
        const auto compiled =
            impl_->shader_cache.get_or_compile(lowered, compile_options);
        if (!compiled->succeeded) {
          impl_->error = compiled->diagnostics.empty()
                             ? "D3D12 writable guest-memory shader compilation failed"
                             : compiled->diagnostics.front();
          return std::shared_ptr<const CompiledShader>{};
        }
        ++impl_->compiled_shader_count;
        impl_->writable_guest_memory_shaders.emplace(variant_key, compiled);
        return compiled;
      };
      vertex_shader = writable_variant(draw->vertex_shader.hash,
                                       decoded_vertex->second, vertex_shader);
      pixel_shader = writable_variant(draw->pixel_shader.hash,
                                      decoded_pixel->second, pixel_shader);
      if (!vertex_shader || !pixel_shader) return;
      bool float20_depth_variant = false;
      const auto samples = static_cast<MsaaSamples>(state.raster.msaa_samples_log2);
      if (active_depth && active_depth->requires_float24_conversion() &&
          (state.depth_target.test_enabled || state.depth_target.write_enabled)) {
        const bool per_sample = samples != MsaaSamples::X1;
        const std::uint64_t variant_key =
            draw->pixel_shader.hash ^ 0xD24F20E4D24F20E4ull ^
            (per_sample ? 0x8000000000000000ull : 0ull) ^
            (memexport_writable ? 0x0400000000000000ull : 0ull);
        auto variant = impl_->float20_depth_shaders.find(variant_key);
        if (variant == impl_->float20_depth_shaders.end()) {
          ShaderLoweringOptions lowering_options{};
          lowering_options.pixel_depth_output =
              PixelDepthOutputMode::Float20e4NearestEven;
          lowering_options.force_sample_frequency = per_sample;
          lowering_options.force_guest_memory_rw = memexport_writable;
          const auto lowered =
              HlslShaderLowerer::lower(decoded_pixel->second, lowering_options);
          if (!lowered.complete) {
            impl_->error = lowered.diagnostics.empty()
                               ? "D24FS8 pixel shader lowering failed"
                               : lowered.diagnostics.front();
            return;
          }
          ShaderCompileOptions compile_options{};
          compile_options.format = ShaderBinaryFormat::Dxil;
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

      if (draw->index_buffer.valid && draw->index_buffer.length_bytes &&
          !impl_->mirror.make_cpu_visible(
              draw->index_buffer.physical_address,
              draw->index_buffer.length_bytes,
              memory::GpuRangeUsage::IndexBuffer)) {
        impl_->error = impl_->mirror.error();
        return;
      }
      const PrimitiveProcessingOptions primitive_options{
          state.primitive_assembly.reset_enabled,
          state.primitive_assembly.reset_index};
      std::vector<std::byte> index_snapshot;
      std::uint32_t index_snapshot_base = 0u;
      if (draw->source == DrawSource::Dma) {
        const auto bytes_per_index =
            draw->index_buffer.format == IndexFormat::UInt32 ? 4u : 2u;
        const auto index_bytes = std::uint64_t{(std::min)(
            draw->index_count,
            draw->index_buffer.length_bytes / bytes_per_index)} *
                                 bytes_per_index;
        if (index_bytes > UINT32_MAX ||
            std::uint64_t{draw->index_buffer.physical_address} + index_bytes >
                memory::kPhysicalMemorySize) {
          impl_->error = "D3D12 index snapshot range is invalid";
          return;
        }
        index_snapshot.resize(static_cast<std::size_t>(index_bytes));
        index_snapshot_base = draw->index_buffer.physical_address;
        if (!impl_->memory->copy_physical_range(index_snapshot_base,
                                                 index_snapshot)) {
          impl_->error = "D3D12 index snapshot failed";
          return;
        }
      }
      const auto batch = process_primitives(
          *draw, index_snapshot, primitive_options, index_snapshot_base);
      if (!batch.valid) {
        impl_->error = batch.error;
        return;
      }
      if (batch.requires_rectangle_expansion &&
          !impl_->rectangle_list_shader) {
        ShaderCompileOptions options{};
        options.format = ShaderBinaryFormat::Dxil;
        impl_->rectangle_list_shader = impl_->shader_cache.get_or_compile(
            make_rectangle_list_geometry_shader(), options);
        if (!impl_->rectangle_list_shader->succeeded) {
          impl_->error = impl_->rectangle_list_shader->diagnostics.empty()
                             ? "D3D12 RectangleList shader compilation failed"
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
          state.raster.cull_front && state.raster.cull_back &&
          !vertex_memexport) {
        return;
      }
      auto pipeline_key = state.pipeline_hash(*draw);
      pipeline_key ^= std::uint64_t(batch.topology) << 61u;
      if (batch.requires_rectangle_expansion)
        pipeline_key ^= 0x52454354414E474Cull;
      if (color_count) {
        for (std::size_t i = 0; i < color_count; ++i) {
          pipeline_key ^=
              std::uint64_t(active_targets[i]->format()) << (16u + i * 8u);
        }
      } else {
        pipeline_key ^= 0xD3F7000000000000ull;
      }
      if (float20_depth_variant) pipeline_key ^= 0x20E4D24F5A17A11Cull;
      if (memexport_writable) pipeline_key ^= 0x4D454D4558504F52ull;
      if (!impl_->pipelines.contains(pipeline_key)) {
        auto pipeline = std::make_unique<GraphicsPipeline>();
        std::array<DXGI_FORMAT, 4> formats{};
        std::array<std::uint8_t, 4> write_masks{};
        std::array<BlendState, 4> blend_states{};
        for (std::size_t i = 0; i < color_count; ++i) {
          formats[i] = active_targets[i]->format();
          write_masks[i] = active_write_masks[i];
          blend_states[i] = active_blends[i];
        }
        const std::span<const DXGI_FORMAT> format_span(formats.data(), color_count);
        const std::span<const std::uint8_t> write_mask_span(
            write_masks.data(), color_count);
        const std::span<const BlendState> blend_state_span(
            blend_states.data(), color_count);
        if (!pipeline->initialize(
                impl_->context.device(), impl_->resources.root_signature(),
                *vertex_shader, *pixel_shader,
                batch.requires_rectangle_expansion
                    ? impl_->rectangle_list_shader.get() : nullptr,
                format_span, samples,
                batch.topology, state.raster, write_mask_span, blend_state_span,
                active_depth ? active_depth->format() : DXGI_FORMAT_UNKNOWN,
                active_depth ? &state.depth_target : nullptr)) {
          impl_->error = pipeline->error();
          return;
        }
        impl_->pipelines.emplace(pipeline_key, std::move(pipeline));
      }
      const auto index_byte_size =
          batch.indexed ? batch.indices.size() * sizeof(std::uint32_t) : 0u;
      if (index_byte_size > Impl::kTransientUploadBytes) {
        impl_->error = "D3D12 transient upload arena exhausted by one submission";
        return;
      }
      const auto* pipeline = impl_->pipelines.at(pipeline_key).get();
      const auto render_width = color_count
                                    ? active_targets[0]->width()
                                    : active_depth
                                          ? active_depth->width()
                                          : (std::max)(1u, std::uint32_t(
                                                                state.raster.surface_pitch));
      const auto render_height = color_count
                                     ? active_targets[0]->height()
                                     : active_depth
                                           ? active_depth->height()
                                           : (std::max)(1u, std::uint32_t(
                                                                 (std::max)(1, state.raster.scissor_bottom)));
      const auto scissor_left = (std::max)(0, state.raster.scissor_left);
      const auto scissor_top = (std::max)(0, state.raster.scissor_top);
      const bool suppress_rasterization =
          batch.topology == HostPrimitiveTopology::TriangleList &&
          state.raster.cull_front && state.raster.cull_back &&
          vertex_memexport;
      const auto scissor_right = suppress_rasterization
                                     ? scissor_left
                                     : (std::clamp)(
                                           state.raster.scissor_right, scissor_left,
                                           static_cast<std::int32_t>(render_width));
      const auto scissor_bottom = suppress_rasterization
                                      ? scissor_top
                                      : (std::clamp)(
                                            state.raster.scissor_bottom, scissor_top,
                                            static_cast<std::int32_t>(render_height));
      bool draw_record_failed = false;
      if (!impl_->queue.execute_async(
              [&](ID3D12GraphicsCommandList* list, std::uint32_t frame_index,
                  std::uint32_t draw_slot) {
            impl_->resources.prepare_draw(frame_index, draw_slot);
            D3D12_INDEX_BUFFER_VIEW index_view{};
            if (draw_slot == 0) impl_->transient_upload_offsets[frame_index] = 0;
            if (batch.indexed) {
              const auto aligned_offset =
                  (impl_->transient_upload_offsets[frame_index] + 3u) &
                  ~std::size_t{3u};
              if (index_byte_size >
                  impl_->transient_upload_mappings[frame_index].size() -
                      (std::min)(aligned_offset,
                                 impl_->transient_upload_mappings[frame_index].size())) {
                impl_->error =
                    "D3D12 transient upload arena exhausted by one draw batch";
                draw_record_failed = true;
                return;
              }
              std::memcpy(
                  impl_->transient_upload_mappings[frame_index].data() + aligned_offset,
                  batch.indices.data(), index_byte_size);
              impl_->transient_upload_offsets[frame_index] =
                  aligned_offset + index_byte_size;
              index_view.BufferLocation =
                  impl_->transient_uploads[frame_index].resource()->GetGPUVirtualAddress() +
                  aligned_offset;
              index_view.SizeInBytes = static_cast<UINT>(index_byte_size);
              index_view.Format = DXGI_FORMAT_R32_UINT;
            }
            impl_->mirror.prepare_shader_access(list, memexport_writable);
            list->SetPipelineState(pipeline->pipeline());
            list->SetGraphicsRootSignature(impl_->resources.root_signature());
            ID3D12DescriptorHeap* heaps[]{
                impl_->resources.resource_heap(frame_index, draw_slot),
                impl_->resources.sampler_heap(frame_index, draw_slot)};
            list->SetDescriptorHeaps(2, heaps);
            list->SetGraphicsRootConstantBufferView(
                0, impl_->resources.constants_resource(frame_index, draw_slot)->GetGPUVirtualAddress());
            list->SetGraphicsRootDescriptorTable(
                1, impl_->resources.resource_heap(frame_index, draw_slot)
                       ->GetGPUDescriptorHandleForHeapStart());
            list->SetGraphicsRootDescriptorTable(
                2, impl_->resources.sampler_heap(frame_index, draw_slot)
                       ->GetGPUDescriptorHandleForHeapStart());
            list->SetGraphicsRootDescriptorTable(
                3, impl_->resources.memory_export_handle(frame_index, draw_slot));
            list->OMSetBlendFactor(state.blend_constant.data());
            const auto& guest_viewport = state.raster.viewport;
            const auto x_scale = std::abs(guest_viewport.x_scale);
            const auto y_scale = std::abs(guest_viewport.y_scale);
            const bool has_viewport = x_scale > 0.0001f && y_scale > 0.0001f;
            const auto depth_a = (std::clamp)(guest_viewport.z_offset, 0.0f, 1.0f);
            const auto depth_b = (std::clamp)(
                guest_viewport.z_offset + guest_viewport.z_scale, 0.0f, 1.0f);
            const D3D12_VIEWPORT viewport = has_viewport
                ? D3D12_VIEWPORT{guest_viewport.x_offset - x_scale,
                                 guest_viewport.y_offset - y_scale,
                                 x_scale * 2.0f, y_scale * 2.0f,
                                 (std::min)(depth_a, depth_b),
                                 (std::max)(depth_a, depth_b)}
                : D3D12_VIEWPORT{0.0f, 0.0f, float(render_width),
                                 float(render_height), 0.0f, 1.0f};
            const D3D12_RECT scissor{scissor_left, scissor_top,
                                     scissor_right, scissor_bottom};
            list->RSSetViewports(1, &viewport);
            list->RSSetScissorRects(1, &scissor);
            std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> rtvs{};
            for (std::size_t i = 0; i < color_count; ++i)
              rtvs[i] = active_targets[i]->rtv();
            const auto dsv = active_depth ? active_depth->dsv()
                                          : D3D12_CPU_DESCRIPTOR_HANDLE{};
            list->OMSetRenderTargets(static_cast<UINT>(color_count),
                                     color_count ? rtvs.data() : nullptr, FALSE,
                                     active_depth ? &dsv : nullptr);
            if (active_depth && state.depth_target.stencil_enabled) {
              Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList8> list8;
              if (state.depth_target.backface_stencil_enabled &&
                  SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&list8)))) {
                list8->OMSetFrontAndBackStencilRef(
                    state.depth_target.stencil_reference,
                    state.depth_target.stencil_back_reference);
              } else {
                list->OMSetStencilRef(state.depth_target.stencil_reference);
              }
            }
            list->IASetPrimitiveTopology(pipeline->native_topology());
            if (batch.indexed) {
              list->IASetIndexBuffer(&index_view);
              list->DrawIndexedInstanced(
                  static_cast<UINT>(batch.indices.size()), 1, 0, 0, 0);
            } else {
              list->DrawInstanced(batch.vertex_count, 1, 0, 0);
            }
          })) {
        impl_->error = impl_->queue.error();
        return;
      } else if (draw_record_failed) {
        return;
      } else if (memexport_writable) {
        if (dynamic_memexport_range) {
          // Unusual shaders may synthesize eA dynamically. Until runtime
          // address analysis is available, preserve correctness by treating
          // the complete physical aperture as GPU-owned rather than risking a
          // stale CPU upload over an unknown export.
          impl_->mirror.mark_gpu_write(0, memory::kPhysicalMemorySize);
          impl_->texture_dirty.mark_dirty(0, memory::kPhysicalMemorySize);
        } else {
          for (const auto& range : memexport_ranges) {
            const auto address = range.base_address_dwords << 2u;
            impl_->mirror.mark_gpu_write(address, range.size_bytes);
            impl_->texture_dirty.mark_dirty(address, range.size_bytes);
          }
        }
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
    options.format = ShaderBinaryFormat::Dxil;
    const auto compiled = impl_->shader_cache.get_or_compile(lowered, options);
    if (compiled->succeeded) {
      ++impl_->compiled_shader_count;
      impl_->shaders[load->program.hash()] = compiled;
    } else {
      impl_->error = compiled->diagnostics.empty() ? "DXIL shader compilation failed"
                                                   : compiled->diagnostics.front();
    }
#endif
  }
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
                         ? "D3D12 scanout source range is invalid"
                         : impl_->mirror.error();
      return PresentStatus::Error;
    }
  }
  std::uint32_t presentation_snapshot_base = memory::kPhysicalMemorySize;
  std::uint64_t presentation_snapshot_end = 0u;
  for (const auto& subresource : layout.subresources) {
    presentation_snapshot_base =
        (std::min)(presentation_snapshot_base, subresource.guest_address);
    presentation_snapshot_end = (std::max)(
        presentation_snapshot_end,
        std::uint64_t{subresource.guest_address} +
            subresource.guest_size_bytes);
  }
  if (presentation_snapshot_base >= memory::kPhysicalMemorySize ||
      presentation_snapshot_end > memory::kPhysicalMemorySize ||
      presentation_snapshot_end <= presentation_snapshot_base) {
    impl_->error = "D3D12 presentation snapshot range is invalid";
    return PresentStatus::Error;
  }
  std::vector<std::byte> presentation_snapshot(
      static_cast<std::size_t>(presentation_snapshot_end -
                               presentation_snapshot_base));
  if (!impl_->memory->copy_physical_range(presentation_snapshot_base,
                                           presentation_snapshot)) {
    impl_->error = "D3D12 presentation snapshot failed";
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
  result.pipeline_cache_misses = impl_->pipeline_states.size();
  return result;
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

}  // namespace xenon::gpu::d3d12
