#pragma once

// Submission bracketing, guest-memory visibility, presentation and counters
// for BackendCore. Included by backend_core.hpp; see that header for the
// backend hooks.

#include "graphics/common/backend_core.hpp"

namespace xenon::gpu::detail {

template <typename Derived, typename Api>
void BackendCore<Derived, Api>::begin_submission(
    memory::AddressSpace& new_memory, Edram& new_edram) {
  submission_started = std::chrono::steady_clock::now();
  ++performance.submissions;
  command_count = 0;
  draw_count = 0;
  compiled_shader_count = 0;
  if (!ready) return;
  if ((edram && edram != &new_edram) ||
      (memory && memory != &new_memory)) {
    if (!queue.wait_idle()) {
      error = queue.error();
      ready = false;
      return;
    }
  }
  if (edram != &new_edram) {
    render_targets.clear();
    self().forget_backend_render_targets();
    depth_targets.clear();
    render_target_owners.clear();
    depth_target_owners.clear();
    owner_render_targets.clear();
    owner_depth_targets.clear();
    edram_ownership.reset();
    next_edram_owner = 1;
    edram = &new_edram;
  }
  if (memory != &new_memory) {
    textures.clear();
    texture_dirty.clear();
    if (!self().initialize_mirror(new_memory)) {
      error = mirror.error();
      ready = false;
      return;
    }
    memory = &new_memory;
    if (!self().bind_guest_memory()) {
      error = resources.error();
      ready = false;
      return;
    }
  }
}

template <typename Derived, typename Api>
void BackendCore<Derived, Api>::end_submission() {
  performance.submission_time_ns += static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - submission_started).count());
  if (!ready) return;
  if (!queue.flush()) {
    error = queue.error();
    ready = false;
  }
}

template <typename Derived, typename Api>
bool BackendCore<Derived, Api>::make_guest_memory_cpu_visible(
    std::uint32_t physical_address, std::uint32_t size) {
  if (!ready || !memory) return false;
  if (!mirror.make_cpu_visible(physical_address, size)) {
    error = mirror.error();
    return false;
  }
  return true;
}

template <typename Derived, typename Api>
bool BackendCore<Derived, Api>::invalidate_edram_native_state() {
  if (!ready || !edram) return false;
  if (!queue.wait_idle()) {
    error = queue.error();
    ready = false;
    return false;
  }
  // A portable capture has replaced the canonical byte store externally.
  // Forget only ownership: cached native images may be retained, but no tile
  // may remain authoritative until it is explicitly reacquired from canonical
  // EDRAM on the next draw/resolve.
  edram_ownership.reset();
  return true;
}

template <typename Derived, typename Api>
PresentStatus BackendCore<Derived, Api>::present(
    const PresentationFrame& frame) {
  if (!presentation.ready()) return PresentStatus::NotConfigured;
  if (!ready || !memory) return PresentStatus::Error;
  TextureDescriptor descriptor = frame.texture;
  descriptor.mip_min_level = 0;
  descriptor.mip_max_level = 0;
  descriptor.packed_mips = false;
  const auto layout = build_texture_layout(descriptor);
  if (!layout.valid) {
    error = layout.error;
    return PresentStatus::Error;
  }
  for (const auto& subresource : layout.subresources) {
    if (subresource.guest_size_bytes > UINT32_MAX ||
        !mirror.make_cpu_visible(
            subresource.guest_address,
            static_cast<std::uint32_t>(subresource.guest_size_bytes),
            memory::GpuRangeUsage::RenderReadback)) {
      error = mirror.error().empty()
                         ? named("scanout source range is invalid")
                         : mirror.error();
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
    error = named("presentation snapshot range is invalid");
    return PresentStatus::Error;
  }
  std::vector<std::byte> presentation_snapshot(
      static_cast<std::size_t>(presentation_snapshot_end -
                               presentation_snapshot_base));
  if (!memory->copy_physical_range(presentation_snapshot_base,
                                           presentation_snapshot)) {
    error = named("presentation snapshot failed");
    return PresentStatus::Error;
  }
  const auto prepared = prepare_presentation_frame(
      frame, presentation_snapshot, presentation.width(),
      presentation.height(),
      presentation.config().preserve_aspect_ratio,
      presentation_snapshot_base);
  if (!prepared.valid) {
    error = prepared.error;
    return PresentStatus::Unsupported;
  }
  const auto status = presentation.present(prepared);
  if (status == PresentStatus::Error || status == PresentStatus::SurfaceLost)
    error = presentation.error();
  return status;
}

template <typename Derived, typename Api>
bool BackendCore<Derived, Api>::resize_presentation(std::uint32_t width,
                                                     std::uint32_t height) {
  if (!presentation.resize(width, height)) {
    error = presentation.error();
    return false;
  }
  return true;
}

template <typename Derived, typename Api>
GpuPerformanceCounters BackendCore<Derived, Api>::performance_counters()
    const noexcept {
  auto result = performance;
  result.shader_cache_misses = compiled_shader_count;
  result.pipeline_cache_misses = self().pipeline_cache_misses();
  return result;
}

template <typename Derived, typename Api>
GpuUnsupportedCounters BackendCore<Derived, Api>::unsupported_counters()
    const noexcept {
  auto result = unsupported;
  self().add_backend_unsupported_counters(result);
  return result;
}

template <typename Derived, typename Api>
GpuShaderCoverage BackendCore<Derived, Api>::shader_coverage()
    const noexcept {
  GpuShaderCoverage coverage{};
  coverage.shaders_discovered = decoded_shaders.size();
  coverage.translation_failures = shader_translation_failures;
  coverage.shaders_translated =
      coverage.shaders_discovered >= coverage.translation_failures
          ? coverage.shaders_discovered - coverage.translation_failures
          : 0u;
  coverage.cache_hits = shader_cache.hits();
  coverage.cache_misses = shader_cache.misses();
  return coverage;
}

}  // namespace xenon::gpu::detail
