#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "xenon/gpu/backend.hpp"
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
  [[nodiscard]] bool make_guest_memory_cpu_visible(
      std::uint32_t physical_address, std::uint32_t size) override;
  [[nodiscard]] bool make_edram_canonical() override;
  [[nodiscard]] bool invalidate_edram_native_state() override;
  [[nodiscard]] PresentStatus present(const PresentationFrame& frame) override;
  [[nodiscard]] bool resize_presentation(std::uint32_t width,
                                         std::uint32_t height) override;
  [[nodiscard]] bool presentation_ready() const noexcept override;
  [[nodiscard]] GpuPerformanceCounters performance_counters() const noexcept override;
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] std::size_t command_count() const noexcept;
  [[nodiscard]] std::size_t draw_count() const noexcept;
  [[nodiscard]] std::size_t compiled_shader_count() const noexcept;
  [[nodiscard]] std::size_t pipeline_state_count() const noexcept;
  [[nodiscard]] bool resource_layout_ready() const noexcept;
  [[nodiscard]] std::size_t realized_texture_count() const noexcept;
  [[nodiscard]] std::size_t realized_render_target_count() const noexcept;
  [[nodiscard]] const DeviceProperties& device_properties() const noexcept;
  [[nodiscard]] const std::string& error() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace xenon::gpu::vulkan
