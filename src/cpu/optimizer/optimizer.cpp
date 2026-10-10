#include "xenon/cpu/optimizer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>
#include <vector>

#include "cpu/optimizer/passes/optimizer_passes.hpp"

namespace xenon::cpu::ir {

using namespace detail;

std::string OptimizationReport::format() const {
  std::ostringstream out;
  out << "Function " << std::hex << std::uppercase << function << std::dec
      << "\n"
      << "guest instructions: " << guest_instructions << "\n"
      << "blocks: " << blocks_before << " -> " << blocks_after << "\n"
      << "IR: " << ir_before << " -> " << ir_after << "\n"
      << "constants folded: " << stats.constants_folded << "\n"
      << "algebraic simplifications: " << stats.algebraic_simplifications << "\n"
      << "copies propagated: " << stats.copies_propagated << "\n"
      << "CSE eliminated: " << stats.common_subexpressions_eliminated << "\n"
      << "dead IR eliminated: " << stats.dead_code_eliminated << "\n"
      << "state reads eliminated: " << stats.state_reads_eliminated << "\n"
      << "state writes deferred: " << stats.state_writes_deferred << "\n"
      << "CR reads eliminated: " << stats.cr_reads_eliminated << "\n"
      << "XER reads eliminated: " << stats.xer_reads_eliminated;
  return out.str();
}

OptimizationStats Optimizer::run(Block& block) const {
  OptimizationStats stats{};
  // Fixed deterministic pipeline. The second scalar cleanup wave consumes
  // aliases/constants exposed by architectural-state and CR/XER promotion.
  stats += fold_constants(block);
  stats += simplify_values(block);
  stats += eliminate_common_subexpressions(block);
  stats += eliminate_dead_code(block);
  stats += promote_architectural_state(block);
  stats += forward_cr_compare_state(block);
  stats += fold_constants(block);
  stats += simplify_values(block);
  stats += eliminate_common_subexpressions(block);
  stats += eliminate_dead_code(block);
  return stats;
}

OptimizationReport Optimizer::run_with_report(Function& function) const {
  OptimizationReport report{};
  report.function = function.guest_address;
  report.blocks_before = function.blocks.size();
  std::set<GuestAddress> guest_addresses;
  for (const auto& block : function.blocks) {
    report.ir_before += block.instructions.size();
    for (const auto& instruction : block.instructions)
      if (instruction.guest_opcode.valid()) guest_addresses.insert(instruction.guest_address);
  }
  report.guest_instructions = guest_addresses.size();
  for (auto& block : function.blocks) report.stats += run(block);
  report.stats += simplify_constant_control_flow(function);
  report.blocks_after = function.blocks.size();
  for (const auto& block : function.blocks) report.ir_after += block.instructions.size();
  return report;
}

OptimizationStats Optimizer::run(Function& function) const {
  return run_with_report(function).stats;
}

}  // namespace xenon::cpu::ir
