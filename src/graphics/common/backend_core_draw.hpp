#pragma once

// Draw consumption for BackendCore: EDRAM render targets, textures, shader
// variants and pipelines, then the backend's record_draw() hook. Included by
// backend_core.hpp.

#include "graphics/common/backend_core.hpp"

namespace xenon::gpu::detail {

template <typename Derived, typename Api>
void BackendCore<Derived, Api>::consume_draw(const ir::DrawPacket* draw,
                                             const DrawResourceState& state) {
  pipeline_states.insert(state.pipeline_hash(*draw));
  std::array<RenderTargetImage*, 4> active_targets{};
  std::array<std::uint8_t, 4> active_write_masks{};
  std::array<BlendState, 4> active_blends{};
  const auto color_plan = plan_color_targets(state);
  const auto color_count =
      static_cast<std::size_t>(color_plan.attachment_count);
  DepthTargetImage* active_depth = nullptr;
  if (color_count && (!state.raster.surface_pitch ||
                      state.raster.scissor_bottom <= 0)) {
    error = named("Xenos MRT draw has an invalid EDRAM surface extent");
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
      auto existing = render_targets.find(key);
      if (existing != render_targets.end() &&
          existing->second->height() < height) {
        const auto owner = render_target_owners.at(key);
        if (edram_ownership.owned_tile_count(owner) &&
            !flush_color_owner(owner)) return;
        auto retired = std::move(existing->second);
        owner_render_targets.erase(owner);
        render_target_owners.erase(key);
        render_targets.erase(existing);
        retire_resource(std::move(retired));
      }
      if (!render_targets.contains(key)) {
        auto image = std::make_unique<RenderTargetImage>();
        if (!self().create_render_target(*image, surface, format)) {
          error = image->error();
          return;
        }
        auto* image_pointer = image.get();
        render_targets.emplace(key, std::move(image));
        const auto owner = next_edram_owner++;
        render_target_owners.emplace(key, owner);
        owner_render_targets.emplace(owner, image_pointer);
      }
      active_targets[slot] = render_targets.at(key).get();
      if (!acquire_color_ownership(key, *active_targets[slot],
                                   surface)) return;
      active_write_masks[slot] = target.write_mask;
      active_blends[slot] = target.blend;
    }
    if (!self().fill_unbound_color_slots(state, height, color_count,
                                         active_targets, active_write_masks,
                                         active_blends)) return;
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
    if (const auto existing = depth_targets.find(key);
        existing != depth_targets.end() &&
        existing->second->height() < surface.height_pixels) {
      const auto owner = depth_target_owners.at(key);
      if (edram_ownership.owned_tile_count(owner) &&
          !flush_depth_owner(owner)) return;
      auto retired = std::move(existing->second);
      owner_depth_targets.erase(owner);
      depth_target_owners.erase(key);
      depth_targets.erase(existing);
      retire_resource(std::move(retired));
    }
    if (!depth_targets.contains(key)) {
      auto image = std::make_unique<DepthTargetImage>();
      if (!self().create_depth_target(
              *image, surface,
              static_cast<DepthRenderTargetFormat>(state.depth_target.format))) {
        error = image->error();
        return;
      } else {
        auto* image_pointer = image.get();
        depth_targets.emplace(key, std::move(image));
        const auto owner = next_edram_owner++;
        depth_target_owners.emplace(key, owner);
        owner_depth_targets.emplace(owner, image_pointer);
      }
    }
    if (const auto found = depth_targets.find(key);
        found != depth_targets.end())
      active_depth = found->second.get();
    if (active_depth &&
        !acquire_depth_ownership(key, *active_depth, surface)) return;
  }
  // Phase 15: the guest-memory mirror is populated lazily. Vertex fetches
  // request only the physical ranges described by the active Xenos fetch
  // constants instead of synchronizing the entire 512 MiB aperture.
  if (memory) {
    for (const auto& fetch_group : state.vertex_buffers) {
      for (const auto& fetch : fetch_group) {
        if (!fetch || !fetch->valid || !fetch->size_dwords) continue;
        const auto bytes64 = std::uint64_t{fetch->size_dwords} * 4u;
        if (bytes64 > UINT32_MAX ||
            !mirror.synchronize_range(
                fetch->physical_address, static_cast<std::uint32_t>(bytes64),
                memory::GpuRangeUsage::VertexBuffer)) {
          error = mirror.error().empty()
                             ? named("vertex fetch range is invalid")
                             : mirror.error();
          return;
        }
      }
    }
  }
  auto constants = resources.constants();
  if (!resource_state.write_constant_buffer(constants)) {
    error = named("shader constant upload failed");
    ready = false;
    return;
  }
  std::array<bool, 32> used{};
  const auto mark_used = [&](const ir::ShaderReference& shader) {
    const auto found = shader_textures.find(shader.hash);
    if (shader.valid && found != shader_textures.end())
      for (const auto slot : found->second) if (slot < used.size()) used[slot] = true;
  };
  mark_used(draw->vertex_shader);
  mark_used(draw->pixel_shader);
  if (memory) {
    const auto dirty_epoch = memory->coherency().current_epoch();
    for (std::uint32_t slot = 0; slot < used.size(); ++slot) {
      if (!used[slot] || !state.textures[slot]) continue;
      const auto& descriptor = *state.textures[slot];
      const auto key = descriptor.hash();
      auto existing = textures.find(key);
      const bool already_cached = existing != textures.end();
      const bool dirty = already_cached &&
                        texture_dirty.consume_dirty(
                            key, memory->coherency(), dirty_epoch);
      const bool refresh = !already_cached || dirty;
      if (dirty) ++performance.texture_cache_invalidations;
      if (refresh) {
        // Texture decoding is still CPU-side. Preserve GPU-resident
        // memexport normally, but if a later draw actually samples a
        // GPU-authored texture, download only that texture's subresources.
        const auto source_layout = build_texture_layout(descriptor);
        if (!source_layout.valid) {
          error = source_layout.error;
          return;
        }
        for (const auto& subresource : source_layout.subresources) {
          if (subresource.guest_size_bytes > UINT32_MAX ||
              !mirror.make_cpu_visible(
                  subresource.guest_address,
                  static_cast<std::uint32_t>(subresource.guest_size_bytes),
                  memory::GpuRangeUsage::Texture)) {
            error = mirror.error().empty()
                               ? named("GPU-authored texture range is invalid")
                               : mirror.error();
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
          error = named("texture snapshot range is invalid");
          return;
        }
        std::vector<std::byte> texture_snapshot(
            static_cast<std::size_t>(texture_snapshot_end -
                                     texture_snapshot_base));
        if (!memory->copy_physical_range(texture_snapshot_base,
                                         texture_snapshot)) {
          error = named("texture snapshot failed");
          return;
        }
        const auto decoded =
            decode_texture(descriptor, texture_snapshot, texture_snapshot_base);
        if (!decoded.valid) {
          error = decoded.error;
          return;
        }
        auto image = std::make_unique<TextureImage>();
        if (!self().create_texture(*image, decoded) ||
            !self().bind_texture(slot, descriptor, *image)) {
          error = image->error().empty() ? resources.error() : image->error();
          if (image->unsupported_format()) ++unsupported.unsupported_texture_formats;
          return;
        }
        texture_dirty.track_clean(key, decoded.layout, dirty_epoch);
        if (existing != textures.end()) {
          auto retired = std::move(existing->second);
          existing->second = std::move(image);
          retire_resource(std::move(retired));
        } else {
          textures.emplace(key, std::move(image));
        }
      } else if (!self().bind_texture(slot, descriptor, *existing->second)) {
        error = resources.error();
        return;
      }
    }
  }
#ifdef XENON_HAS_DXC
  if (memory && draw->vertex_shader.valid &&
      draw->pixel_shader.valid) {
    const auto vertex = shaders.find(draw->vertex_shader.hash);
    const auto pixel = shaders.find(draw->pixel_shader.hash);
    if (vertex == shaders.end() || pixel == shaders.end()) {
      error = named("draw references a shader that has not been compiled");
      return;
    }
    const auto decoded_vertex =
        decoded_shaders.find(draw->vertex_shader.hash);
    const auto decoded_pixel =
        decoded_shaders.find(draw->pixel_shader.hash);
    if (decoded_vertex == decoded_shaders.end() ||
        decoded_pixel == decoded_shaders.end()) {
      error = named("draw lacks decoded shader reflection");
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
      const auto plan = resource_state.plan_memexport(decoded);
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
        if (!mirror.synchronize()) {
          error = mirror.error();
          return;
        }
      } else {
        for (const auto& range : memexport_ranges) {
          const auto address = range.base_address_dwords << 2u;
          if (range.size_bytes &&
              !mirror.synchronize_range(
                  address, range.size_bytes,
                  memory::GpuRangeUsage::MemoryExport)) {
            error = mirror.error().empty()
                               ? named("memexport range is invalid")
                               : mirror.error();
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
      auto found = writable_guest_memory_shaders.find(variant_key);
      if (found != writable_guest_memory_shaders.end())
        return found->second;
      ShaderLoweringOptions lowering_options{};
      lowering_options.force_guest_memory_rw = true;
      const auto lowered = HlslShaderLowerer::lower(decoded, lowering_options);
      unsupported.unsupported_shader_instructions += lowered.unsupported_instructions;
      unsupported.unsupported_shader_features += lowered.unsupported_features;
      unsupported.unsupported_fetch_formats += lowered.unsupported_fetch_formats;
      if (!lowered.complete) {
        error = lowered.diagnostics.empty()
                           ? named("writable guest-memory shader lowering failed")
                           : lowered.diagnostics.front();
        return std::shared_ptr<const CompiledShader>{};
      }
      ShaderCompileOptions compile_options{};
      compile_options.format = Api::kShaderBinaryFormat;
      const auto compiled =
          shader_cache.get_or_compile(lowered, compile_options);
      if (!compiled->succeeded) {
        error = compiled->diagnostics.empty()
                           ? named("writable guest-memory shader compilation failed")
                           : compiled->diagnostics.front();
        return std::shared_ptr<const CompiledShader>{};
      }
      ++compiled_shader_count;
      writable_guest_memory_shaders.emplace(variant_key, compiled);
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
      auto variant = float20_depth_shaders.find(variant_key);
      if (variant == float20_depth_shaders.end()) {
        ShaderLoweringOptions lowering_options{};
        lowering_options.pixel_depth_output =
            PixelDepthOutputMode::Float20e4NearestEven;
        lowering_options.force_sample_frequency = per_sample;
        lowering_options.force_guest_memory_rw = memexport_writable;
        const auto lowered =
            HlslShaderLowerer::lower(decoded_pixel->second, lowering_options);
        unsupported.unsupported_shader_instructions += lowered.unsupported_instructions;
        unsupported.unsupported_shader_features += lowered.unsupported_features;
        unsupported.unsupported_fetch_formats += lowered.unsupported_fetch_formats;
        if (!lowered.complete) {
          error = lowered.diagnostics.empty()
                             ? "D24FS8 pixel shader lowering failed"
                             : lowered.diagnostics.front();
          return;
        }
        ShaderCompileOptions compile_options{};
        compile_options.format = Api::kShaderBinaryFormat;
        const auto compiled =
            shader_cache.get_or_compile(lowered, compile_options);
        if (!compiled->succeeded) {
          error = compiled->diagnostics.empty()
                             ? "D24FS8 pixel shader compilation failed"
                             : compiled->diagnostics.front();
          return;
        }
        ++compiled_shader_count;
        variant = float20_depth_shaders
                      .emplace(variant_key, compiled)
                      .first;
      }
      pixel_shader = variant->second;
      float20_depth_variant = true;
    }

    if (draw->index_buffer.valid && draw->index_buffer.length_bytes &&
        !mirror.make_cpu_visible(
            draw->index_buffer.physical_address,
            draw->index_buffer.length_bytes,
            memory::GpuRangeUsage::IndexBuffer)) {
      error = mirror.error();
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
        error = named("index snapshot range is invalid");
        return;
      }
      index_snapshot.resize(static_cast<std::size_t>(index_bytes));
      index_snapshot_base = draw->index_buffer.physical_address;
      if (!memory->copy_physical_range(index_snapshot_base,
                                       index_snapshot)) {
        error = named("index snapshot failed");
        return;
      }
    }
    const auto batch = process_primitives(
        *draw, index_snapshot, primitive_options, index_snapshot_base);
    if (!batch.valid) {
      error = batch.error;
      return;
    }
    if (batch.requires_rectangle_expansion) {
      // A host-only fallback geometry shader, not the title's own -
      // Part 7 of the AC6 Runtime Readiness pass wants every draw that
      // actually takes this path counted, not merely its (rare) compile
      // failure.
      ++unsupported.fallback_shader_uses;
      if (!rectangle_list_shader) {
        ShaderCompileOptions options{};
        options.format = Api::kShaderBinaryFormat;
        rectangle_list_shader = shader_cache.get_or_compile(
            make_rectangle_list_geometry_shader(), options);
        if (!rectangle_list_shader->succeeded) {
          error = rectangle_list_shader->diagnostics.empty()
                             ? named("RectangleList shader compilation failed")
                             : rectangle_list_shader->diagnostics.front();
          return;
        }
        ++compiled_shader_count;
      }
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
        pipeline_key ^= std::uint64_t(Api::color_format(active_targets[i]))
                        << (16u + i * 8u);
      }
    } else {
      pipeline_key ^= 0xD3F7000000000000ull;
    }
    if (float20_depth_variant) pipeline_key ^= 0x20E4D24F5A17A11Cull;
    if (memexport_writable) pipeline_key ^= 0x4D454D4558504F52ull;
    if (!pipelines.contains(pipeline_key)) {
      auto pipeline = std::make_unique<GraphicsPipeline>();
      std::array<typename Api::Format, 4> formats{};
      std::array<std::uint8_t, 4> write_masks{};
      std::array<BlendState, 4> blend_states{};
      for (std::size_t i = 0; i < color_count; ++i) {
        formats[i] = Api::color_format(active_targets[i]);
        if (active_targets[i]) {
          write_masks[i] = active_write_masks[i];
          blend_states[i] = active_blends[i];
        }
      }
      const std::span<const typename Api::Format> format_span(formats.data(),
                                                              color_count);
      const std::span<const std::uint8_t> write_mask_span(
          write_masks.data(), color_count);
      const std::span<const BlendState> blend_state_span(
          blend_states.data(), color_count);
      if (!pipeline->initialize(
              context.device(), self().pipeline_root(),
              *vertex_shader, *pixel_shader,
              batch.requires_rectangle_expansion
                  ? rectangle_list_shader.get() : nullptr,
              format_span, samples,
              batch.topology, state.raster, write_mask_span, blend_state_span,
              active_depth ? active_depth->format() : Api::kUndefinedFormat,
              active_depth ? &state.depth_target : nullptr)) {
        error = pipeline->error();
        return;
      }
      pipelines.emplace(pipeline_key, std::move(pipeline));
    }
    const auto index_byte_size =
        batch.indexed ? batch.indices.size() * sizeof(std::uint32_t) : 0u;
    if (index_byte_size > kTransientUploadBytes) {
      error = named("transient upload arena exhausted by one submission");
      return;
    }
    const auto* pipeline = pipelines.at(pipeline_key).get();
    const auto render_width = color_count
                                  ? self().color_render_width(state, active_targets[0])
                                  : active_depth
                                        ? active_depth->width()
                                        : (std::max)(1u, std::uint32_t(
                                                             state.raster.surface_pitch));
    const auto render_height = color_count
                                   ? self().color_render_height(state, active_targets[0])
                                   : active_depth
                                         ? active_depth->height()
                                         : (std::max)(1u, std::uint32_t(
                                                              (std::max)(1, state.raster.scissor_bottom)));
    const auto scissor_left = (std::max)(0, state.raster.scissor_left);
    const auto scissor_top = (std::max)(0, state.raster.scissor_top);
    // A backend without a FRONT_AND_BACK cull mode still has to run the vertex
    // shader of a fully culled memexport draw; an empty scissor keeps any
    // triangle from reaching the render targets.
    const bool suppress_rasterization =
        Api::kEmulatesFrontAndBackCull &&
        batch.topology == HostPrimitiveTopology::TriangleList &&
        state.raster.cull_front && state.raster.cull_back && vertex_memexport;
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
    if (!self().record_draw(
            DrawRecording{state, batch, *pipeline, active_targets, color_count,
                          active_depth, index_byte_size, memexport_writable,
                          render_width, render_height, scissor_left,
                          scissor_top, scissor_right, scissor_bottom},
            draw_record_failed)) {
      error = queue.error();
      return;
    } else if (draw_record_failed) {
      return;
    } else if (memexport_writable) {
      if (dynamic_memexport_range) {
        // Unusual shaders may synthesize eA dynamically. Until runtime
        // address analysis is available, preserve correctness by treating
        // the complete physical aperture as GPU-owned rather than risking a
        // stale CPU upload over an unknown export.
        mirror.mark_gpu_write(0, memory::kPhysicalMemorySize);
        texture_dirty.mark_dirty(0, memory::kPhysicalMemorySize);
      } else {
        for (const auto& range : memexport_ranges) {
          const auto address = range.base_address_dwords << 2u;
          mirror.mark_gpu_write(address, range.size_bytes);
          texture_dirty.mark_dirty(address, range.size_bytes);
        }
      }
    }
  }
#endif
}

}  // namespace xenon::gpu::detail
