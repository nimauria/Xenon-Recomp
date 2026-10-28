#pragma once

// Command dispatch, Xenos resolves and shader loads for BackendCore. Included
// by backend_core.hpp; see that header for the backend hooks.

#include "graphics/common/backend_core.hpp"
#include "xenon/logging/probe_log.hpp"

namespace xenon::gpu::detail {

template <typename Derived, typename Api>
void BackendCore<Derived, Api>::consume(const ir::Command& command) {
  collect_retired_resources();
  ++command_count;
  ++performance.commands;
  // RegisterWrite/DrawPacket/ShaderLoad carry state the host renderer needs.
  // PhysicalMemoryWrite/IndirectBuffer/ShaderPacket/SynchronizationPacket/
  // EventPacket/StatePacket record work the command processor has already
  // executed: memory writes reach guest memory (and its GPU coherency
  // tracking) through AddressSpace, fences and interrupts are delivered, and
  // constant/state packets are re-emitted as RegisterWrites. The renderer has
  // nothing left to do for them. A Type3Packet other than NOP is a packet
  // the command processor did not execute, which is a real gap.
  if (const auto* packet = std::get_if<ir::Type3Packet>(&command)) {
    if (packet->opcode == Type3Opcode::Nop) return;
    ++unsupported.unknown_packets;
    xenon::logging::Logger::instance().log_if_enabled(
        xenon::logging::Level::Warning, "gpu", [&] {
          return "unexecuted PM4 type-3 packet (opcode=0x" +
                 [](std::uint32_t value) {
                   char text[8];
                   std::snprintf(text, sizeof(text), "%02X", value);
                   return std::string(text);
                 }(static_cast<std::uint32_t>(packet->opcode)) +
                 ", " + std::to_string(packet->payload.size()) + " dwords)";
        });
    return;
  }
  if (!std::holds_alternative<ir::RegisterWrite>(command) &&
      !std::holds_alternative<ir::DrawPacket>(command) &&
      !std::holds_alternative<ir::ShaderLoad>(command)) {
    return;
  }
  if (const auto* write = std::get_if<ir::RegisterWrite>(&command)) {
    resource_state.apply(*write);
  }
  if (const auto* draw = std::get_if<ir::DrawPacket>(&command)) {
    ++draw_count;
    ++performance.draws;
    {
      static std::atomic<int> _draw_diag_count{0};
      const int _n = _draw_diag_count.fetch_add(1) + 1;
      // First 10 in full, then a running total every 1000 draws.
      if (_n <= 10 || _n % 1000 == 0) {
        xenon::logging::append_probe_log(
            "draw_calls_diag.log",
            "%.*s draw #%d: index_count=%u primitive=%d source=%d vs_valid=%d ps_valid=%d\n",
            static_cast<int>(Api::kName.size()), Api::kName.data(), _n, draw->index_count,
            static_cast<int>(draw->primitive_type), static_cast<int>(draw->source),
            draw->vertex_shader.valid, draw->pixel_shader.valid);
      }
    }
    const auto state = resource_state.snapshot();
    const auto draw_start = std::chrono::steady_clock::now();
    const auto report_slow = [&](const char* kind) {
      const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now() - draw_start)
                          .count();
      if (ms >= 50)
        xenon::logging::append_probe_log(
            "ir_timing_diag.log",
            "%s %lld ms: primitive=%d source=%d index_count=%u vs=%016llX ps=%016llX "
            "scissor=(%d,%d)-(%d,%d) viewport scale=(%g,%g) offset=(%g,%g) pitch=%u "
            "msaa=%u color0 base=%u format=%u (enabled %d mask=0x%X blend=%d) "
            "depth base=%u (test %d write %d) cull=(%d,%d) alpha=(%d,%d) "
            "window offset=(%d,%d) drawable_bottom=%d\n",
            kind, static_cast<long long>(ms), static_cast<int>(draw->primitive_type),
            static_cast<int>(draw->source), draw->index_count,
            static_cast<unsigned long long>(draw->vertex_shader.hash),
            static_cast<unsigned long long>(draw->pixel_shader.hash), state.raster.scissor_left,
            state.raster.scissor_top, state.raster.scissor_right, state.raster.scissor_bottom,
            state.raster.viewport.x_scale, state.raster.viewport.y_scale,
            state.raster.viewport.x_offset, state.raster.viewport.y_offset,
            unsigned(state.raster.surface_pitch), unsigned(state.raster.msaa_samples_log2),
            unsigned(state.color_targets[0].base_tile), unsigned(state.color_targets[0].format),
            int(state.color_targets[0].enabled),
            unsigned(state.color_targets[0].write_mask), int(state.color_targets[0].blend.enabled),
            unsigned(state.depth_target.base_tile), int(state.depth_target.test_enabled),
            int(state.depth_target.write_enabled), int(state.raster.cull_front),
            int(state.raster.cull_back), int(state.pixel_control.alpha_test_enabled),
            int(state.pixel_control.alpha_to_mask_enabled), state.raster.window_offset_x,
            state.raster.window_offset_y, state.raster.drawable_bottom);
    };
    // A failure sets `error`; count it, and keep an earlier error when
    // this command succeeded.
    auto previous_error = std::move(error);
    error.clear();
    const bool resolve = state.edram_mode == EdramMode::Copy;
    if (resolve) {
      consume_resolve(state);
      report_slow("resolve");
    } else {
      consume_draw(draw, state);
      report_slow("draw");
    }
    if (error.empty()) {
      if (resolve) ++performance.resolve_operations;
      error = std::move(previous_error);
    } else {
      ++(resolve ? performance.failed_resolves : performance.failed_draws);
    }
    if (resolve) return;
  }
  if (const auto* load = std::get_if<ir::ShaderLoad>(&command)) {
    consume_shader_load(load);
  }
}

