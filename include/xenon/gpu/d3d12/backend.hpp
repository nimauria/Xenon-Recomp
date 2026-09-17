#pragma once

#include <cstddef>
#include <memory>
#include <string>

#include "xenon/gpu/backend.hpp"
#include "xenon/gpu/d3d12/context.hpp"

namespace xenon::gpu::d3d12 {

class Backend final : public xenon::gpu::Backend {
 public:
  Backend();
  ~Backend() override;
  Backend(const Backend&) = delete;
  Backend& operator=(const Backend&) = delete;

  [[nodiscard]] bool initialize(const ContextConfig& config = {});
  void begin_submission(memory::AddressSpace& memory, Edram& edram) override;
  void consume(const ir::Command& command) override;
  void end_submission() override;

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

}  // namespace xenon::gpu::d3d12
