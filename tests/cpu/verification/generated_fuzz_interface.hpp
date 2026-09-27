#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "xenon/cpu/semantic_verifier.hpp"

namespace xenon::cpu::verify::test_corpus {

struct GeneratedFuzzInstruction {
  GuestAddress address{};
  std::uint32_t word{};
};

struct GeneratedFuzzCase {
  const char* name{};
  const GeneratedFuzzInstruction* instructions{};
  std::size_t instruction_count{};
  CompiledEntry candidate{};
  bool memory_setup{};
};

[[nodiscard]] std::span<const GeneratedFuzzCase>
generated_semantic_fuzz_cases() noexcept;

}  // namespace xenon::cpu::verify::test_corpus
