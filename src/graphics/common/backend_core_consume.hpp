#pragma once

// Command dispatch, Xenos resolves and shader loads for BackendCore. Included
// by backend_core.hpp; see that header for the backend hooks.

#include "graphics/common/backend_core.hpp"

namespace xenon::gpu::detail {

template <typename Derived, typename Api>
void BackendCore<Derived, Api>::consume(const ir::Command& command) {
  collect_retired_resources();
  ++command_count;
  ++performance.commands;
  // Part 7 of the AC6 Runtime Readiness pass: RegisterWrite/DrawPacket/
  // ShaderLoad are the only Command variants this backend actually consumes
  // below. PhysicalMemoryWrite/IndirectBuffer/ShaderPacket/
  // SynchronizationPacket/EventPacket/StatePacket/Type3Packet reaching here
  // have no handler at all - previously silently dropped. Type3Packet's own
  // doc comment (ir.hpp) is explicit that "packets are never silently
  // discarded merely because the host backend does not consume them yet";
  // this is the observability half of that promise; consuming them is
  // separate future work.
  if (!std::holds_alternative<ir::RegisterWrite>(command) &&
      !std::holds_alternative<ir::DrawPacket>(command) &&
      !std::holds_alternative<ir::ShaderLoad>(command)) {
    ++unsupported.unknown_packets;
    xenon::logging::Logger::instance().log_if_enabled(
        xenon::logging::Level::Warning, "gpu", [&] {
          return "unhandled GPU command variant (index=" +
                 std::to_string(command.index()) + ")";
        });
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
        if (FILE* _d = std::fopen("draw_calls_diag.log", "a")) {
          std::fprintf(_d, "%.*s draw #%d: index_count=%u primitive=%d source=%d vs_valid=%d ps_valid=%d\n",
                       static_cast<int>(Api::kName.size()), Api::kName.data(), _n, draw->index_count, static_cast<int>(draw->primitive_type),
                       static_cast<int>(draw->source), draw->vertex_shader.valid, draw->pixel_shader.valid);
          std::fclose(_d);
        }
      }
    }
    const auto state = resource_state.snapshot();
    if (state.edram_mode == EdramMode::Copy) {
      consume_resolve(state);
      return;
    }
    consume_draw(draw, state);
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
    if (!source) {
      error =
          named("Xenos depth resolve source EDRAM surface is unavailable");
      return;
    }
    if (!acquire_depth_ownership(source_key, *source,
                                 source->surface())) return;
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
    if (!source->readback_sample(
            queue, guest_sample, rectangle.left, rectangle.top,
            rectangle.right, rectangle.bottom, readback, row_pitch)) {
      error = source->error();
      return;
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
  if (!source) {
    error = named("Xenos resolve source EDRAM surface is unavailable");
    return;
  }
  if (!acquire_color_ownership(source_key, *source,
                               source->surface())) return;
  std::vector<std::byte> readback;
  std::uint32_t row_pitch{};
  if (resolve_plan.native_color_average) {
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
      if (resolve_plan.guest_sample_mask & (1u << guest_sample)) {
        selected_samples[selected_count] = guest_sample;
        ++selected_count;
      }
    }
    if (selected_count == 1) {
      if (!source->readback_sample(
              queue, selected_samples[0], rectangle.left,
              rectangle.top, rectangle.right, rectangle.bottom, readback,
              row_pitch)) {
        error = source->error();
        return;
      }
    } else if (selected_count == 2 || selected_count == 4) {
      std::vector<std::byte> first;
      std::vector<std::byte> second;
      std::uint32_t first_pitch{};
      std::uint32_t second_pitch{};
      if (!source->readback_sample(
              queue, selected_samples[0], rectangle.left,
              rectangle.top, rectangle.right, rectangle.bottom, first,
              first_pitch) ||
          !source->readback_sample(
              queue, selected_samples[1], rectangle.left,
              rectangle.top, rectangle.right, rectangle.bottom, second,
              second_pitch)) {
        error = source->error();
        return;
      }
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
        if (!source->readback_sample(
                queue, selected_samples[2], rectangle.left,
                rectangle.top, rectangle.right, rectangle.bottom, third,
                third_pitch) ||
            !source->readback_sample(
                queue, selected_samples[3], rectangle.left,
                rectangle.top, rectangle.right, rectangle.bottom, fourth,
                fourth_pitch) ||
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
  if (state.copy.color_clear_enabled) {
    const auto owner = render_target_owners.find(source_key);
    if (owner == render_target_owners.end() ||
        !flush_color_owner(owner->second) ||
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
