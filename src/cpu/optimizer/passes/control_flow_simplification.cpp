// Function-level folding of branches on constant conditions.

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

OptimizationStats simplify_constant_control_flow(Function& function) {
  OptimizationStats stats{};
  if (function.blocks.empty()) return stats;

  for (auto& block : function.blocks) {
    std::map<ValueId, std::uint64_t> constants;
    for (const auto& instruction : block.instructions) {
      if (instruction.op == Op::Constant && instruction.result != kNoValue)
        constants[instruction.result] = instruction.imm0;
    }

    for (std::size_t index = 0; index < block.instructions.size();) {
      auto& instruction = block.instructions[index];
      if (instruction.op != Op::BranchIf || instruction.args.size() != 2u) {
        ++index;
        continue;
      }
      const auto condition = constants.find(instruction.args[0]);
      if (condition == constants.end()) {
        ++index;
        continue;
      }

      const bool taken = condition->second != 0u;
      const auto target_value = instruction.args[1];
      const auto target_constant = constants.find(target_value);
      const auto edge_kind = instruction.imm0 ? EdgeKind::Call : EdgeKind::Branch;

      if (taken) {
        instruction.op = instruction.imm0 ? Op::Call : Op::Branch;
        instruction.args = {target_value};
        instruction.imm0 = 0;
        instruction.imm1 = 0;
        if (edge_kind == EdgeKind::Branch) {
          block.successors.erase(
              std::remove_if(block.successors.begin(), block.successors.end(),
                  [&](const ControlFlowEdge& edge) {
                    if (edge.kind == EdgeKind::Fallthrough) return true;
                    if (target_constant != constants.end())
                      return edge.kind == EdgeKind::Branch &&
                             edge.target != static_cast<GuestAddress>(target_constant->second);
                    return false;
                  }),
              block.successors.end());
        }
        ++stats.branches_simplified;
        ++index;
      } else {
        if (target_constant != constants.end()) {
          const auto target = static_cast<GuestAddress>(target_constant->second);
          block.successors.erase(
              std::remove_if(block.successors.begin(), block.successors.end(),
                  [&](const ControlFlowEdge& edge) {
                    return edge.kind == edge_kind && edge.target == target;
                  }),
              block.successors.end());
        }
        block.instructions.erase(block.instructions.begin() +
                                 static_cast<std::ptrdiff_t>(index));
        ++stats.branches_simplified;
        ++stats.instructions_removed;
      }
    }
  }

  // Branch simplification may remove local incoming edges. Rebuild predecessor
  // metadata from the surviving CFG so later phases can trust it exactly.
  for (auto& block : function.blocks) block.predecessors.clear();
  std::map<GuestAddress, Block*> by_address;
  for (auto& block : function.blocks) by_address.emplace(block.guest_address, &block);
  for (const auto& source : function.blocks) {
    for (const auto& edge : source.successors) {
      if (!edge.local || edge.kind == EdgeKind::Call) continue;
      const auto target = by_address.find(edge.target);
      if (target == by_address.end()) continue;
      auto& predecessors = target->second->predecessors;
      if (std::find(predecessors.begin(), predecessors.end(), source.guest_address) ==
          predecessors.end())
        predecessors.push_back(source.guest_address);
    }
  }
  return stats;
}

}  // namespace xenon::cpu::ir
