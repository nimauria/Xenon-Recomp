#pragma once

#include <cstdint>
#include <variant>

#include "xenon/gpu/edram.hpp"
#include "xenon/gpu/ir.hpp"
#include "xenon/gpu/presentation.hpp"
#include "xenon/gpu/register_file.hpp"
#include "xenon/memory/address_space.hpp"

namespace xenon::gpu {

struct GpuPerformanceCounters {
  std::uint64_t submissions{};
  std::uint64_t commands{};
  std::uint64_t draws{};
  std::uint64_t shader_cache_misses{};
  std::uint64_t pipeline_cache_misses{};
  std::uint64_t resolve_operations{};
  std::uint64_t edram_transfers{};
  std::uint64_t staging_transfers{};
  std::uint64_t queue_submissions{};
  std::uint64_t submission_time_ns{};
  // Part 10 of the AC6 Runtime Readiness pass ("Memory / GPU coherency
  // assertions"): a previously-cached guest texture was found dirty (a CPU
  // write landed in its guest memory range since it was last uploaded, per
  // memory::GuestMemoryCoherency's epoch) and was re-decoded/re-uploaded
  // before this draw sampled it - the real CPU-write -> GPU-sample coherency
  // path, not a first-time texture creation (which is not a re-validation
  // and is not counted here).
  std::uint64_t texture_cache_invalidations{};
};

// Part 7 of the AC6 Runtime Readiness pass ("GPU capability / silent
// fallback audit"): every unsupported GPU operation must be observable, not
// silently approximated. Each field here corresponds to one category from
// that audit. These counters must reflect reality - never suppressed,
// reclassified, or downgraded just to make a report read as "0" (see the
// reviewer's explicit warning on this point). A correct, fully-supported run
// against a title Xenon genuinely handles end to end should read all zeros;
// any nonzero value is real, actionable evidence of a gap, not noise to be
// hidden.
struct GpuUnsupportedCounters {
  std::uint64_t unknown_packets{};                 // IR command variant consume() has no handler for.
  std::uint64_t unknown_registers{};                // Xenos register index outside the known register file.
  std::uint64_t unsupported_fetch_formats{};        // Vertex/texture fetch format the shader lowerer rejects.
  std::uint64_t unsupported_texture_formats{};      // Guest texture format with no host equivalent.
  std::uint64_t unsupported_sampler_behaviors{};    // Sampler state (address mode, filter, ...) with no host mapping.
  std::uint64_t unsupported_shader_instructions{};  // Xenos ALU/CF opcode the translator does not lower.
  std::uint64_t unsupported_shader_features{};      // A recognized-but-unimplemented shader capability.
  std::uint64_t unhandled_resolve_modes{};          // EDRAM->texture resolve configuration with no host path.
  std::uint64_t unhandled_depth_stencil_paths{};    // Depth/stencil format or transfer with no host path.
  std::uint64_t unexpected_ownership_transitions{}; // EDRAM/render-target ownership handoff the planner rejected.
  std::uint64_t failed_resource_barriers{};         // A requested resource barrier/transition that could not be satisfied.
  std::uint64_t fallback_shader_uses{};             // Draws that ran through a generic fallback shader, not the title's own.

  [[nodiscard]] std::uint64_t total() const noexcept {
    return unknown_packets + unknown_registers + unsupported_fetch_formats +
           unsupported_texture_formats + unsupported_sampler_behaviors +
           unsupported_shader_instructions + unsupported_shader_features +
           unhandled_resolve_modes + unhandled_depth_stencil_paths +
           unexpected_ownership_transitions + failed_resource_barriers +
           fallback_shader_uses;
  }
};

// Part 9 of the AC6 Runtime Readiness pass ("shader coverage report").
// Shaders are discovered dynamically as a title streams ir::ShaderLoad
// commands - this is a live, cumulative report queryable at any point, not
// a static "every shader known before boot" requirement. The target for a
// fully-supported title is translationFailures == 0 and
// unsupportedShaderInstructions/unsupportedFetchFormats (GpuUnsupportedCounters)
// == 0; like those counters, these must reflect reality, never be tuned to
// read as clean.
struct GpuShaderCoverage {
  std::uint64_t shaders_discovered{};    // Distinct ir::ShaderLoad programs seen.
  std::uint64_t shaders_translated{};    // Successfully lowered to HLSL (may still fail to compile).
  std::uint64_t translation_failures{};  // HlslShaderLowerer::lower() returned incomplete.
  std::uint64_t cache_hits{};            // ShaderCache::get_or_compile() reused a prior compile.
  std::uint64_t cache_misses{};          // ShaderCache::get_or_compile() actually invoked the compiler.
};

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
  [[nodiscard]] virtual GpuPerformanceCounters performance_counters() const noexcept {
    return {};
  }
  // See GpuUnsupportedCounters's doc comment. Queryable at any point during
  // or after a run (not just at backend construction, unlike error()), so a
  // capability report can include a live, honest "GPU unsupported
  // operations: N" figure.
  [[nodiscard]] virtual GpuUnsupportedCounters unsupported_counters() const noexcept {
    return {};
  }
  // See GpuShaderCoverage's doc comment.
  [[nodiscard]] virtual GpuShaderCoverage shader_coverage() const noexcept {
    return {};
  }
};

class NullBackend final : public Backend {
 public:
  void begin_submission(memory::AddressSpace&, Edram&) override {
    command_count_ = 0;
    ++counters_.submissions;
  }
  void consume(const ir::Command& command) override {
    ++command_count_;
    ++counters_.commands;
    if (std::holds_alternative<ir::DrawPacket>(command)) ++counters_.draws;
  }
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
  [[nodiscard]] GpuPerformanceCounters performance_counters() const noexcept override {
    return counters_;
  }
  [[nodiscard]] std::size_t command_count() const noexcept { return command_count_; }

 private:
  std::size_t command_count_{};
  GpuPerformanceCounters counters_{};
};

}  // namespace xenon::gpu
