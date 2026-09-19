#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "xenon/cpu/ir.hpp"

namespace xenon::cpu::ir {

struct OptimizationStats {
  std::size_t constants_folded{};
  std::size_t instructions_removed{};
  std::size_t state_reads_eliminated{};
  std::size_t state_writes_deferred{};
  std::size_t state_materializations{};
  std::size_t xer_reads_eliminated{};
  std::size_t xer_writes_deferred{};
  std::size_t cr_reads_eliminated{};
  std::size_t cr_writes_deferred{};
  std::size_t algebraic_simplifications{};
  std::size_t copies_propagated{};
  std::size_t common_subexpressions_eliminated{};
  std::size_t dead_code_eliminated{};
  std::size_t comparisons_simplified{};
  std::size_t selects_simplified{};
  std::size_t extensions_eliminated{};
  std::size_t branches_simplified{};

  OptimizationStats& operator+=(const OptimizationStats& other) noexcept {
    constants_folded += other.constants_folded;
    instructions_removed += other.instructions_removed;
    state_reads_eliminated += other.state_reads_eliminated;
    state_writes_deferred += other.state_writes_deferred;
    state_materializations += other.state_materializations;
    xer_reads_eliminated += other.xer_reads_eliminated;
    xer_writes_deferred += other.xer_writes_deferred;
    cr_reads_eliminated += other.cr_reads_eliminated;
    cr_writes_deferred += other.cr_writes_deferred;
    algebraic_simplifications += other.algebraic_simplifications;
    copies_propagated += other.copies_propagated;
    common_subexpressions_eliminated += other.common_subexpressions_eliminated;
    dead_code_eliminated += other.dead_code_eliminated;
    comparisons_simplified += other.comparisons_simplified;
    selects_simplified += other.selects_simplified;
    extensions_eliminated += other.extensions_eliminated;
    branches_simplified += other.branches_simplified;
    return *this;
  }
};

struct OptimizationReport {
  GuestAddress function{};
  std::size_t guest_instructions{};
  std::size_t blocks_before{};
  std::size_t blocks_after{};
  std::size_t ir_before{};
  std::size_t ir_after{};
  OptimizationStats stats{};

  [[nodiscard]] std::string format() const;
};

class Optimizer {
 public:
  OptimizationStats run(Function& function) const;
  OptimizationStats run(Block& block) const;
  [[nodiscard]] OptimizationReport run_with_report(Function& function) const;
};

}  // namespace xenon::cpu::ir
