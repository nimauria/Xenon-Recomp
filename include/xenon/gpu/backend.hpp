#pragma once

#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

// Host graphics backends consume Xenon graphics IR, never PM4 directly. Vulkan
// and D3D12 implementations therefore remain replaceable without changing the
// Xenos frontend or Project Gracemeria.
class Backend {
 public:
  virtual ~Backend() = default;
  virtual void begin_submission(memory::AddressSpace& memory,
                                Edram& edram) = 0;
  virtual void consume(const ir::Command& command) = 0;
  virtual void end_submission() = 0;
  // Makes GPU-authored guest physical memory visible to the CPU on demand.
  // Normal rendering and memexport remain GPU-resident until a CPU consumer
  // explicitly requests the range.
  [[nodiscard]] virtual bool make_guest_memory_cpu_visible(
      std::uint32_t physical_address, std::uint32_t size) = 0;
  // Makes the canonical 10 MiB Xenos EDRAM byte store authoritative by
  // synchronously flushing every native color/depth owner that still contains
  // newer bits. Used for backend migration, captures and save-state/debug
  // checkpoints; normal rendering should keep EDRAM native/GPU-resident.
  [[nodiscard]] virtual bool make_edram_canonical() = 0;
  // Called after a portable capture restores the canonical EDRAM byte store.
  // Native render-target/depth ownership must be discarded so the next use is
  // rehydrated from the restored canonical bytes rather than stale host images.
  [[nodiscard]] virtual bool invalidate_edram_native_state() = 0;
  // Presents an explicit runtime-provided scanout resource. Native window /
  // surface attachment is backend-specific, while frame semantics stay common.
  [[nodiscard]] virtual PresentStatus present(const PresentationFrame& frame) = 0;
  [[nodiscard]] virtual bool resize_presentation(std::uint32_t width,
                                                 std::uint32_t height) = 0;
  [[nodiscard]] virtual bool presentation_ready() const noexcept = 0;
};

class NullBackend final : public Backend {
 public:
  void begin_submission(memory::AddressSpace&, Edram&) override {
    command_count_ = 0;
  }
  void consume(const ir::Command&) override { ++command_count_; }
  void end_submission() override {}
  [[nodiscard]] bool make_guest_memory_cpu_visible(
      std::uint32_t, std::uint32_t) override {
    return true;
  }
  [[nodiscard]] bool make_edram_canonical() override { return true; }
  [[nodiscard]] bool invalidate_edram_native_state() override { return true; }
  [[nodiscard]] PresentStatus present(const PresentationFrame&) override {
    return PresentStatus::NotConfigured;
  }
  [[nodiscard]] bool resize_presentation(std::uint32_t, std::uint32_t) override {
    return false;
  }
  [[nodiscard]] bool presentation_ready() const noexcept override { return false; }
  [[nodiscard]] std::size_t command_count() const noexcept { return command_count_; }

 private:
  std::size_t command_count_{};
};

}  // namespace xenon::gpu
