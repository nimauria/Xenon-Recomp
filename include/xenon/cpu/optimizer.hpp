#pragma once

#include <cstddef>

#include "xenon/cpu/ir.hpp"

namespace xenon::cpu::ir {

struct OptimizationStats {
  std::size_t constants_folded{};
  std::size_t instructions_removed{};
};

class Optimizer {
 public:
  OptimizationStats run(Function& function) const;
  OptimizationStats run(Block& block) const;
};

}  // namespace xenon::cpu::ir