template <typename Derived, typename Api>
void BackendCore<Derived, Api>::consume_resolve(
    const DrawResourceState& state) {
  if (!memory) {
    error = named("Xenos resolve has no bound guest memory");
    return;
  }
  const auto& resolve_vertices = state.vertex_buffers[0][0];
  if (!resolve_vertices || !resolve_vertices->valid ||
      resolve_vertices->size_dwords != 6u) {
    error = named("Xenos resolve has no valid rectangle vertices");
    return;
  }
  // Resolve rectangles are read by the CPU-side common planner from the
  // conventional six-dword vertex stream. If that stream was produced by
  // an earlier memexport, download only those bytes before decoding it.
  if (!mirror.make_cpu_visible(
          resolve_vertices->physical_address, 6u * sizeof(std::uint32_t),
          memory::GpuRangeUsage::CommandData)) {
    error = mirror.error();
    return;
  }
  std::array<std::byte, 6u * sizeof(std::uint32_t)>
      resolve_vertex_snapshot{};
  const auto resolve_vertex_base =
      state.vertex_buffers[0][0]->physical_address;
  if (!memory->copy_physical_range(resolve_vertex_base,
                                   resolve_vertex_snapshot)) {
    error = named("resolve vertex snapshot is outside physical memory");
    return;
  }
  const auto resolve_plan =
      plan_resolve(state, resolve_vertex_snapshot, resolve_vertex_base);
  if (!resolve_plan.valid) {
    error = named(resolve_plan.error);
    return;
  }
  const auto& rectangle = resolve_plan.rectangle;
  if (rectangle.empty()) return;
  if (state.copy.command != CopyCommand::Raw &&
      state.copy.command != CopyCommand::Convert) {
    error = named("Xenos resolve command is unsupported");
    ++unsupported.unhandled_resolve_modes;
    return;
  }
  const auto samples = resolve_plan.samples;
  if (resolve_plan.depth) {
    const auto format = static_cast<DepthRenderTargetFormat>(
        state.depth_target.format);
    DepthTargetImage* source = nullptr;
    std::uint64_t source_key{};
    for (auto& [key, candidate] : depth_targets) {
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
    // As for color: with no native image of this surface, the resolve
    // reads canonical EDRAM once the owners of its tiles have returned them.
    const EdramSurfaceLayout canonical_surface{
        state.depth_target.base_tile, state.raster.surface_pitch,
        static_cast<std::uint32_t>(rectangle.bottom), samples, false, true};
    if (source) {
      if (!acquire_depth_ownership(source_key, *source, source->surface())) return;
    } else if (!make_region_canonical(canonical_surface, {rectangle.left, rectangle.top,
                                                          rectangle.right, rectangle.bottom})) {
      return;
    }
    if (resolve_plan.selected_sample_count != 1) {
      error = named("Xenos depth resolve selected multiple samples");
      return;
    }
    std::uint32_t guest_sample{};
    while (guest_sample < 4 &&
           !(resolve_plan.guest_sample_mask & (1u << guest_sample))) {
      ++guest_sample;
    }
    if (guest_sample >= 4) {
      error = named("Xenos depth resolve selected no sample");
      return;
    }
    std::vector<std::uint32_t> readback;
    std::uint32_t row_pitch{};
    if (source) {
      if (!source->readback_sample(
              queue, guest_sample, rectangle.left, rectangle.top,
              rectangle.right, rectangle.bottom, readback, row_pitch)) {
        error = source->error();
        return;
      }
    } else {
      const auto region_width = static_cast<std::uint32_t>(rectangle.right - rectangle.left);
      const auto region_height = static_cast<std::uint32_t>(rectangle.bottom - rectangle.top);
      readback.assign(std::size_t(region_width) * region_height, 0u);
      row_pitch = region_width * 4u;
      if (!resolve_edram_raw(*edram, canonical_surface,
                             static_cast<std::uint32_t>(rectangle.left),
                             static_cast<std::uint32_t>(rectangle.top), region_width,
                             region_height, guest_sample,
                             std::as_writable_bytes(std::span(readback)), row_pitch)) {
        error = named("Xenos depth resolve could not read canonical EDRAM");
        return;
      }
    }
    const auto written = write_depth_resolve(
        resolve_plan.copy, format, rectangle, readback, row_pitch,
        *memory);
    if (!written.valid) {
      error = written.error;
      return;
    }
    if (!clear_depth_resolve_region(state, rectangle, samples))
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
    error = named("Xenos raw resolve source and destination formats differ");
    return;
  }
  RenderTargetImage* source = nullptr;
  std::uint64_t source_key{};
  for (auto& [key, candidate] : render_targets) {
    const auto& surface = candidate->surface();
    if (surface.base_tile != target.base_tile ||
        surface.pitch_pixels != state.raster.surface_pitch ||
        surface.msaa != samples || surface.depth ||
        surface.is_64bpp != color_render_target_is_64bpp(format) ||
        candidate->format() != Api::native_color_format(format) ||
        candidate->height() < static_cast<std::uint32_t>(rectangle.bottom)) {
      continue;
    }
    if (!source || candidate->height() < source->height()) {
      source = candidate.get();
      source_key = key;
    }
  }
  // On Xenos a resolve reads EDRAM itself, whatever last wrote it: a draw
  // through another view of the same tiles, a clear, or nothing at all.
  const EdramSurfaceLayout canonical_surface{
      target.base_tile, state.raster.surface_pitch,
      static_cast<std::uint32_t>(rectangle.bottom), samples,
      color_render_target_is_64bpp(format), false};
  const EdramSurfaceRegion resolve_region{rectangle.left, rectangle.top,
                                          rectangle.right, rectangle.bottom};
  if (source && !acquire_color_ownership(source_key, *source, source->surface())) return;
  // When one native image owns every tile, the resolve runs on the GPU out of
  // that image, through the EDRAM mapping when it is another view of the
  // tiles (AC6 resolves its 640x360 4x MSAA scene as a 1280x720 surface).
  // Nothing is flushed to canonical EDRAM or read back to convert.
  if (auto* owner = source ? source : single_color_owner(canonical_surface, resolve_region)) {
    const auto gpu = plan_gpu_color_resolve(resolve_plan, format, canonical_surface,
                                            owner->surface(), owner->host_msaa(),
                                            owner->guest_format());
    GpuClearConstants clear{};
    if (gpu.valid && (!state.copy.color_clear_enabled ||
                      plan_gpu_color_clear(resolve_plan, format, canonical_surface,
                                           owner->surface(), owner->host_msaa(), clear))) {
      // Bytes of the touched pages outside the rectangle keep their CPU
      // values; the pass writes the rectangle's pixels over them.
      if (!mirror.synchronize_range(gpu.range_address, gpu.range_size,
                                    memory::GpuRangeUsage::RenderReadback)) {
        error = mirror.error();
        return;
      }
      if (!self().gpu_color_resolve(*owner, gpu.constants)) return;
      mirror.mark_gpu_write(gpu.range_address, gpu.range_size);
      if (state.copy.color_clear_enabled && !self().gpu_color_clear(*owner, clear)) return;
      // Guest code may read what it resolved, and it reads CPU memory.
      if (!mirror.make_cpu_visible(gpu.range_address, gpu.range_size,
                                   memory::GpuRangeUsage::RenderReadback)) {
        error = mirror.error();
        return;
      }
      ++performance.gpu_resolves;
      // A depth clear sets its error itself; the color resolve is done.
      (void)clear_depth_resolve_region(state, rectangle, samples);
      return;
    }
  }
  // Otherwise whichever images own the rectangle's tiles return them to
  // canonical EDRAM and the resolve reads them there. Failing the resolve
  // instead left AC6's front buffer unwritten, and the screen black.
  if (!source && !make_region_canonical(canonical_surface, resolve_region)) return;
  // One guest sample of the rectangle, in the native host color layout a
  // render target readback has.
  const auto read_sample = [&](std::uint32_t sample, std::vector<std::byte>& pixels,
                               std::uint32_t& pitch) {
    if (source) {
      if (source->readback_sample(queue, sample, rectangle.left, rectangle.top,
                                  rectangle.right, rectangle.bottom, pixels, pitch))
        return true;
      error = source->error();
      return false;
    }
    const auto region_width = static_cast<std::uint32_t>(rectangle.right - rectangle.left);
    const auto region_height = static_cast<std::uint32_t>(rectangle.bottom - rectangle.top);
    const auto canonical_pitch = region_width * (canonical_surface.is_64bpp ? 8u : 4u);
    std::vector<std::byte> canonical(std::size_t(canonical_pitch) * region_height);
    pitch = region_width * color_host_bytes_per_pixel(format);
    pixels.assign(std::size_t(pitch) * region_height, std::byte{});
    if (resolve_edram_raw(*edram, canonical_surface, static_cast<std::uint32_t>(rectangle.left),
                          static_cast<std::uint32_t>(rectangle.top), region_width,
                          region_height, sample, canonical, canonical_pitch) &&
        edram_color_to_host(format, region_width, region_height, canonical, canonical_pitch,
                            pixels, pitch))
      return true;
    error = named("Xenos resolve could not read canonical EDRAM");
    return false;
  };
  // A native average needs a native image; from canonical EDRAM every
  // sample is read and averaged explicitly.
  auto guest_sample_mask = resolve_plan.guest_sample_mask;
  if (resolve_plan.native_color_average && !source)
    guest_sample_mask = (1u << (1u << static_cast<unsigned>(samples))) - 1u;
  std::vector<std::byte> readback;
  std::uint32_t row_pitch{};
  if (resolve_plan.native_color_average && source) {
    if (!source->readback(queue, rectangle.left, rectangle.top,
                          rectangle.right, rectangle.bottom, readback,
                          row_pitch)) {
      error = source->error();
      return;
    }
  } else {
    std::array<std::uint32_t, 4> selected_samples{};
    std::uint32_t selected_count{};
    for (std::uint32_t guest_sample = 0; guest_sample < 4; ++guest_sample) {
      if (guest_sample_mask & (1u << guest_sample)) {
        selected_samples[selected_count] = guest_sample;
        ++selected_count;
      }
    }
    if (selected_count == 1) {
      if (!read_sample(selected_samples[0], readback, row_pitch)) return;
    } else if (selected_count == 2 || selected_count == 4) {
      std::vector<std::byte> first;
      std::vector<std::byte> second;
      std::uint32_t first_pitch{};
      std::uint32_t second_pitch{};
      if (!read_sample(selected_samples[0], first, first_pitch) ||
          !read_sample(selected_samples[1], second, second_pitch)) return;
      const auto width = static_cast<std::uint32_t>(rectangle.right -
                                                    rectangle.left);
      const auto height = static_cast<std::uint32_t>(rectangle.bottom -
                                                     rectangle.top);
      if (!average_host_color_samples(format, width, height, first,
                                      first_pitch, second, second_pitch,
                                      readback, row_pitch)) {
        error = named("Xenos selected-sample resolve averaging failed");
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
        if (!read_sample(selected_samples[2], third, third_pitch) ||
            !read_sample(selected_samples[3], fourth, fourth_pitch) ||
            !average_host_color_samples(
                format, width, height, third, third_pitch, fourth,
                fourth_pitch, second_average, second_average_pitch) ||
            !average_host_color_samples(
                format, width, height, readback, row_pitch,
                second_average, second_average_pitch, all_average,
                all_average_pitch)) {
          error =
              named("Xenos four-sample resolve averaging failed");
          return;
        }
        readback = std::move(all_average);
        row_pitch = all_average_pitch;
      }
    } else {
      error = named("Xenos resolve selected an unsupported sample set");
      ++unsupported.unhandled_resolve_modes;
      return;
    }
  }
  const auto written = state.copy.command == CopyCommand::Convert ||
                               color_host_requires_conversion(format)
      ? write_converted_resolve(state.copy, format, rectangle, readback,
                                row_pitch, *memory)
      : write_raw_resolve(state.copy, rectangle, readback, row_pitch,
                          *memory);
  if (!written.valid) {
    error = written.error;
    return;
  }
  // Keep resolve correctness failures separate from presentation failures.
  // When probe logging is explicitly enabled, sample the first few resolves
  // on both sides of the native-readback -> guest-memory boundary. This is
  // intentionally bounded: AC6 resolves every frame, and an unbounded byte
  // audit would distort the performance issue it is meant to diagnose.
  static std::atomic_uint32_t resolve_content_probe_count{};
  const auto resolve_content_probe =
      resolve_content_probe_count.fetch_add(1u, std::memory_order_relaxed);
  if (resolve_content_probe < 64u) {
    std::size_t source_nonzero{};
    for (const auto byte : readback) {
      if (byte != std::byte{}) ++source_nonzero;
    }
    std::vector<std::byte> destination_snapshot(written.modified_size);
    const auto destination_read =
        written.modified_size == 0u ||
        memory->copy_physical_range(written.modified_address,
                                    destination_snapshot);
    std::size_t destination_nonzero{};
    if (destination_read) {
      for (const auto byte : destination_snapshot) {
        if (byte != std::byte{}) ++destination_nonzero;
      }
    }
    xenon::logging::append_probe_log(
        "resolve_content_diag.log",
        "resolve #%u source_nonzero=%zu/%zu destination=0x%08X+0x%X "
        "destination_read=%u destination_nonzero=%zu/%zu format=%u "
        "copy_command=%u samples=%u rectangle=(%d,%d)-(%d,%d)\n",
        resolve_content_probe + 1u, source_nonzero, readback.size(),
        written.modified_address, written.modified_size,
        destination_read ? 1u : 0u, destination_nonzero,
        destination_snapshot.size(), static_cast<unsigned>(format),
        static_cast<unsigned>(state.copy.command),
        static_cast<unsigned>(samples), rectangle.left, rectangle.top,
        rectangle.right, rectangle.bottom);
  }
  if (state.copy.color_clear_enabled && !source) {
    // The rectangle is already canonical: clear it there.
    if (!clear_edram_surface_region(*edram, canonical_surface, rectangle.left, rectangle.top,
                                    rectangle.right, rectangle.bottom,
                                    state.copy.color_clear)) {
      error = named("regional color resolve clear failed");
      return;
    }
    edram_ownership.make_canonical_region(canonical_surface, resolve_region);
  } else if (state.copy.color_clear_enabled) {
    const auto owner = render_target_owners.find(source_key);
    // Only the source's tiles under the cleared rectangle go back to
    // canonical storage to be cleared; the source keeps the rest.
    std::vector<std::uint16_t> cleared_tiles;
    if (owner != render_target_owners.end()) {
      for (const auto tile : covered_edram_tiles(
               source->surface(),
               {rectangle.left, rectangle.top, rectangle.right, rectangle.bottom}))
        if (edram_ownership.owner(tile) == owner->second) cleared_tiles.push_back(tile);
    }
    if (owner == render_target_owners.end() ||
        !flush_owner_tiles(owner->second, cleared_tiles) ||
        !clear_edram_surface_region(
            *edram, source->surface(), rectangle.left,
            rectangle.top, rectangle.right, rectangle.bottom,
            state.copy.color_clear)) {
      if (error.empty())
        error = named("regional color resolve clear failed");
      return;
    }
    edram_ownership.make_canonical_region(
        source->surface(), {rectangle.left, rectangle.top,
                            rectangle.right, rectangle.bottom});
  }
  if (!clear_depth_resolve_region(state, rectangle, samples)) return;
  return;
}

template <typename Derived, typename Api>
void BackendCore<Derived, Api>::consume_shader_load(
    const ir::ShaderLoad* load) {
  shader_textures[load->program.hash()] =
      load->decoded.reflection.texture_fetch_constants;
#ifdef XENON_HAS_DXC
  decoded_shaders[load->program.hash()] = load->decoded;
  const auto lowered = HlslShaderLowerer::lower(load->decoded);
  unsupported.unsupported_shader_instructions += lowered.unsupported_instructions;
  unsupported.unsupported_shader_features += lowered.unsupported_features;
  unsupported.unsupported_fetch_formats += lowered.unsupported_fetch_formats;
  if (!lowered.complete) {
    // Do not even attempt to compile an incomplete lowering - it would
    // only fail again with a generic "compilation failed" that discards
    // the specific, already-known reason lower() recorded.
    ++shader_translation_failures;
    error = lowered.diagnostics.empty() ? std::string(Api::kShaderBinaryName) + " shader lowering failed"
                                               : lowered.diagnostics.front();
    xenon::logging::Logger::instance().log_if_enabled(
        xenon::logging::Level::Warning, "gpu", [&] {
          return "shader lowering failed (hash=" + std::to_string(load->program.hash()) +
                 "): " + error;
        });
  } else {
    ShaderCompileOptions options{};
    options.format = Api::kShaderBinaryFormat;
    const auto compiled = shader_cache.get_or_compile(lowered, options);
    if (compiled->succeeded) {
      ++compiled_shader_count;
      shaders[load->program.hash()] = compiled;
    } else {
      error = compiled->diagnostics.empty() ? std::string(Api::kShaderBinaryName) + " shader compilation failed"
                                                   : compiled->diagnostics.front();
    }
  }
#endif
}

}  // namespace xenon::gpu::detail
