#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

#include "xenon/gpu/backend.hpp"
#include "xenon/gpu/depth_format.hpp"
#include "xenon/gpu/vulkan/context.hpp"

namespace xenon::gpu::vulkan {

class Backend final : public xenon::gpu::Backend {
 public:
  Backend();
  ~Backend() override;
  [[nodiscard]] bool initialize(const ContextConfig& config = {});
  // Runtime creates the VkSurfaceKHR from instance() using its window-system
  // integration, then transfers or lends it to the backend here.
  [[nodiscard]] VkInstance instance() const noexcept;
  [[nodiscard]] bool configure_presentation(
      VkSurfaceKHR surface, const PresentationConfig& config = {},
      bool take_surface_ownership = false);
  void begin_submission(memory::AddressSpace& memory, Edram& edram) override;
  void consume(const ir::Command& command) override;
  void end_submission() override;
  void end_submission_deferred() override;
  [[nodiscard]] bool make_guest_memory_cpu_visible(
      std::uint32_t physical_address, std::uint32_t size) override;
  [[nodiscard]] bool make_edram_canonical() override;
  [[nodiscard]] bool invalidate_edram_native_state() override;
  [[nodiscard]] PresentStatus present(const PresentationFrame& frame) override;
  // Draws scan-out of `frame` on the GPU into an offscreen width x height
  // RGBA8 image and reads it back, as presentation would show it.
  [[nodiscard]] bool capture_scanout(const PresentationFrame& frame, std::uint32_t width,
                                     std::uint32_t height, PreparedPresentationFrame& output) override;
  [[nodiscard]] bool resize_presentation(std::uint32_t width,
                                         std::uint32_t height) override;
  [[nodiscard]] bool presentation_ready() const noexcept override;
  [[nodiscard]] GpuPerformanceCounters performance_counters() const noexcept override;
  [[nodiscard]] std::vector<GpuFailureReason> failure_reasons() const override;
  [[nodiscard]] std::vector<GpuResolveKind> resolve_census() const override;
  [[nodiscard]] GpuUnsupportedCounters unsupported_counters() const noexcept override;
  [[nodiscard]] GpuShaderCoverage shader_coverage() const noexcept override;
  void sync_guest_reads() override;
  void prefetch_shaders(std::span<const ir::Command> commands) override;
  void end_guest_read_sync() override;
  [[nodiscard]] bool publish_gpu_writes(bool wait) override;
  void set_resolve_readback(ResolveReadback policy) override;
  void set_shader_cache_directory(const std::string& directory) override;
  // GPU V3 parallel recording (see d3d12::Backend::set_parallel_recording()).
  [[nodiscard]] bool set_parallel_recording(unsigned threads, std::uint32_t min_run_draws);
  // See d3d12::Backend::set_gpu_canonical_edram_flushes().
  void set_gpu_canonical_edram_flushes(bool enabled) noexcept;
  [[nodiscard]] std::uint64_t parallel_runs_recorded() const noexcept;
  // GPU timestamps (perf_frame.log "gpu"): waits for the GPU, reads every
  // batch's timestamps, and returns the batches timed and their GPU time.
  struct GpuTiming {
    std::uint64_t batches{};
    std::uint64_t gpu_ns{};
  };
  [[nodiscard]] GpuTiming gpu_timing();
  // GPU V3 shader-variant prefetch: variants the frontend predicted from the
  // IR it handed over, draws that found theirs prefetched, and draws that
  // lowered and compiled theirs themselves.
  struct ShaderPrefetchStatistics {
    std::uint64_t variants_predicted{};
    std::uint64_t variants_prefetched_at_draw{};
    std::uint64_t variants_compiled_at_draw{};
  };
  [[nodiscard]] ShaderPrefetchStatistics shader_prefetch_statistics() const noexcept;
  [[nodiscard]] bool ready() const noexcept;
  // How this device's D24 depth writes were measured to encode a code at
  // initialize(); empty before it, or in a build without shader writes.
  [[nodiscard]] std::optional<Unorm24DepthCalibration> unorm24_depth_calibration() const;
  [[nodiscard]] std::size_t command_count() const noexcept;
  [[nodiscard]] std::size_t draw_count() const noexcept;
  [[nodiscard]] std::size_t compiled_shader_count() const noexcept;
  [[nodiscard]] std::size_t pipeline_state_count() const noexcept;
  [[nodiscard]] bool resource_layout_ready() const noexcept;
  [[nodiscard]] std::size_t realized_texture_count() const noexcept;
  [[nodiscard]] std::size_t realized_render_target_count() const noexcept;
  // The rows of the tallest colour render target alive: how much EDRAM
  // the draws so far had to cover (a regression check for draws sized to
  // an open scissor).
  [[nodiscard]] std::uint32_t tallest_render_target_rows() const noexcept;
  [[nodiscard]] const DeviceProperties& device_properties() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept;
  [[nodiscard]] std::string last_error() const override { return error(); }

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xenon::gpu::vulkan
