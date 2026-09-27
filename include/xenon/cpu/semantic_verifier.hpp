#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/flat_memory.hpp"
#include "xenon/cpu/runtime.hpp"

namespace xenon::cpu::verify {

// Gen 8 intentionally validates generated/native execution against an
// independent reference implementation.  Never point the reference side at
// DynamicFallbackExecutor: agreement between two paths sharing the same
// implementation is not semantic verification.
using CompiledEntry = ExecutionResult (*)(CpuState&, MemoryPort&, RuntimeServices&);

class DeterministicRng {
 public:
  explicit DeterministicRng(std::uint64_t seed) noexcept;

  [[nodiscard]] std::uint64_t next_u64() noexcept;
  [[nodiscard]] std::uint32_t next_u32() noexcept {
    return static_cast<std::uint32_t>(next_u64());
  }
  [[nodiscard]] std::uint8_t next_u8() noexcept {
    return static_cast<std::uint8_t>(next_u64());
  }

 private:
  std::uint64_t state_{};
};

enum class ExceptionClass : std::uint8_t {
  None,
  OutOfRange,
  Logic,
  Runtime,
  Standard,
  Unknown,
};

struct CapturedOutcome {
  ExceptionClass exception{ExceptionClass::None};
  std::string exception_text{};
  ExecutionResult result{};
};

using CaseSetup = void (*)(CpuState&, FlatMemory&, DeterministicRng&);

struct SemanticCase {
  std::string name{};
  std::vector<DecodedInstruction> instructions{};
  CompiledEntry candidate{};
  CaseSetup setup{};
};

struct VerificationConfig {
  // Fixed by default so a CI failure is exactly replayable locally.
  std::uint64_t seed{0x58454E4F4E47454Eull}; // "XENONGEN"
  std::size_t trials{128};
  GuestAddress memory_base{0x00010000u};
  std::size_t memory_size{0x1000u};
  bool minimize_failure{true};
  std::size_t max_minimization_steps{256};
  std::filesystem::path repro_directory{};
};

struct SemanticMismatch {
  std::string case_name{};
  std::size_t trial_index{};
  std::uint64_t trial_seed{};
  std::string summary{};
  std::string first_state_difference{};
  std::optional<GuestAddress> first_memory_difference{};
  CapturedOutcome expected{};
  CapturedOutcome actual{};
  CpuState minimized_state{};
  std::vector<std::uint8_t> minimized_memory{};
  std::filesystem::path repro_path{};
};

struct VerificationReport {
  std::size_t trials_run{};
  std::optional<SemanticMismatch> mismatch{};

  [[nodiscard]] bool ok() const noexcept { return !mismatch.has_value(); }
};

// Deliberately small, independent PowerPC semantic model used only as an
// oracle for Gen 8.  Coverage grows instruction family by instruction family;
// unsupported instructions fail closed rather than silently delegating to AOT
// helpers or the Gen 7 fallback executor.
class ReferenceExecutor {
 public:
  [[nodiscard]] static bool supports(const DecodedInstruction& instruction) noexcept;

  [[nodiscard]] static ExecutionResult execute_one(
      const DecodedInstruction& instruction, CpuState& state, MemoryPort& memory,
      RuntimeServices& runtime);

  [[nodiscard]] static ExecutionResult execute_block(
      std::span<const DecodedInstruction> instructions, CpuState& state,
      MemoryPort& memory, RuntimeServices& runtime);
};

class SemanticVerifier {
 public:
  [[nodiscard]] VerificationReport run(
      const SemanticCase& semantic_case,
      const VerificationConfig& config = {}) const;

  // Replays one exact randomized trial without walking the preceding trial
  // sequence.  This is what repro artifacts record.
  [[nodiscard]] VerificationReport replay(
      const SemanticCase& semantic_case, std::uint64_t trial_seed,
      const VerificationConfig& config = {}) const;
};

[[nodiscard]] const char* exception_class_name(ExceptionClass value) noexcept;

}  // namespace xenon::cpu::verify
