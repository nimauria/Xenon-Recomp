#pragma once

#include <cstdint>

#include "xenon/gpu/backend.hpp"
#include "xenon/gpu/command_processor.hpp"
#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

class GraphicsSystem {
 public:
  explicit GraphicsSystem(memory::AddressSpace& memory);

  void reset();
  void submit_buffer(std::uint32_t physical_address, std::uint32_t dword_count);
  [[nodiscard]] std::uint32_t submit_ring(std::uint32_t physical_address,
                                          std::uint32_t capacity_dwords,
                                          std::uint32_t read_index,
                                          std::uint32_t write_index);
  // Executes each pending graphics-IR command exactly once, in order, then
  // clears the pending stream. Backends retain their own shadow state across
  // submissions via RegisterWrite commands.
  void execute_ir(Backend& backend);
  // Explicit GPU -> CPU visibility hook for Memory v2 / CPU consumers.
  [[nodiscard]] bool make_guest_memory_cpu_visible(
      Backend& backend, std::uint32_t physical_address, std::uint32_t size);
  // Explicit native-owner -> canonical EDRAM checkpoint for backend switching,
  // capture/save-state and validation.
  [[nodiscard]] bool make_edram_canonical(Backend& backend);
  [[nodiscard]] PresentStatus present(Backend& backend,
                                      const PresentationFrame& frame);

  [[nodiscard]] RegisterFile& registers() noexcept { return registers_; }
  [[nodiscard]] const RegisterFile& registers() const noexcept { return registers_; }
  [[nodiscard]] ir::Stream& stream() noexcept { return stream_; }
  [[nodiscard]] const ir::Stream& stream() const noexcept { return stream_; }
  [[nodiscard]] Edram& edram() noexcept { return edram_; }
  [[nodiscard]] const Edram& edram() const noexcept { return edram_; }
  [[nodiscard]] CommandProcessor& command_processor() noexcept { return command_processor_; }

 private:
  memory::AddressSpace& memory_;
  RegisterFile registers_{};
  ir::Stream stream_{};
  Edram edram_{};
  CommandProcessor command_processor_;
};

}  // namespace xenon::gpu
