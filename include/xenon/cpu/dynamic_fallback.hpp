#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string_view>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/runtime.hpp"

namespace xenon::cpu {

// Gen 7 safety-net execution is intentionally bounded and subordinate to AOT.
// It exists so a valid executable edge missed by static discovery can continue
// far enough to produce evidence for the next AOT pass; it is not the normal
// execution engine and never replaces a compiled entry that already exists.
enum class DynamicFallbackStopReason : std::uint8_t {
  Returned,
  CompiledHandoff,
  UnsupportedInstruction,
  InvalidTarget,
  SourceChanged,
  InstructionLimit,
  CallDepthLimit,
  Trap,
  Syscall,
};

[[nodiscard]] constexpr std::string_view dynamic_fallback_stop_reason_name(
    DynamicFallbackStopReason reason) noexcept {
  switch (reason) {
    case DynamicFallbackStopReason::Returned: return "returned";
    case DynamicFallbackStopReason::CompiledHandoff: return "compiled-handoff";
    case DynamicFallbackStopReason::UnsupportedInstruction: return "unsupported-instruction";
    case DynamicFallbackStopReason::InvalidTarget: return "invalid-target";
    case DynamicFallbackStopReason::SourceChanged: return "source-changed";
    case DynamicFallbackStopReason::InstructionLimit: return "instruction-limit";
    case DynamicFallbackStopReason::CallDepthLimit: return "call-depth-limit";
    case DynamicFallbackStopReason::Trap: return "trap";
    case DynamicFallbackStopReason::Syscall: return "syscall";
  }
  return "unknown";
}

struct DynamicFallbackObservation {
  GuestAddress entry{};
  GuestAddress site{};
  GuestAddress exit{};
  CompiledLookupKind kind{CompiledLookupKind::Branch};
  DynamicFallbackStopReason reason{DynamicFallbackStopReason::InvalidTarget};
  std::uint32_t instructions{};
  std::uint64_t block_fingerprint{};
};

struct DynamicFallbackConfig {
  // A missed edge should normally be a small thunk/function fragment. Hard
  // limits guarantee corrupted guest control flow cannot turn the safety net
  // into an unbounded interpreter loop.
  std::uint32_t max_instructions_per_dispatch{4096u};
  std::uint32_t max_nested_calls{64u};
};

class DynamicFallbackExecutor {
 public:
  using ObservationCallback =
      std::function<void(const DynamicFallbackObservation&)>;

  explicit DynamicFallbackExecutor(DynamicFallbackConfig config = {},
                                   ObservationCallback observer = {});

  void bind(ExecutionContext& context) noexcept;

  [[nodiscard]] DynamicFallbackResult try_execute(
      ExecutionContext& context, GuestAddress target, CompiledLookupKind kind);

  [[nodiscard]] std::uint64_t executed_blocks() const noexcept {
    return executed_blocks_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t executed_instructions() const noexcept {
    return executed_instructions_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t unsupported_instructions() const noexcept {
    return unsupported_instructions_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t source_invalidations() const noexcept {
    return source_invalidations_.load(std::memory_order_relaxed);
  }

 private:
  struct RunResult {
    DynamicFallbackResult public_result{};
    DynamicFallbackStopReason reason{DynamicFallbackStopReason::InvalidTarget};
    GuestAddress exit{};
    std::uint32_t instructions{};
    std::uint64_t fingerprint{};
  };

  [[nodiscard]] static DynamicFallbackResult callback(
      void* executor, ExecutionContext& context, GuestAddress target,
      CompiledLookupKind kind);
  [[nodiscard]] RunResult run(ExecutionContext& context, GuestAddress target,
                              CompiledLookupKind kind, std::uint32_t depth);

  DynamicFallbackConfig config_{};
  ObservationCallback observer_{};
  std::atomic<std::uint64_t> executed_blocks_{};
  std::atomic<std::uint64_t> executed_instructions_{};
  std::atomic<std::uint64_t> unsupported_instructions_{};
  std::atomic<std::uint64_t> source_invalidations_{};
};

}  // namespace xenon::cpu
