#pragma once

#include <cstdint>
#include <span>
#include <string>

#include "xenon/cpu/decoder.hpp"
#include "xenon/cpu/ir.hpp"
#include "xenon/cpu/lifter.hpp"

namespace xenon::cpu {

struct FunctionCompileResult {
  bool ok{};
  ir::Function function{};
  GuestAddress error_address{};
  std::uint32_t error_word{};
  std::string error{};
};

// One contiguous piece of a logical guest function. A function may own
// multiple non-contiguous ranges (FunctionChunk metadata) while still being
// compiled into one IR/CFG.
struct FunctionCodeRange {
  GuestAddress base{};
  std::span<const std::uint32_t> words{};
};

// Builds architecture-neutral Xenon IR for a statically-known guest function.
// Function boundaries come from the game package / analysis layer; this class
// intentionally does not discover executable memory or depend on the RAM model.
class StaticFunctionCompiler {
 public:
  [[nodiscard]] FunctionCompileResult compile(
      GuestAddress base, std::span<const std::uint32_t> words) const;

  // Sparse whole-function path used by FunctionChunk metadata. All supplied
  // ranges participate in one CFG and retain their original guest addresses.
  [[nodiscard]] FunctionCompileResult compile_ranges(
      GuestAddress entry, std::span<const FunctionCodeRange> ranges) const;

 private:
  Decoder decoder_{};
  Lifter lifter_{};
};

}  // namespace xenon::cpu
