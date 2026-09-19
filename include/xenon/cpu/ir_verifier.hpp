#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "xenon/cpu/ir.hpp"

namespace xenon::cpu::ir {

enum class VerifyError : std::uint8_t {
  None,
  EmptyFunction,
  MissingEntryBlock,
  DuplicateBlock,
  InvalidBlockRange,
  InvalidResult,
  DuplicateResult,
  UseBeforeDefinition,
  InvalidResultType,
  InvalidOperandType,
  InvalidArgumentCount,
  InvalidTerminator,
  InvalidBranchTarget,
  InvalidLocalEdge,
  InvalidPredecessor,
  InvalidMemoryMetadata,
  InvalidGuestSource,
  InvalidSideEffectOrdering,
};

struct VerifyResult {
  bool ok{};
  VerifyError error{VerifyError::None};
  GuestAddress block_address{};
  std::size_t instruction_index{};
  std::string message{};

  explicit operator bool() const noexcept { return ok; }
};

// Structural/type verifier for the architecture-neutral Xenon IR. Values are
// single-definition and block-local in CPU V2 Phase 4; function-wide SSA is
// intentionally deferred until profiling demonstrates that phi/merge machinery
// would materially improve generated code.
class Verifier {
 public:
  [[nodiscard]] VerifyResult verify(const Function& function) const;
  [[nodiscard]] VerifyResult verify(const Block& block) const;
};

}  // namespace xenon::cpu::ir
