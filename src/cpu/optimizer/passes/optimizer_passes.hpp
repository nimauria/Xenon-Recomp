#pragma once

// Block- and function-level IR optimization passes run by Optimizer
// (src/cpu/optimizer/optimizer.cpp) in a fixed, deterministic order.
// Private to xenon_cpu.

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "xenon/cpu/optimizer.hpp"

namespace xenon::cpu::ir {
namespace detail {

inline std::size_t max_value_id(const Block& block) noexcept {
  std::size_t maximum = 0;
  bool any = false;
  for (const auto& i : block.instructions) {
    if (i.result != kNoValue) {
      maximum = std::max(maximum, static_cast<std::size_t>(i.result));
      any = true;
    }
    for (const auto arg : i.args) {
      if (arg != kNoValue) {
        maximum = std::max(maximum, static_cast<std::size_t>(arg));
        any = true;
      }
    }
  }
  return any ? maximum : 0;
}

// passes/value_simplification.cpp
OptimizationStats fold_constants(Block& block);
OptimizationStats simplify_values(Block& block);
// passes/redundancy_elimination.cpp
OptimizationStats eliminate_common_subexpressions(Block& block);
OptimizationStats eliminate_dead_code(Block& block);
// passes/state_promotion.cpp
OptimizationStats promote_architectural_state(Block& block);
OptimizationStats forward_cr_compare_state(Block& block);

}  // namespace detail

// passes/control_flow_simplification.cpp
OptimizationStats simplify_constant_control_flow(Function& function);

}  // namespace xenon::cpu::ir
