#pragma once

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

#include "xenon/gpu/backend.hpp"
#include "xenon/gpu/capture.hpp"
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
  [[nodiscard]] FrontendSubmissionCapture capture_buffer(
      std::uint32_t physical_address, std::uint32_t dword_count);
  [[nodiscard]] FrontendSubmissionCapture capture_ring(
      std::uint32_t physical_address, std::uint32_t capacity_dwords,
      std::uint32_t read_index, std::uint32_t write_index);
  // Creates a standalone capture by canonicalizing backend-authored memory,
  // snapshotting every guest range reachable by the normalized submission and
  // retaining the complete canonical EDRAM store.
  [[nodiscard]] bool capture_portable_buffer(
      Backend& backend, std::uint32_t physical_address, std::uint32_t dword_count,
      PortableSubmissionCapture& out, std::string* error = nullptr);
  [[nodiscard]] bool capture_portable_ring(
      Backend& backend, std::uint32_t physical_address,
      std::uint32_t capacity_dwords, std::uint32_t read_index,
      std::uint32_t write_index, PortableSubmissionCapture& out,
      std::string* error = nullptr);
  // Restores captured guest RAM/EDRAM through Memory V2 and replays normalized
  // work into the backend without depending on any prior backend cache state.
  [[nodiscard]] bool replay_portable_capture(
      Backend& backend, const PortableSubmissionCapture& capture,
      std::string* error = nullptr);
  // Replays captured normalized frontend work against the current guest RAM
  // and EDRAM. A complete register preamble is emitted first so replay is
  // deterministic even when the backend retained state from prior work.
  void replay_capture(Backend& backend,
                      const FrontendSubmissionCapture& capture);
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
  void set_interrupt_callback(
      std::function<void(std::uint32_t cpu_index)> callback) {
    command_processor_.set_interrupt_callback(std::move(callback));
  }

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
