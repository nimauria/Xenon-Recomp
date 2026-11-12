#pragma once

// Draw consumption for BackendCore: EDRAM render targets, textures, shader
// variants and pipelines, then the backend's record_draw() hook. Included by
// backend_core.hpp.

#include "graphics/common/backend_core.hpp"

namespace xenon::gpu::detail {

template <typename Derived, typename Api>
void BackendCore<Derived, Api>::consume_draw(const ir::DrawPacket* draw,
                                             const DrawResourceState& state) {
  // Diagnostic: per-phase time of a draw slower than 50 ms (the GPU pump
  // thread runs draws between ring drains and vsync interrupts), and, in
  // probe logs, every phase's running total over all draws every 4096 draws
  // (draw_phase_totals_diag.log) - where an ordinary draw's time goes.
  struct DrawPhases {
    struct Totals {
      std::array<const char*, 8> names{};
      std::array<std::uint64_t, 8> ns{};
      std::uint64_t draws{};
    };
    static Totals& totals() {
      static Totals value;
      return value;
    }
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point mark = start;
    std::array<std::pair<const char*, long long>, 8> phases{};
    std::size_t count = 0;
    void at(const char* name) {
      const auto now = std::chrono::steady_clock::now();
      const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - mark).count();
      // Each phase name (a literal) owns one totals slot, whichever phase a
      // draw that returns early ends on.
      auto& sums = totals();
      for (std::size_t slot = 0; slot < sums.names.size(); ++slot) {
        if (sums.names[slot] && std::string_view(sums.names[slot]) != name) continue;
        sums.names[slot] = name;
        sums.ns[slot] += static_cast<std::uint64_t>(ns);
        break;
      }
      if (count < phases.size()) phases[count++] = {name, ns / 1000000};
      mark = now;
    }
    ~DrawPhases() {
      at("record and finish");
      auto& sums = totals();
      if (xenon::logging::probe_logs_enabled() && (++sums.draws & 4095u) == 0u) {
        std::string text;
        for (std::size_t i = 0; i < sums.names.size() && sums.names[i]; ++i)
          text += std::string(sums.names[i]) + " " + std::to_string(sums.ns[i] / 1000000u) +
                  " ms; ";
        xenon::logging::append_probe_log("draw_phase_totals_diag.log", "%llu draws: %s\n",
                                         static_cast<unsigned long long>(sums.draws),
                                         text.c_str());
      }
      const auto total = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count();
      if (total < 50) return;
      std::string text;
      for (std::size_t i = 0; i < count; ++i)
        text += std::string(phases[i].first) + " " + std::to_string(phases[i].second) + " ms; ";
      xenon::logging::append_probe_log("ir_timing_diag.log", "draw phases (%lld ms): %s\n",
                                       static_cast<long long>(total), text.c_str());
    }
  } phases;
  static const auto activity_key = xenon::logging::intern_activity(
      {"gpu", "draw", {"prim", "indices", "vs", "ps"}, false});
  xenon::logging::ActivityScope activity(
      activity_key, {static_cast<std::uint64_t>(draw->primitive_type), draw->index_count,
                     draw->vertex_shader.hash, draw->pixel_shader.hash});
  pipeline_states.insert(state.pipeline_hash(*draw));
  std::array<RenderTargetImage*, 4> active_targets{};
  std::array<std::uint8_t, 4> active_write_masks{};
  std::array<BlendState, 4> active_blends{};
  // Only the colors the pixel shader exports are bound. A shader that is not
  // decoded yet keeps every slot and fails on its missing reflection below.
  const auto decoded_pixel_shader = draw->pixel_shader.valid
                                        ? decoded_shaders.find(draw->pixel_shader.hash)
                                        : decoded_shaders.end();
  // A multipass vertex shader (SQ_PROGRAM_CNTL.VS_EXPORT_MODE) only exports
  // to memory: nothing is rasterized, so no render target is bound.
  const bool rasterizes = !state.program.multipass();
  const auto color_plan =
      rasterizes ? plan_color_targets(
                       state, decoded_pixel_shader != decoded_shaders.end()
                                  ? shader_color_output_mask(decoded_pixel_shader->second.reflection)
                                  : std::uint8_t{0xF})
                 : ColorTargetPlan{};
  const auto color_count =
      static_cast<std::size_t>(color_plan.attachment_count);
  DepthTargetImage* active_depth = nullptr;
  // A depth target on the EDRAM base of a color target the draw writes is
  // not bound: one surface cannot be bound twice, and depth is the one
  // dropped, as Xenia does.
  bool depth_aliases_color = false;
  for (std::size_t slot = 0; slot < color_count; ++slot)
    depth_aliases_color |= color_plan.enabled[slot] &&
                           state.color_targets[slot].base_tile == state.depth_target.base_tile;
  const bool depth_bound =
      rasterizes && !depth_aliases_color &&
      (state.edram_mode == EdramMode::ColorDepth || state.edram_mode == EdramMode::DepthOnly) &&
      (state.depth_target.test_enabled || state.depth_target.write_enabled ||
       state.depth_target.stencil_enabled);
  // The surfaces bound to this draw never share EDRAM, so each one ends where
  // the next bound surface begins (AC6's 640x360 4x MSAA color target at tile
  // 0 is followed by its depth at tile 720).
  std::vector<std::uint32_t> bound_bases;
  for (std::size_t slot = 0; slot < color_count; ++slot)
    if (color_plan.enabled[slot]) bound_bases.push_back(state.color_targets[slot].base_tile);
  if (depth_bound) bound_bases.push_back(state.depth_target.base_tile);
  if (color_count && (!state.raster.surface_pitch ||
                      state.raster.scissor_bottom <= 0)) {
    error = named("Xenos MRT draw has an invalid EDRAM surface extent");
    return;
  }
  if (color_count && state.raster.surface_pitch &&
      state.raster.scissor_bottom > 0) {
    // Sized to the rows the draw can reach, not the (often open) scissor,
    // and never taller than EDRAM can hold at this pitch.
    const auto height = (std::min)(
        std::uint32_t((std::max)(1, state.raster.drawable_bottom)),
        EdramSurfaceLayout{0u, state.raster.surface_pitch, 1u,
                           static_cast<MsaaSamples>(state.raster.msaa_samples_log2), false,
                           false}
            .max_resident_height_pixels());
    for (std::size_t slot = 0; slot < color_count; ++slot) {
      if (!color_plan.enabled[slot]) continue;
      const auto& target = state.color_targets[slot];
      const auto format = static_cast<ColorRenderTargetFormat>(target.format);
      EdramSurfaceLayout surface{target.base_tile, state.raster.surface_pitch,
          height, static_cast<MsaaSamples>(state.raster.msaa_samples_log2),
          color_render_target_is_64bpp(format), false};
      surface.height_pixels = (std::min)(whole_tile_height_pixels(surface),
                                         rows_before_next_surface(surface, bound_bases));
      const auto key = surface.identity_hash() ^
                       (std::uint64_t(target.format) << 56u);
      auto existing = render_targets.find(key);
      if (existing != render_targets.end() &&
          existing->second->height() < surface.height_pixels) {
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
    EdramSurfaceLayout surface{
        state.depth_target.base_tile, state.raster.surface_pitch,
        std::uint32_t((std::max)(1, state.raster.drawable_bottom)),
        static_cast<MsaaSamples>(state.raster.msaa_samples_log2), false, true};
    surface.height_pixels = (std::min)(whole_tile_height_pixels(surface),
                                       rows_before_next_surface(surface, bound_bases));
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
  phases.at("render targets");
#ifdef XENON_HAS_DXC
  // With no bound render target and no memory export a draw changes nothing
  // the guest can observe, so it needs no vertex or texture uploads (AC6's
  // single-point draws spent 50-225 ms each syncing vertex buffers and then
  // returned). A shader whose reflection is missing still reaches the error
  // below rather than being skipped here.
  if (!color_count && !active_depth) {
    const auto may_export = [&](const ir::ShaderReference& shader) {
      if (!shader.valid) return false;
      const auto decoded = decoded_shaders.find(shader.hash);
      return decoded == decoded_shaders.end() || decoded->second.reflection.memory_exports != 0;
    };
    if (!may_export(draw->vertex_shader) && !may_export(draw->pixel_shader)) return;
  }
#endif
  // Phase 15: the guest-memory mirror is populated lazily. Vertex fetches
  // request only the physical ranges of the fetch constants the draw's shaders
  // read: the register file keeps stale constants for buffers no shader
  // fetches, and syncing those too cost AC6 up to 100 ms per draw. A shader
  // with no reflection syncs every valid constant.
  if (memory) {
    // Vertex data is read as guest memory stood at the latest sync point, as
    // textures are: the CPU writes since then are folded in once, at the next
    // one, not once for every buffer of every draw.
    const auto vertex_read_epoch = guest_reads.read_epoch(*memory);
    std::array<bool, 96> fetched{};
#ifdef XENON_HAS_DXC
    bool all_fetched = false;
    for (const auto* shader : {&draw->vertex_shader, &draw->pixel_shader}) {
      if (!shader->valid) continue;
      const auto decoded = decoded_shaders.find(shader->hash);
      if (decoded == decoded_shaders.end()) {
        all_fetched = true;
        continue;
      }
      for (const auto index : decoded->second.reflection.vertex_fetches)
        if (index < fetched.size()) fetched[index] = true;
    }
#else
    const bool all_fetched = true;
#endif
    for (std::uint32_t slot = 0; slot < state.vertex_buffers.size(); ++slot) {
      for (std::uint32_t select = 0; select < 3; ++select) {
        const auto& fetch = state.vertex_buffers[slot][select];
        if (!all_fetched && !fetched[slot * 3u + select]) continue;
        if (!fetch || !fetch->valid || !fetch->size_dwords) continue;
        const auto bytes64 = std::uint64_t{fetch->size_dwords} * 4u;
        const auto sync_start = std::chrono::steady_clock::now();
        if (bytes64 > UINT32_MAX ||
            !mirror.synchronize_range(
                fetch->physical_address, static_cast<std::uint32_t>(bytes64),
                memory::GpuRangeUsage::VertexBuffer, vertex_read_epoch)) {
          error = mirror.error().empty()
                             ? named("vertex fetch range is invalid")
                             : mirror.error();
          return;
        }
        static std::atomic_uint32_t vertex_sync_probe_count{};
        if (vertex_sync_probe_count.fetch_add(1u, std::memory_order_relaxed) < 200u) {
          xenon::logging::append_probe_log(
              "vertex_sync_diag.log", "fetch %u.%u address=0x%08X bytes=%llu took %lld ms\n",
              slot, select, fetch->physical_address,
              static_cast<unsigned long long>(bytes64),
              static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::steady_clock::now() - sync_start)
                                         .count()));
        }
      }
    }
  }
  phases.at("vertex buffers");
  auto constants = resources.constants();
  if (!resource_state.write_constant_buffer(constants)) {
    error = named("shader constant upload failed");
    ready = false;
    return;
  }
  probe_skinning_constants(draw->vertex_shader, constants, state.vertex_buffers);
  std::array<bool, 32> used{};
  const auto mark_used = [&](const ir::ShaderReference& shader) {
    const auto found = shader_textures.find(shader.hash);
    if (shader.valid && found != shader_textures.end())
      for (const auto slot : found->second) if (slot < used.size()) used[slot] = true;
  };
  mark_used(draw->vertex_shader);
  mark_used(draw->pixel_shader);
  if (memory) {
    // Textures are read as guest memory stood at the latest GPU/CPU sync point
    // (sync_guest_reads()): the hardware samples them microseconds after it,
    // while this draw may run milliseconds later. AC6's movie decoder refills
    // the planes this draw samples as soon as the interrupt before it wakes
    // the decoder; reading them when the draw ran showed the top of the
    // picture a frame ahead, in the wrong colours. Changes after the sync
    // point are not visible yet and are picked up by a later draw.
    const auto dirty_epoch = guest_reads.read_epoch(*memory);
    // Textures this draw has already loaded: another slot sampling the same
    // one binds that image.
    std::vector<std::uint64_t> loaded_keys;
    for (std::uint32_t slot = 0; slot < used.size(); ++slot) {
      if (!used[slot] || !state.textures[slot]) continue;
      const auto& descriptor = *state.textures[slot];
      const auto key = descriptor.hash();
      // Each distinct shader/texture pairing once, when it first appears.
      static std::mutex texture_probe_mutex;
      static std::unordered_set<std::uint64_t> texture_probe_seen;
      bool first_use = false;
      if (xenon::logging::probe_logs_enabled()) {
        std::scoped_lock probe_lock(texture_probe_mutex);
        first_use = texture_probe_seen.insert(key * 31u ^ draw->pixel_shader.hash).second;
      }
      if (first_use) {
        xenon::logging::append_probe_log(
            "texture_fetch_diag.log",
            "%lld ps=%016llX slot %u: format=%u endian=%u swizzle=0x%03X signs=%u%u%u%u "
            "%ux%u pitch=%u tiled=%d base=0x%08X exp_adjust=%d\n",
            static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now().time_since_epoch())
                                       .count()),
            static_cast<unsigned long long>(draw->pixel_shader.hash), slot,
            unsigned(descriptor.format), unsigned(descriptor.endian),
            unsigned(descriptor.swizzle), unsigned(descriptor.signs[0]),
            unsigned(descriptor.signs[1]), unsigned(descriptor.signs[2]),
            unsigned(descriptor.signs[3]), descriptor.width, descriptor.height,
            descriptor.pitch, int(descriptor.tiled), descriptor.base_address,
            int(descriptor.exp_adjust));
      }
      auto existing = textures.find(key);
      const bool already_cached = existing != textures.end();
      const bool loaded_here =
          std::find(loaded_keys.begin(), loaded_keys.end(), key) != loaded_keys.end();
      const bool dirty = already_cached && !loaded_here &&
                         cached_texture_stale(texture_dirty, guest_reads, key,
                                              memory->coherency(), dirty_epoch);
      if (!already_cached || dirty) {
        if (dirty) ++performance.texture_cache_invalidations;
        const auto layout = build_texture_layout(descriptor);
        if (!layout.valid) {
          error = layout.error;
          return;
        }
        if (!layout.format.host_supported()) {
          error = named("Xenos texture format " + std::to_string(descriptor.format) + " (" +
                        std::string(layout.format.name) + ") has no lossless native mapping");
          ++unsupported.unsupported_texture_formats;
          return;
        }
        std::uint32_t texture_base = memory::kPhysicalMemorySize;
        std::uint64_t texture_end = 0u;
        for (const auto& subresource : layout.subresources) {
          texture_base = (std::min)(texture_base, subresource.guest_address);
          texture_end = (std::max)(
              texture_end, std::uint64_t{subresource.guest_address} + subresource.guest_size_bytes);
        }
        if (texture_base >= memory::kPhysicalMemorySize ||
            texture_end > memory::kPhysicalMemorySize || texture_end <= texture_base) {
          error = named("texture guest range is invalid");
          return;
        }
        const auto texture_size = static_cast<std::uint32_t>(texture_end - texture_base);
        // The copy the latest sync point holds, whatever the CPU wrote since.
        // (Page epochs cannot say whether a write came before or after it: a
        // page records only its newest write.) Otherwise the texture is read
        // where it lies: the mirror takes the bytes the CPU changed, and bytes
        // a resolve or memory export wrote are already there - the texture
        // never comes back to the CPU.
        auto capture = guest_reads.take_capture(key, texture_base, texture_size);
        if (capture) {
          ++performance.texture_sync_captures;
        } else {
          for (const auto& subresource : layout.subresources) {
            if (subresource.guest_size_bytes > UINT32_MAX ||
                !mirror.synchronize_range(subresource.guest_address,
                                          static_cast<std::uint32_t>(subresource.guest_size_bytes),
                                          memory::GpuRangeUsage::Texture, dirty_epoch)) {
              error = mirror.error().empty() ? named("texture guest range is invalid")
                                             : mirror.error();
              return;
            }
          }
        }
        guest_reads.refreshed(key, texture_base, texture_size, dirty_epoch);
        static const auto load_key = xenon::logging::intern_activity(
            {"gpu", "texture load", {"format", "width", "height", "address"}, false});
        xenon::logging::ActivityScope load_activity(
            load_key, {static_cast<std::uint64_t>(descriptor.format), descriptor.width,
                       descriptor.height, texture_base});
        // A cached texture of the same key is reloaded in place: the work is
        // queued after the draws that sampled it, so they read the old texels.
        std::unique_ptr<TextureImage> created;
        TextureImage* image = already_cached ? existing->second.get() : nullptr;
        if (image && !image->matches(layout)) image = nullptr;
        if (!image) {
          created = std::make_unique<TextureImage>();
          image = created.get();
        }
        const std::span<const std::byte> capture_bytes =
            capture ? std::span<const std::byte>(*capture) : std::span<const std::byte>{};
        if (!self().load_texture(*image, layout, capture_bytes, texture_base)) {
          if (image->unsupported_format()) ++unsupported.unsupported_texture_formats;
          if (!created && already_cached) {
            // The image may be half written: decode it afresh next time.
            retire_resource(std::move(existing->second));
            textures.erase(existing);
            texture_dirty.erase(key);
          }
          return;
        }
        ++performance.gpu_texture_loads;
        texture_dirty.track_clean(key, layout, dirty_epoch);
        if (created) {
          if (already_cached) {
            auto retired = std::move(existing->second);
            existing->second = std::move(created);
            retire_resource(std::move(retired));
          } else {
            textures.emplace(key, std::move(created));
          }
        }
        loaded_keys.push_back(key);
      } else {
        guest_reads.reused(key);
      }
      if (!self().bind_texture(slot, descriptor, *textures.at(key))) {
        error = resources.error();
        return;
      }
    }
  }
  phases.at("textures");
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

    phases.at("shader lookup");
    // Known memexport targets stay range-driven. Only shaders whose export
    // address cannot be resolved statically request the deliberate full-range
    // fallback required for genuinely unrestricted guest-memory access.
    if (memexport_writable) {
      if (dynamic_memexport_range) {
        const auto sync_start = std::chrono::steady_clock::now();
        if (!mirror.synchronize()) {
          error = mirror.error();
          return;
        }
        // Diagnostic: the whole-aperture fallback is the most expensive thing
        // a draw can do; say when it runs and for which shaders.
        const auto sync_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - sync_start)
                                 .count();
        xenon::logging::append_probe_log(
            "ir_timing_diag.log",
            "dynamic memexport full mirror sync %lld ms (vs %016llX memexport=%d, ps %016llX memexport=%d)\n",
            static_cast<long long>(sync_ms),
            static_cast<unsigned long long>(draw->vertex_shader.hash), int(vertex_memexport),
            static_cast<unsigned long long>(draw->pixel_shader.hash), int(pixel_memexport));
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

    phases.at("memexport ranges");
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
    // Pixel shader variants: D24FS8 depth output (float20e4 rounding, per
    // sample under MSAA) and PsParamGen (SQ_PROGRAM_CNTL.PARAM_GEN).
    const bool float20_depth =
        active_depth && active_depth->requires_float24_conversion() &&
        (state.depth_target.test_enabled || state.depth_target.write_enabled);
    const auto param_gen = state.program.param_gen_interpolator();
    const bool point_primitive = draw->primitive_type == PrimitiveType::PointList;
    if (float20_depth || param_gen != ShaderProgramState::kNoParamGen) {
      const bool per_sample = float20_depth && samples != MsaaSamples::X1;
      const std::uint64_t variant_key =
          draw->pixel_shader.hash ^ (float20_depth ? 0xD24F20E4D24F20E4ull : 0ull) ^
          (per_sample ? 0x8000000000000000ull : 0ull) ^
          (memexport_writable ? 0x0400000000000000ull : 0ull) ^
          (param_gen != ShaderProgramState::kNoParamGen
               ? 0x5041524D47454E00ull ^ (std::uint64_t(param_gen) << 40u) ^
                     (point_primitive ? 0x0000100000000000ull : 0ull)
               : 0ull);
      auto variant = float20_depth_shaders.find(variant_key);
      if (variant == float20_depth_shaders.end()) {
        ShaderLoweringOptions lowering_options{};
        if (float20_depth)
          lowering_options.pixel_depth_output = PixelDepthOutputMode::Float20e4NearestEven;
        lowering_options.force_sample_frequency = per_sample;
        lowering_options.force_guest_memory_rw = memexport_writable;
        lowering_options.param_gen_interpolator = param_gen;
        lowering_options.param_gen_point = point_primitive;
        const auto lowered =
            HlslShaderLowerer::lower(decoded_pixel->second, lowering_options);
        unsupported.unsupported_shader_instructions += lowered.unsupported_instructions;
        unsupported.unsupported_shader_features += lowered.unsupported_features;
        unsupported.unsupported_fetch_formats += lowered.unsupported_fetch_formats;
        if (!lowered.complete) {
          error = lowered.diagnostics.empty()
                             ? "pixel shader variant lowering failed"
                             : lowered.diagnostics.front();
          return;
        }
        ShaderCompileOptions compile_options{};
        compile_options.format = Api::kShaderBinaryFormat;
        const auto compiled =
            shader_cache.get_or_compile(lowered, compile_options);
        if (!compiled->succeeded) {
          error = compiled->diagnostics.empty()
                             ? "pixel shader variant compilation failed"
                             : compiled->diagnostics.front();
          return;
        }
        ++compiled_shader_count;
        variant = float20_depth_shaders
                      .emplace(variant_key, compiled)
                      .first;
      }
      pixel_shader = variant->second;
      float20_depth_variant = float20_depth;
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
    // Xenos draws points as sprites (PA_SU_POINT_SIZE, or the size the
    // vertex shader exports), expanded by a geometry shader.
    const bool point_sprites = batch.topology == HostPrimitiveTopology::PointList;
    if (point_sprites && !point_sprite_shader) {
      ShaderCompileOptions options{};
      options.format = Api::kShaderBinaryFormat;
      point_sprite_shader =
          shader_cache.get_or_compile(make_point_sprite_geometry_shader(), options);
      if (!point_sprite_shader->succeeded) {
        error = point_sprite_shader->diagnostics.empty()
                    ? named("point sprite shader compilation failed")
                    : point_sprite_shader->diagnostics.front();
        return;
      }
      ++compiled_shader_count;
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
    // Face culling applies only to polygonal primitive types: a rectangle
    // list (a full-screen pass, typically) is drawn whatever the cull mode the
    // scene left set. D3D12 has no FRONT_AND_BACK cull mode and Vulkan does,
    // but the common result is simpler: once Xenos has requested both faces
    // culled, no polygon can reach rasterization.
    const auto raster = host_raster_state(state.raster, draw->primitive_type);
    if (batch.topology == HostPrimitiveTopology::TriangleList &&
        raster.cull_front && raster.cull_back &&
        !vertex_memexport) {
      return;
    }
    auto pipeline_key = state.pipeline_hash(*draw);
    pipeline_key ^= std::uint64_t(batch.topology) << 61u;
    if (batch.requires_rectangle_expansion)
      pipeline_key ^= 0x52454354414E474Cull;
    if (point_sprites) pipeline_key ^= 0x504F494E54535052ull;
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
              batch.requires_rectangle_expansion ? rectangle_list_shader.get()
              : point_sprites                    ? point_sprite_shader.get()
                                                 : nullptr,
              format_span, samples,
              batch.topology, raster, write_mask_span, blend_state_span,
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
        !rasterizes ||
        (Api::kEmulatesFrontAndBackCull &&
         batch.topology == HostPrimitiveTopology::TriangleList &&
         raster.cull_front && raster.cull_back && vertex_memexport);
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
    // The vertex shader maps render-target pixels to host NDC over the whole
    // render target, which is the host viewport (XenonDrawState[3].yz).
    const std::array<float, 2> pixels_to_ndc{2.0f / float(render_width),
                                             2.0f / float(render_height)};
    std::memcpy(constants.data() + kDrawStateOffset + 13 * 4, pixels_to_ndc.data(),
                sizeof(pixels_to_ndc));
    // Words 16 and 17 (XenonDrawState[4].xy): the Z part of the host depth
    // transform, whose depth range the backend gives its viewport.
    const auto depth_transform =
        host_depth_transform(state.raster, decoded_pixel->second.reflection.writes_depth);
    const std::array<float, 2> z_transform{depth_transform.ndc_scale, depth_transform.ndc_offset};
    std::memcpy(constants.data() + kDrawStateOffset + 16 * 4, z_transform.data(),
                sizeof(z_transform));
    // Word 18 (XenonDrawState[4].z): the primitive class PsParamGen reports
    // (1 polygonal, 2 line, 4 point) and 8 when a point's size comes from
    // its vertex shader.
    const auto type = draw->primitive_type;
    const bool line_primitive = type == PrimitiveType::LineList || type == PrimitiveType::LineStrip ||
                                type == PrimitiveType::LineLoop ||
                                type == PrimitiveType::TwoDLineStrip;
    const std::uint32_t primitive_flags =
        (primitive_is_polygonal(type) ? 1u : 0u) | (line_primitive ? 2u : 0u) |
        (point_primitive ? 4u : 0u) | (state.program.vertex_point_size() ? 8u : 0u);
    std::memcpy(constants.data() + kDrawStateOffset + 18 * 4, &primitive_flags,
                sizeof(primitive_flags));
    bool draw_record_failed = false;
    phases.at("constants, pipeline and uploads");
    if constexpr (requires { self().request_draw_statistics(); }) {
      if (tracing() && state.raster.msaa_samples_log2 != 0 && trace_probes_left != 0)
        self().request_draw_statistics();
    }
    auto blend_constant = state.blend_constant;
    if constexpr (Api::kSharedBlendFactorConstant) {
      std::array<BlendState, 4> draw_blends{};
      for (std::size_t i = 0; i < color_count; ++i)
        if (active_targets[i]) draw_blends[i] = active_blends[i];
      const auto host = shared_blend_factor_constant(
          std::span<const BlendState>(draw_blends.data(), color_count), state.blend_constant);
      blend_constant = host.value;
      if (!host.exact) ++unsupported.unsupported_blend_states;
    }
    if (!make_transient_upload_room(index_byte_size)) return;
    if (!self().record_draw(
            DrawRecording{state, batch, *pipeline, active_targets, color_count,
                          active_depth, index_byte_size, memexport_writable,
                          render_width, render_height, scissor_left,
                          scissor_top, scissor_right, scissor_bottom,
                          depth_transform.z_min, depth_transform.z_max, blend_constant},
            draw_record_failed)) {
      error = queue.error();
      return;
    } else if (draw_record_failed) {
      return;
    }
    probe_draw_output(*draw, state.color_targets[0].format, color_count, active_targets,
                      active_depth, state.raster.msaa_samples_log2);
    trace_vertex_inputs(*draw, state);
    if (memexport_writable) {
      if (dynamic_memexport_range) {
        // Unusual shaders may synthesize eA dynamically. Until runtime
        // address analysis is available, preserve correctness by treating
        // the complete physical aperture as GPU-owned rather than risking a
        // stale CPU upload over an unknown export.
        mirror.mark_gpu_write(0, memory::kPhysicalMemorySize);
        note_backend_write(texture_dirty, guest_reads, 0, memory::kPhysicalMemorySize);
      } else {
        for (const auto& range : memexport_ranges) {
          const auto address = range.base_address_dwords << 2u;
          mirror.mark_gpu_write(address, range.size_bytes);
          note_backend_write(texture_dirty, guest_reads, address, range.size_bytes);
        }
      }
    }
  }
#endif
}

}  // namespace xenon::gpu::detail
